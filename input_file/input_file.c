/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom StÃ¶veken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#define INPUT_PLUGIN_NAME "FILE input plugin"
#define QUEUE   10
#define WIFICAR_MEDIA "WIFI_MEDIA"
#define WIFICAR_MEDIA_LEN 10
#define RECV_BUF_SIZE (8*1024)
#define JPEG_BUF_SIZE (64*1024)
#define LOG_BUF_SIZE (20)

/* private functions and variables to this plugin */
static pthread_t   worker;

static globals     *pglobal;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);
void *recvThread(void *);



static int plugin_number;

static int PORT = 19090;
static int server_fd = 0;
static int last_client_fd = -1;

/* global variables for this plugin */
int input_cmd(int plugin, int command_id, int group, int value, char* sValue);

/*** plugin interface functions ***/
int input_init(input_parameter *param, int id)
{
    int i;
    plugin_number = id;

    param->argv[0] = INPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    

    pglobal = param->global;



    param->global->in[id].name = malloc((strlen(INPUT_PLUGIN_NAME) + 1) * sizeof(char));
    sprintf(param->global->in[id].name, INPUT_PLUGIN_NAME);

    return 0;
}

int input_stop(int id)
{
    DBG("will cancel input thread\n");
    pthread_cancel(worker);
    //pthread_cancel(recvWorker);
    return 0;
}


int input_run(int id)
{
    pglobal->in[id].buf = NULL;

    int server_sockfd = socket(AF_INET,SOCK_STREAM, 0);
    int conn;

    int rcvBufSize = RECV_BUF_SIZE;
    int optlen = sizeof(rcvBufSize);
    if (setsockopt(server_sockfd, SOL_SOCKET, SO_RCVBUF, &rcvBufSize, optlen) < 0)//important, make sure buffer size
    {
        perror("setsockopt");
        exit(1);
    }

    //make socket reuse, to avoid "address in use" error
    int option = 1;
    setsockopt(server_sockfd, SOL_SOCKET,(SO_REUSEPORT | SO_REUSEADDR | SO_DEBUG), (char*) &option,sizeof(option));

    struct sockaddr_in server_sockaddr;
    bzero(&server_sockaddr, sizeof(server_sockaddr));
    server_sockaddr.sin_family = AF_INET;//IPv4
    server_sockaddr.sin_port = htons(PORT);
    server_sockaddr.sin_addr.s_addr = INADDR_ANY;


    ///bind£¬³É¹¦·µ»Ø0£¬³ö´í·µ»Ø-1
    if(bind(server_sockfd,(struct sockaddr *)&server_sockaddr,sizeof(server_sockaddr))==-1)
    {
        perror("bind");
        exit(1);
    }
        
    DBG("bind socket success, start listen!\n");
 
    ///listen£¬³É¹¦·µ»Ø0£¬³ö´í·µ»Ø-1
    if(listen(server_sockfd,QUEUE) == -1)
    {
        perror("listen");
        exit(1);
    }
    
    if (setsockopt(server_sockfd, SOL_SOCKET, SO_RCVBUF, &rcvBufSize, optlen) < 0)//important, make sure buffer size
    {
        perror("setsockopt");
        exit(1);
    }
    DBG("ready to create socket thread : %d\n", server_sockfd);
    server_fd = server_sockfd;//save to global value

    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        //free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }

    pthread_detach(worker);
    //pthread_detach(recvWorker);
    DBG("input_run ..finish\n");
    return 0;
}

/*** private functions for this plugin below ***/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
    " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-d | --delay ]........: delay (in seconds) to pause between frames\n" \
    " [-f | --folder ].......: folder to watch for new JPEG files\n" \
    " [-r | --remove ].......: remove/delete JPEG file after reading\n" \
    " [-n | --name ].........: ignore changes unless filename matches\n" \
    " [-e | --existing ].....: serve the existing *.jpg files from the specified directory\n" \
    " ---------------------------------------------------------------\n");
}

/* the socket sender thread */
void *worker_thread(void *arg)
{
    int conn;
    //int server_sockfd = *((int*)arg);
    pthread_t clientThread;//tmp value, use worker_cleanup to release res

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, &clientThread);

    DBG("worker_thread starting .... server socket id: %d\n", server_fd);
    while(!pglobal->stop) {
        

        struct sockaddr_in client_socket;
        socklen_t length = sizeof(client_socket);
 
        if (-1 == (conn = accept(server_fd, (struct sockaddr*)&client_socket, &length)))
        {
            perror("accept");
            break;
        }

        char buf_ip[INET_ADDRSTRLEN];
        memset(buf_ip, '\0', sizeof(buf_ip));
        inet_ntop(AF_INET,&client_socket.sin_addr, buf_ip, sizeof(buf_ip));
        DBG("A client connect! client id:%d, ip:%s \n", conn, buf_ip);

        DBG("creating recvThread() for client....\n");
        last_client_fd = conn; //important!this meas only last client will be used
        int * tmp = malloc(sizeof(int));
        *tmp = conn;
        
        if (0!= pthread_create(&clientThread, 0, recvThread, tmp))
        {
            perror("pthread_create");
            break; 
        }
        pthread_detach(clientThread);
        DBG("client id:%d recvThread created!\n ", last_client_fd);
    }

thread_quit:

    DBG("leaving input thread, calling cleanup function now\n");
    /* call cleanup handler, signal with the parameter */
    pthread_cleanup_pop(1);

    return NULL;
}
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
//receive thread for client
void *recvThread(void *arg)
{
    int conn = *((int*)arg);//client socket id;
    //import!! put these buf outsize while, keep heap size not exceed!
    char recv_buf[RECV_BUF_SIZE];
    char data[JPEG_BUF_SIZE];
    int dataLen = 0;//length of jpeg data
    char logBuf[LOG_BUF_SIZE];
    while(!pglobal->stop) {
        if (conn > 0) {

            int recv_length = read(conn, recv_buf, sizeof(recv_buf));
            if (recv_length > 0) {

                DBG("jgp data received,conn_fd:%d length:%d \n", conn, recv_length);

                if (dataLen + recv_length >= JPEG_BUF_SIZE) {
                    dataLen = 0;
                    DBG("errer! data exceed==========================================================");
                }
                int pos = findstr(recv_buf, recv_length, WIFICAR_MEDIA);
                
                memset(logBuf, '\0',sizeof(logBuf));
                arrayCopy(logBuf, 0, recv_buf, 0, min(LOG_BUF_SIZE-1, recv_length));
                //DBG("jgp data processing...pos:%d, dataLen:%d, data:%s \n", pos, dataLen, tmp);
                if (pos > 0) {
                    if (dataLen >= 0) {
                        arrayCopy(data, dataLen, recv_buf, 0, pos);

                        //DBG("jgp data broadcast...data:%s \n", tmp);
                        onImageReceived(data, dataLen + pos);  //combine exist data & left part of recv buf

                        arrayCopy(data, 0, recv_buf, pos + WIFICAR_MEDIA_LEN, recv_length - pos -WIFICAR_MEDIA_LEN);
                        dataLen = recv_length - pos - WIFICAR_MEDIA_LEN; //copy right part of recv buf
                    } else {
                        //error state
                    }
                } else {
                    if (pos == 0) {
                        //first frame
                        if (dataLen > 0) {
                            //DBG("jgp data broadcast...data:%s \n", tmp);
                            onImageReceived(data, dataLen);
                        }
                        arrayCopy(data, 0, recv_buf, WIFICAR_MEDIA_LEN, recv_length - WIFICAR_MEDIA_LEN);//TODO: over flow
                        dataLen = recv_length - WIFICAR_MEDIA_LEN;
                    } else {
                        //not found, copy buf to image data
                        arrayCopy(data, dataLen, recv_buf, 0, recv_length);//TODO: over flow
                        dataLen += recv_length;
                    }
                }
            }
        }
        //sleep(1);
    }
    return NULL;
}

void onImageReceived(char *data, int length){
        /* copy JPG picture to global buffer */
        pthread_mutex_lock(&pglobal->in[plugin_number].db);

        /* allocate memory for frame */
        if(pglobal->in[plugin_number].buf != NULL)
            free(pglobal->in[plugin_number].buf);
            
        pglobal->in[plugin_number].buf = malloc(length + 1);
        pglobal->in[plugin_number].size = length;
        memcpy(pglobal->in[plugin_number].buf, data, pglobal->in[plugin_number].size);
        struct timeval timestamp;
        gettimeofday(&timestamp, NULL);
        pglobal->in[plugin_number].timestamp = timestamp;
        DBG("new frame copied (size: %d)\n", pglobal->in[plugin_number].size);
        /* signal fresh_frame */
        pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
        pthread_mutex_unlock(&pglobal->in[plugin_number].db);

}

void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    DBG("cleaning up resources allocated by input thread\n");

    pthread_t *p = (pthread_t *)arg;
    pthread_cancel(*p);

    if(pglobal->in[plugin_number].buf != NULL) {
        free(pglobal->in[plugin_number].buf);
        pglobal->in[plugin_number].buf = NULL;
    }
}

int input_cmd(int plugin, int command_id, int group, int value, char* sValue)
{
    int res;
    int i;
    DBG("Requested cmd (id: %d) for the %d plugin. Group: %d value: %d string: %s \n", command_id, plugin, group, value, sValue);
    if (value > 0 && sValue != NULL && last_client_fd > 0) {
        char *tmp = malloc((1+strlen(sValue))*sizeof(char));
        sprintf(tmp, "%s", sValue);
        int cmdLength = strlen(sValue);
        if(send(last_client_fd, tmp, cmdLength, 0) < 0) {
            DBG("send cmd error! \n");
        }
        free(tmp);
    }
    return 0;
}

int findstr(char* buf, int len, char* str) {
    if (buf != NULL && len > WIFICAR_MEDIA_LEN && str != NULL && strlen(str) == WIFICAR_MEDIA_LEN) {
        int i;
        for (i = 0; i < len -WIFICAR_MEDIA_LEN + 1; i++) {
            /*if (buf[i] == str[0] && buf[i+1] == str[1]
                && buf[i+2] == str[2] && buf[i+3] == str[3]) {
                return i;
            }*/
            int match = 0;
            for (int j = 0; j < WIFICAR_MEDIA_LEN; j++) {
                if (buf[i + j] != str[j]) {
                    break;
                } else {
                    match ++;
                }
            }
            if (match == WIFICAR_MEDIA_LEN) return i;
        }
    } 
    return -1;
}

int arrayCopy(char* dest, int dest_pos, char* src, int src_pos, int len) {
    if (dest_pos < 0 || len <= 0) {
        return -1;//error
    }
    int i = 0;
    int j = src_pos;
    for (i = dest_pos; i < dest_pos + len; i++) {
        dest[i] = src[j];
        j++;
    }
    // DBG("arrayCopy returned! len:%d\n", len);
    return len;
}
    