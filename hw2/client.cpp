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
#include "opencv2/opencv.hpp"

#define BUFF_SIZE 1024
#define ERR_EXIT(buf) {perror(buf); exit(1);}

using namespace std;
using namespace cv;

void send_file(FILE *fp, const int localSocket) {
    char buf[BUFF_SIZE] = {};
    
    /* send file size */
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char file_size[BUFF_SIZE] = {};
    sprintf(file_size, "%d", size);
    send(localSocket, file_size, BUFF_SIZE, 0);
    
    int sent;
    while(fread(buf, 1, min(BUFF_SIZE, size), fp) > 0) { 
        if( (sent = send(localSocket, buf, sizeof(char) * BUFF_SIZE, 0)) <= 0) {
            cout << "send failed" << endl;
        }
        size -= BUFF_SIZE;
        if(size <= 0) break;
        memset(buf, 0, sizeof(buf));
    }
}

void recv_file(char *filename, const int localSocket) {
    char file_size[BUFF_SIZE] = {};
    recv(localSocket, file_size, BUFF_SIZE, 0);
    int size;
    sscanf(file_size, "%d", &size);
    cout << "size: " << size << endl;


    char buf[BUFF_SIZE] = {};
    FILE *fp = fopen(filename, "wb");
    while(1) {
        memset(buf, 0, sizeof(buf));
        int rec = recv(localSocket, buf, BUFF_SIZE, 0);
        cout << rec << endl;
        if(fwrite(buf, 1, min(rec, size), fp) <= 0)
            cout << "error" << endl;
        fflush(fp); 
        size -= rec;
        if(size <= 0) break;     
    }
}
void play_video(char *filename, const int localSocket) {
    Mat imgClient;
    
    int width, height;
    unsigned int elemSize;
    char preprocBuffer[BUFF_SIZE];
    recv(localSocket, preprocBuffer, BUFF_SIZE, 0);
    sscanf(preprocBuffer, "%d %d %u", &height, &width, &elemSize);
    
    const int imgSize = width * height * elemSize;

    imgClient = Mat::zeros(height, width, CV_8UC3);
    if(!imgClient.isContinuous()){
         imgClient = imgClient.clone();
    }
    // recv image size
    char sig_close[BUFF_SIZE] = "ESC";
    char sig_play[BUFF_SIZE] = "ACK";
    char isEof[BUFF_SIZE];
    while(1) {
        // allocate a buffer to load the frame (there would be 2 buffers in the world of the Internet)
        uchar *buffer = (uchar *)malloc(sizeof(uchar) * imgSize);
        memset(buffer, 0, sizeof(uchar) * imgSize);

        /* receive eof */
        memset(isEof, 0, sizeof(isEof));
        recv(localSocket, isEof, BUFF_SIZE, 0);
        if(strcmp(isEof, "EOF") == 0) 
            break;

        /* data transmitting */
        int total_recv = recv(localSocket, buffer, imgSize, 0);
        if(total_recv == 0) break;
        while(total_recv < imgSize) {
            int recv_size = 0;
            recv_size = recv(localSocket, buffer + total_recv, imgSize - total_recv, 0);
            total_recv += recv_size;
        }
        memcpy(imgClient.data, buffer, imgSize);

        free(buffer);
        imshow("Video", imgClient);
        char c = (char)waitKey(33.3333);
        if(c==27) {
            send(localSocket, sig_close, BUFF_SIZE, 0);
            break;
        } 
        /* ACK */
        send(localSocket, sig_play, BUFF_SIZE, 0);
    }
    destroyAllWindows();
}

int main(int argc , char *argv[])
{
    if(argc != 2) {
        ERR_EXIT("argument number is incorrect!");
    }
    mkdir("client_dir", 0777);
    chdir("client_dir");

    char ip[128] = {};
    int port = 0;

    char *semi = strstr(argv[1], ":");
    strncpy(ip, argv[1], semi-argv[1]);
    sscanf(semi+1, "%d", &port);

    int localSocket, recved;
    localSocket = socket(AF_INET , SOCK_STREAM , 0);

    if (localSocket == -1){
        printf("Fail to create a socket.\n");
        return 0;
    }

    struct sockaddr_in info;
    bzero(&info,sizeof(info));

    info.sin_family = PF_INET;
    info.sin_addr.s_addr = inet_addr(ip);
    info.sin_port = htons(port);


    int err = connect(localSocket,(struct sockaddr *)&info,sizeof(info));
    if(err==-1){
        printf("Connection error\n");
        return 0;
    }
    char receiveMessage[BUFF_SIZE] = {};
    int sent;
    char sendMessage[BUFF_SIZE] = {};
    while(1){
        bzero(receiveMessage,sizeof(char)*BUFF_SIZE);
        bzero(sendMessage, sizeof(char)*BUFF_SIZE);
        string str_sendMessage;

        cout << "$ ";

        getline(cin, str_sendMessage);
        strcpy(sendMessage, str_sendMessage.c_str());

        char cmd[BUFF_SIZE] = {};
        char filename[BUFF_SIZE] = {};
        // cout << sendMessage << endl;
        int r = sscanf(sendMessage, "%s %s", cmd, filename);
        
        if(strcmp(cmd, "ls") == 0 && r == 1) {
            if((sent = send(localSocket, sendMessage, sizeof(char)*BUFF_SIZE, 0)) < 0) {
                cout << "sent failed, with sent bytes = " << sent << endl;
                break;
            }
            if ((recved = recv(localSocket,receiveMessage,sizeof(char)*BUFF_SIZE,0)) < 0){
                cout << "recv failed, with received bytes = " << recved << endl;
                break;
            }
            else if (recved == 0){
                cout << "<end>\n";
                break;
            }
            printf("%s", receiveMessage);
        
        } else {
            
            if(r == 1) {
                cout << "Command format error." << endl;
                continue;
            }
            
            if(strcmp(cmd, "put") == 0) {
                FILE *fp = fopen(filename, "rb");
                if(fp == NULL) {
                    cout << "The \'" << filename << "\' doesn't exist." << endl;
                    continue;
                }
                if((sent = send(localSocket, sendMessage, sizeof(char)*BUFF_SIZE, 0)) < 0) {
                    cout << "sent failed, with sent bytes = " << sent << endl;
                }
                // send(localSocket, filename, sizeof(char) * strlen(filename), 0);
                send_file(fp, localSocket);
            } else if(strcmp(cmd, "get") == 0) {
                if((sent = send(localSocket, sendMessage, sizeof(char) * BUFF_SIZE, 0)) < 0) {
                    cout << "sent failed, with sent bytes = " << sent << endl;
                }
                char state[BUFF_SIZE] = {};
                recv(localSocket, state, BUFF_SIZE, 0);
                if(strcmp(state, "OK") != 0) {
                    cout << "The \'" << filename << "\' doesn't exist." << endl;
                    continue;
                }
                recv_file(filename, localSocket);
            } else if(strcmp(cmd, "play") == 0) {
                if((sent = send(localSocket, sendMessage, sizeof(char) * BUFF_SIZE, 0)) < 0) {
                    cout << "sent failed, with sent bytes = " << sent << endl;
                }
                // handle state
                char state[BUFF_SIZE] = {};
                recv(localSocket, state, BUFF_SIZE, 0);
                // cout << state << endl;
                if(strcmp(state, "OK") == 0) {
                    play_video(filename, localSocket);
                } else {
                    cout << state << endl;
                }
            } else {
                cout << "Command not found." << endl;
            }
        }
    }
    printf("close Socket\n");
    close(localSocket);
    return 0;
}

