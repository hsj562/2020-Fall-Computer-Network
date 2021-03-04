#include <iostream>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <deque>
#include "opencv2/opencv.hpp"

#define ERR_EXIT(msg) {perror(msg); exit(1);}
#define DATA_SIZE 4096
#define TIMEOUT 1000
#define THRESH 16
using namespace std;
using namespace cv;

typedef struct {
	int length;
	int seqNumber;
	int ackNumber;
	int fin;
	int syn;
	int ack;
} header;

typedef struct{
	header head;
	uchar data[DATA_SIZE];
} segment;

void setIP(char *dst, char *src) {
    if(strcmp(src, "0.0.0.0") == 0 || strcmp(src, "local") == 0
            || strcmp(src, "localhost")) {
        sscanf("127.0.0.1", "%s", dst);
    } else {
        sscanf(src, "%s", dst);
    }
}

bool checkFileExist(char *filename);

deque<segment> buffer;
int window_size = 1;
int THRESHOLD = THRESH;
int seqNo = 1;
int sentMax = 0;

void procPacket(const int len, uchar data[], bool fin) {
    segment pkt;
    /* process packet */
    pkt.head.length = len;
    pkt.head.seqNumber = seqNo++;
    pkt.head.ackNumber = 0;
    pkt.head.fin = fin? 1:0;
    pkt.head.syn = 0;
    pkt.head.ack = 0;
    memcpy(pkt.data, data, len);
    
    /* put into buffer */
    buffer.push_back(pkt);
}

void procTimeout() {
    THRESHOLD = max(window_size/2, 1);
    window_size = 1;
    cout << "time    out,           threshold = " << THRESHOLD << endl;
}
void procWindowSize() {
    if (window_size >= THRESHOLD) {
        window_size++;
    } else {
        window_size *= 2;
    }
}

int main(int argc, char** argv){

    if(argc != 5) {
        fprintf(stderr, "用法: %s <sender port> <agent ip> <agent port> <file path>\n", argv[0]);
        fprintf(stderr, "例如: ./server 8887 127.0.0.1 8888 ./tmp.mpg\n");
        ERR_EXIT("argument number is incorrect!");
    }

    /* init server */
    int localSocket, port = atoi(argv[1]);   
    char agentIP[128];
    setIP(agentIP, argv[2]);
    int agentPort = atoi(argv[3]);
    char filePath[128];
    sscanf(argv[4], "%s", filePath);

    if(!checkFileExist(filePath)) {
        char err_msg[128];
        sprintf(err_msg, "The \'%s\' doesn't exist.", filePath);
        ERR_EXIT(err_msg);
    }

    struct sockaddr_in localAddr, remoteAddr;
    bzero(&localSocket,sizeof(localSocket));
    bzero(&remoteAddr,sizeof(remoteAddr));

    // int addrLen = sizeof(struct sockaddr_in);  

    localSocket = socket(AF_INET , SOCK_DGRAM , 0);

    if (localSocket == -1){
        ERR_EXIT("socket() call failed!!");
    }
    const char localIP[] = "127.0.0.1";

    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = inet_addr(localIP);
    localAddr.sin_port = htons(port);

    if( bind(localSocket,(struct sockaddr *)&localAddr , sizeof(localAddr)) < 0) {
        ERR_EXIT("Can't bind() socket");
    }
    
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_addr.s_addr = inet_addr(agentIP);
    remoteAddr.sin_port = htons(agentPort);
    
    socklen_t tmp_size = sizeof(remoteAddr);

    /* init imgServer */
    Mat imgServer;
    VideoCapture cap(filePath);

    // get the resolution of the video

    int width = cap.get(CV_CAP_PROP_FRAME_WIDTH);
    int height = cap.get(CV_CAP_PROP_FRAME_HEIGHT);
    // cout << width << ", " << height << endl;

    // allocate container to load frames
    imgServer = Mat::zeros(height, width, CV_8UC3);    

    /* preprocess frameSize */
    char preprocBuffer[DATA_SIZE] = {};
    sprintf(preprocBuffer, "%d %d %lu", height, width, imgServer.elemSize());
    // cout << "height: " << height << " width: " << width << " elemSize: " << imgServer.elemSize() << endl;
    procPacket(strlen(preprocBuffer), (uchar *)preprocBuffer, false);
    
    /* set timeout */
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT;
    if(setsockopt(localSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ERR_EXIT("set timeout error!");
    }

    /* send frameSize */
    while(1) {
        sendto(localSocket, &buffer[0], sizeof(segment), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
        cout << "send    data    #1,    winSize = " << window_size << endl;
        segment ACK;
        if (recvfrom(localSocket, &ACK, sizeof(ACK), 0, (struct sockaddr *)&remoteAddr, &tmp_size) < 0) {
            /* timeout */
            procTimeout();
            continue;
        }
        if (ACK.head.ack == 1) {
            cout << "recv   ack    #1" << endl;
            buffer.pop_front();
            procWindowSize();
            break;
        }
    }

    // ensure the memory is continuous (for efficiency issue.)
    if(!imgServer.isContinuous()){
        imgServer = imgServer.clone();
    }
    bool getData = true;
    /* critical region */
    while(1) {
        /* check window size and buffer size */
        if (getData && buffer.size() < window_size) {
            //get a frame from the video to the container on server.
            cap >> imgServer;
            
            // get the size of a frame in bytes 
            int imgSize = imgServer.total() * imgServer.elemSize();
            if (imgSize <= 0) {
                getData = false;
            } else {
                // allocate a buffer to load the frame (there would be 2 buffers in the world of the Internet)
                uchar *head = (uchar *)malloc(sizeof(uchar) * imgSize);
                uchar *ptr = head;
                memset(ptr, 0, sizeof(uchar) * imgSize);
                memcpy(ptr, imgServer.data, imgSize);

                while(imgSize > 0) {
                    uchar tmp[DATA_SIZE] = {};
                    int getSize = min(DATA_SIZE, imgSize);
                    memcpy(tmp, ptr, getSize);    

                    // copy a frame to the buffer
                    procPacket(getSize, tmp, false);
                    imgSize -= getSize;
                    ptr += getSize;
                }
                free(head);
            }
        }
        /* send packet */
        char op[16] = {};
        int bound = min(window_size, (int)buffer.size());
        // cout << "bound: " << bound << endl; 
        for (int i = 0; i < bound; ++i) {
            sendto(localSocket, &buffer[i], sizeof(segment), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));

            if (buffer[i].head.seqNumber < sentMax) 
                sprintf(op, "resnd   ");
            else {
                sentMax = buffer[i].head.seqNumber;
                sprintf(op, "send    ");
            }

            
            cout << op << "data    #" << buffer[i].head.seqNumber << ",    winSize = " << window_size << endl; 
        }
        /* recv ACK */
        bool isTimeout = false;
        int recvd = 0;
        while (1) {
            segment ACK;
            if (recvfrom(localSocket, &ACK, sizeof(segment), 0, (struct sockaddr *)&remoteAddr, &tmp_size) < 0) {
                /* timeout */
                isTimeout = true;
                break;
            } else {
                assert(ACK.head.ack == 1);
                // cout << "ack num: " << ACK.head.ackNumber << endl;
                if (!buffer.empty() && buffer.front().head.seqNumber == ACK.head.ackNumber) {
                    cout << "recv    ack     #" << buffer.front().head.seqNumber << endl;
                    buffer.pop_front();
                    recvd++;
                }
            }
            if (recvd == bound) break; 
        }   
        // cout << "winSize: " << window_size << ", recvd: " << recv << endl; 
        /* check finish */
        if (buffer.empty() && !getData) {
            break;
        }
        // if (c++ > 120) exit(0);
        /* check timeout */
        if (isTimeout) 
            procTimeout();
        else
            procWindowSize();
    }

    /* unset timeout */
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    if(setsockopt(localSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ERR_EXIT("set timeout error!");
    }
    procPacket(0, NULL, 1);
    sendto(localSocket, &buffer[0], sizeof(segment), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
    cout << "send    fin" << endl;
    segment FINACK;
    recvfrom(localSocket, &FINACK, sizeof(segment), 0, (struct sockaddr *)&remoteAddr, &tmp_size);

    while (FINACK.head.fin != 1)
        recvfrom(localSocket, &FINACK, sizeof(segment), 0, (struct sockaddr *)&remoteAddr, &tmp_size);
    assert(FINACK.head.fin == 1 && FINACK.head.ack == 1);
    cout << "recv    finack" << endl;

    cap.release();
    return 0;
}

bool checkFileExist(char *filename) {
    return (fopen(filename, "rb") != NULL);
}