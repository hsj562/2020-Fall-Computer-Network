#include <iostream>
#include <sys/socket.h> 
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include "opencv2/opencv.hpp"

#define DATA_SIZE 4096
#define MAX_BUFSIZ 32
#define ERR_EXIT(buf) {perror(buf); exit(1);}

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

segment buffer[MAX_BUFSIZ];
int nextSeqNo = 1;
int curBufferSize = 0;
int imgSize = -1;

int curImgSize = 0;
Mat imgClient;

void setIP(char *dst, char *src) {
    if(strcmp(src, "0.0.0.0") == 0 || strcmp(src, "local") == 0
            || strcmp(src, "localhost")) {
        sscanf("127.0.0.1", "%s", dst);
    } else {
        sscanf(src, "%s", dst);
    }
}

void bufferFlush() {
    // assert(curBufferSize == MAX_BUFSIZ);
    int first = 0;
    if (buffer[0].head.seqNumber == 1) {
        int height, width;
        unsigned int eleSize;
        sscanf((char *)buffer[0].data, "%d %d %u", &height, &width, &eleSize);
        imgSize = height * width * eleSize;

        // cout << height << " " << width << " " << eleSize << endl;
        imgClient = Mat::zeros(height, width, CV_8UC3);

        if(!imgClient.isContinuous())
            imgClient = imgClient.clone();
        first = 1;
    }
    // cout << "curBufferSize: " << curBufferSize << endl;
    for (int i = first; i < curBufferSize; ++i) {
        memcpy(imgClient.data + curImgSize, buffer[i].data, buffer[i].head.length);
        curImgSize += buffer[i].head.length;
        // cout << "curImgSize: " << curImgSize << endl;
        if (curImgSize == imgSize) {
            /* play */
            imshow("Video", imgClient);
            waitKey(1);
            curImgSize = 0;
        }
    }
    curBufferSize = 0;
    cout << "flush" << endl;
}
void procACK(segment *ACK, int ackNum, bool fin) {
    ACK->head.ack = 1;
    ACK->head.syn = 0;
    ACK->head.fin = fin? 1 : 0;
    ACK->head.seqNumber = 0;
    ACK->head.ackNumber = ackNum;
}
int main(int argc , char *argv[])
{
    if(argc != 4) {
        fprintf(stderr, "用法: %s <receiver port> <agent ip> <agent port>\n", argv[0]);
        fprintf(stderr, "例如: ./receiver 8887 127.0.0.1 8888\n");
        ERR_EXIT("argument number is incorrect!");
    }

    int localSocket, port = atoi(argv[1]);
    localSocket = socket(AF_INET , SOCK_DGRAM , 0);

    if (localSocket == -1){
        ERR_EXIT("fail to create socket");
    }

    struct sockaddr_in localAddr, remoteAddr;
    bzero(&localAddr,sizeof(localAddr));
    bzero(&remoteAddr,sizeof(remoteAddr));

    const char localIP[] = "127.0.0.1";
    
    localAddr.sin_family = PF_INET;
    localAddr.sin_addr.s_addr = inet_addr(localIP);
    localAddr.sin_port = htons(port);

    int addrLen = sizeof(struct sockaddr_in);  

    char agentIP[1024] = {};
    setIP(agentIP, argv[2]);
    int agentPort = atoi(argv[3]);

    remoteAddr.sin_family = PF_INET;
    remoteAddr.sin_addr.s_addr = inet_addr(agentIP);
    remoteAddr.sin_port = htons(agentPort);

    socklen_t tmp_size = sizeof(remoteAddr);


    if( bind(localSocket,(struct sockaddr *)&localAddr , sizeof(localAddr)) < 0) {
        ERR_EXIT("Can't bind() socket");
    } 
    // struct timeval timeout;
    // timeout.tv_sec = 0;
    // timeout.tv_usec = 1000;
    // setsockopt(localSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(&timeout));
    while(1) {
        /* recv data */
        segment seg;
        recvfrom(localSocket, &seg, sizeof(segment), 0, (struct sockaddr *)&remoteAddr, &tmp_size);
        if (seg.head.fin == 1) 
            cout << "recv    fin" << endl;
        else 
            cout << "recv    data    #" << seg.head.seqNumber << endl;

        if (seg.head.fin == 1) {
            segment FINACK;
            procACK(&FINACK, nextSeqNo, true);    
            sendto(localSocket, &FINACK, sizeof(segment), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
            cout << "send    finack" << endl;
            bufferFlush();
            break;
        } else if (seg.head.seqNumber == nextSeqNo) {
            if (curBufferSize == MAX_BUFSIZ) {
                /* drop and flush */
                segment ACK;
                procACK(&ACK, nextSeqNo-1, false);
                sendto(localSocket, &ACK, sizeof(segment), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
                cout << "drop    data    #" << seg.head.seqNumber << endl;
                cout << "send    ack     #" << nextSeqNo-1 << endl;
                bufferFlush();
            } else {
                /* append to buffer */
                buffer[curBufferSize++] = seg;
                segment ACK;
                procACK(&ACK, nextSeqNo, false);
                sendto(localSocket, &ACK, sizeof(segment), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
                cout << "send    ack     #" << nextSeqNo << endl;
                nextSeqNo++;
            }
        } else {
            /* drop */
            segment ACK;
            procACK(&ACK, nextSeqNo-1, false);
            sendto(localSocket, &ACK, sizeof(segment), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
            cout << "drop    data    #" << seg.head.seqNumber << endl;
            cout << "send    ack     #" << nextSeqNo-1 << endl;
        }
    }
    // destroyAllWindows();
    return 0;
}
