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
#include <queue>
#include "opencv2/opencv.hpp"

#define BUFF_SIZE 1024
#define MAX_CLIENT 20
#define ERR_EXIT(msg) {perror(msg); exit(1);}

using namespace std;
using namespace cv;

typedef struct {
    unsigned int port;
    int listen_fd;
} Server;

typedef struct {
    int conn_fd;
    int cmd; 
    /**
        -1: error
        0: no cmd
        
        / get command /
        1: ls
        2: put
        3: get
        4: play

        / handling /
        5: recv
        6: send
        7: play
    **/
   char filename[BUFF_SIZE];
   int file_size;
   FILE *fp;
   VideoCapture cap;
   Mat imgServer;
   queue<uchar> video_buffer;
} Request;



bool checkFileExist(char *filename, const int remoteSocket);
void recv_file(Request *request);
void send_file(Request *request);
void play_video(Request *request);

void ls_cmd(const int remoteSocket) {
    struct dirent **namelist;
    int n;
    int sent;
    char sendMessage[BUFF_SIZE] = {};
    if((n = scandir(".", &namelist, NULL, alphasort)) == -1) {
        cout << "scandir error" << endl;
    }
    for(int i = 0; i < n; ++i) {
        if(strcmp(namelist[i]->d_name, ".") == 0 || strcmp(namelist[i]->d_name, "..") == 0) {
            free(namelist[i]);
            continue;
        }
        strcat(sendMessage, namelist[i]->d_name);
        strcat(sendMessage, "\n");
        free(namelist[i]);
    }
    free(namelist);
    // cout << sendMessage << endl;
    if((sent = send(remoteSocket, sendMessage, BUFF_SIZE, 0)) < 0) {
        cout << "send failed, with sent bytes = " << sent << endl;
    }
}
void put_cmd(Request *request) {
    request->fp = fopen(request->filename, "wb"); 
    /* recv file size */
    char buf[BUFF_SIZE] = {};
    recv(request->conn_fd, buf, BUFF_SIZE, 0);
    sscanf(buf, "%d", &request->file_size);
    // cout << request->file_size << endl;

    recv_file(request);     
}

void get_cmd(Request *request) {
    request->fp = fopen(request->filename, "rb");
    char state[BUFF_SIZE] = {};

    /* send state */
    if(request->fp == NULL) {
        sprintf(state, "The \'%s\' doesn't exist.", request->filename);
        send(request->conn_fd, state, BUFF_SIZE, 0);
        request->cmd = 0;
        return;
    } else {
        strcpy(state, "OK");
        send(request->conn_fd, state, BUFF_SIZE, 0);
    }

    /* send file size */
    fseek(request->fp, 0, SEEK_END);
    request->file_size = ftell(request->fp);
    fseek(request->fp, 0, SEEK_SET);

    char file_size[BUFF_SIZE] = {};
    sprintf(file_size, "%d", request->file_size);
    send(request->conn_fd, file_size, BUFF_SIZE, 0);

    /* send data */
    send_file(request);
}

void play_cmd(Request *request) {
    Mat imgServer;

    /* check mpg file */
    int len = strlen(request->filename);
    char ext[8] = {};
    char *ptr = &request->filename[len-4];
    strncpy(ext, ptr, 4);
    char state[BUFF_SIZE] = {};

    if(!checkFileExist(request->filename, request->conn_fd)) {
        request->cmd = 0;
        return;
    } else if(strcmp(ext, ".mpg") != 0) {   
        sprintf(state, "The \'%s\' is not a mpg file.", request->filename);     
        send(request->conn_fd, state, BUFF_SIZE, 0);
        request->cmd = 0;
        return;
    } else {
        strcpy(state, "OK");
        send(request->conn_fd, state, BUFF_SIZE, 0);
    }
    VideoCapture cap(request->filename);

    request->cap = cap;

    // get the resolution of the video

    int width = cap.get(CV_CAP_PROP_FRAME_WIDTH);
    int height = cap.get(CV_CAP_PROP_FRAME_HEIGHT);
    // cout << width << ", " << height << endl;

    // allocate container to load frames
    imgServer = Mat::zeros(height, width, CV_8UC3);    

    char preprocBuffer[BUFF_SIZE];
    sprintf(preprocBuffer, "%d %d %lu", height, width, imgServer.elemSize());
    send(request->conn_fd, preprocBuffer, BUFF_SIZE, 0);

    // ensure the memory is continuous (for efficiency issue.)
    if(!imgServer.isContinuous()){
        imgServer = imgServer.clone();
    }
    
    request->imgServer = imgServer;

    play_video(request);
}

void recv_file(Request *request) {
    char buf[BUFF_SIZE] = {};
    int rec = recv(request->conn_fd, buf, sizeof(char)*BUFF_SIZE, 0);
    // cout << rec << endl;
    fwrite(buf, 1, min(request->file_size, rec), request->fp);
    fflush(request->fp);
    request->file_size -= rec;
    if(request->file_size <= 0) {
        request->cmd = 0;
    } else {
        request->cmd = 5;
    }
}

void send_file(Request *request) {
    char buf[BUFF_SIZE] = {};
    int sent;
    int res = fread(buf, 1, min(BUFF_SIZE, request->file_size), request->fp);
    cout << res << endl;
    if((sent = send(request->conn_fd, buf, sizeof(char) * BUFF_SIZE, 0) <= 0)) {
        cout << "send failed" << endl;
    }
    // cout << buf << endl;
    request->file_size -= BUFF_SIZE;
    if(request->file_size <= 0) {
        request->cmd = 0;
    } else {
        request->cmd = 6;
    }
}

void play_video(Request *request) {

    //get a frame from the video to the container on server.
    request->cap >> request->imgServer;
    
    // get the size of a frame in bytes 
    int imgSize = request->imgServer.total() * request->imgServer.elemSize();

    /* check file EOF */
    char eof[BUFF_SIZE] = "EOF";
    char nEof[BUFF_SIZE] = "NEOF";
    if(imgSize <= 0) {
        send(request->conn_fd, eof, BUFF_SIZE, 0);

        request->cap.release();
        request->cmd = 0;
        return;
    } else {
        send(request->conn_fd, nEof, BUFF_SIZE, 0);
        request->cmd = 7;
    }
    
    // allocate a buffer to load the frame (there would be 2 buffers in the world of the Internet)
    uchar *ptr = (uchar *)malloc(sizeof(uchar) * imgSize);
    // uchar buffer[imgSize];
    memset(ptr, 0, sizeof(uchar) * imgSize);
    // copy a frame to the buffer
    memcpy(ptr, request->imgServer.data, imgSize);
    int sent;
    send(request->conn_fd, ptr, imgSize, 0);

    free(ptr);
    /* recv ack */
    char sig[BUFF_SIZE] = {};
    recv(request->conn_fd, sig, BUFF_SIZE, 0);
    // cout << sig << endl;
    if(strcmp(sig, "ESC") == 0) {
        request->cap.release();
        request->cmd = 0;
        return;
    }
    
}
Request requests[MAX_CLIENT];
Server svr;
fd_set r_working, w_working, r_master, w_master;

void handle_read(Request *requests);

int main(int argc, char** argv){

    if(argc != 2) {
        ERR_EXIT("argument number is incorrect!");
    }

    mkdir("server_dir", 0777);
    chdir("server_dir");

    /* init server */
    int localSocket, remoteSocket, port = atoi(argv[1]);                               
    
    struct sockaddr_in localAddr,remoteAddr;
          
    int addrLen = sizeof(struct sockaddr_in);  

    localSocket = socket(AF_INET , SOCK_STREAM , 0);

    if (localSocket == -1){
        ERR_EXIT("socket() call failed!!");
    }
  
    svr.port = port;
    svr.listen_fd = localSocket;

    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(port);

    char Message[BUFF_SIZE] = {};

    if( bind(localSocket,(struct sockaddr *)&localAddr , sizeof(localAddr)) < 0) {
        ERR_EXIT("Can't bind() socket");
    }
    listen(localSocket , MAX_CLIENT);
    
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 10000;
    
    /* init client */
    for(int i = 0; i < MAX_CLIENT; ++i) {
        requests[i].conn_fd = 0;
        requests[i].cmd = 0;
        requests[i].file_size = 0;
        requests[i].fp = NULL;
    }
    // requests[svr.listen_fd].conn_fd = svr.listen_fd;
    std::cout <<  "Waiting for connections...\n"
                <<  "Server Port:" << port << std::endl;
    while(1) {    
        memcpy(&r_working, &r_master, sizeof(r_master));
        memcpy(&w_working, &w_master, sizeof(w_master));
        
        select(MAX_CLIENT, &r_working, &w_working, NULL, &timeout);
        for(int fd = 3; fd < MAX_CLIENT; ++fd) {
            if(FD_ISSET(fd, &r_working)) {
                if(requests[fd].cmd == -1) continue;
                else if(requests[fd].cmd == 0) 
                    handle_read(&requests[fd]);
                switch(requests[fd].cmd) {
                    case 1:
                        ls_cmd(fd);
                        requests[fd].cmd = 0;
                        break;
                    case 2:
                        put_cmd(&requests[fd]);
                        // cout << requests[fd].cmd << endl;
                        break;
                    case 3:
                        get_cmd(&requests[fd]);
                        if(requests[fd].cmd == 6) 
                            FD_SET(fd, &w_master);
                        // cout << requests[fd].cmd << endl;
                        break;
                    case 4:
                        play_cmd(&requests[fd]);
                        if(requests[fd].cmd == 7)
                            FD_SET(fd, &w_master);
                        break;
                    case 5:
                        recv_file(&requests[fd]);
                        break;
                }
            }
            if(FD_ISSET(fd, &w_working)) {
                switch(requests[fd].cmd) {
                    case 6:
                        send_file(&requests[fd]);
                        break;
                    case 7:
                        play_video(&requests[fd]);
                        break;
                }
            }
        }
        // check new connection
        fcntl(svr.listen_fd, F_SETFL, O_NONBLOCK);
        remoteSocket = accept(svr.listen_fd, (struct sockaddr *)&remoteAddr, (socklen_t*)&addrLen);  
        if (remoteSocket < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;  // try again
            else if (errno == ENFILE) {
                perror("out of file descriptor table\n");
                continue;
            }
            ERR_EXIT("accept failed!");
        }
        std::cout << "Connection accepted" << std::endl;
        FD_SET(remoteSocket, &r_master);
        requests[remoteSocket].conn_fd = remoteSocket;
    }
    close(remoteSocket);

    return 0;
}


void handle_read(Request *request) {
    char receiveMessage[BUFF_SIZE] = {};
    int recved;
    char cmd[BUFF_SIZE], filename[BUFF_SIZE];
    if ((recved = recv(request->conn_fd,receiveMessage, sizeof(char)*BUFF_SIZE, 0)) < 0){
        cout << "recv failed, with received bytes = " << recved << endl;
        return;
    } 
    // cout << receiveMessage << endl;
    if(strcmp(receiveMessage, "ls") == 0) request->cmd = 1;
    else {
        sscanf(receiveMessage, "%s %s", cmd, filename);
        if(strcmp(cmd, "put") == 0) request->cmd = 2;
        else if(strcmp(cmd, "get") == 0) request->cmd = 3;
        else if(strcmp(cmd, "play") == 0) request->cmd = 4;
        else {
            request->cmd = -1;
            return;
        }
        memset(request->filename, 0, sizeof(request->filename));
        strcpy(request->filename, filename);
        // cout << request->filename << endl;
    }
}


bool checkFileExist(char *filename, const int remoteSocket) {
    struct dirent **namelist;
    int n, sent;
    char sendMessage[BUFF_SIZE] = {};
    if((n = scandir(".", &namelist, NULL, alphasort)) == -1) {
        cout << "scandir error" << endl;
    }
    bool ret = false;
    for(int i = 0; i < n; ++i) {
        if(strcmp(filename, namelist[i]->d_name) == 0) {
            ret = true;
            break;
        }
    }
    for(int i = 0; i < n; ++i) 
        free(namelist[i]);
    free(namelist);
    // handle state
    char state[BUFF_SIZE] = {};
    if(!ret) {
        sprintf(state, "The \'%s\' doesn't exist.", filename);
        send(remoteSocket, state, BUFF_SIZE, 0);
    }
    return ret;
}