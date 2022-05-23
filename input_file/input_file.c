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
typedef enum _read_mode {
    NewFilesOnly,
    ExistingFiles
} read_mode;

/* private functions and variables to this plugin */
static pthread_t   worker;
static pthread_t   recvWorker;
static globals     *pglobal;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);
void *recvThread(void *);
static double delay = 1.0;


static char *filename = NULL;
static int rm = 0;
static int plugin_number;
static read_mode mode = NewFilesOnly;
static int PORT = 19090;
static int server_fd = 0;
static int conn_fd = 0;

/* global variables for this plugin */
static int fd, rc, wd, size;
static struct inotify_event *ev;
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
    pthread_cancel(recvWorker);
    return 0;
}


int input_run(int id)
{
    pglobal->in[id].buf = NULL;

    int server_sockfd = socket(AF_INET,SOCK_STREAM, 0);
    int conn;
    
    int rcvBufSize = 1024*1024;
    int optlen = sizeof(rcvBufSize);
    if (setsockopt(server_sockfd, SOL_SOCKET, SO_RCVBUF, &rcvBufSize, optlen) < 0)//important, make sure buffer size
    {
        perror("setsockopt");
        exit(1);
    }
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
        
    DBG("bind success, start listen!\n");
 
    ///listen£¬³É¹¦·µ»Ø0£¬³ö´í·µ»Ø-1
    if(listen(server_sockfd,QUEUE) == -1)
    {
        perror("listen");
        exit(1);
    }
    DBG("ready to create socket thread : %d\n", server_sockfd);
    server_fd = server_sockfd;//save to global value
    conn_fd = 0;
    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        //free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }

    /*if(pthread_create(&recvWorker, 0, recvThread, NULL) != 0) {
        //free(pglobal->in[id].buf);
        fprintf(stderr, "could not start recvThread\n");
        exit(EXIT_FAILURE);
    }*/
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
    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);
    int conn;
    //int server_sockfd = *((int*)arg);
    DBG("success create socket thread, wait for client connect....sockfd: %d\n", server_fd);
    while(!pglobal->stop)
    {
        //pthread_t thread;

        struct sockaddr_in client_addr;
        socklen_t length = sizeof(client_addr);
 
        if (-1 == (conn = accept(server_fd, (struct sockaddr*)&client_addr, &length)))
        {
            perror("accept");
            break;
        }
        if (conn == 0) {
            continue;
        }
        conn_fd = conn; //important!this meas only last client will be used
        int * tmp = malloc(sizeof(int));
        *tmp = conn;
        DBG("client connect! conn_id:%d pthread_create senderThread()\n", conn);
        if (0!= pthread_create(&recvWorker, 0, recvThread, tmp))
        {
            perror("pthread_create");
            break; 
        }
        DBG("client connect! conn id:%d \n ", conn_fd);
    }

thread_quit:

    DBG("leaving input thread, calling cleanup function now\n");
    /* call cleanup handler, signal with the parameter */
    pthread_cleanup_pop(1);

    return NULL;
}


void *recvThread(void *arg)
{
    int conn = *((int*)arg);
    DBG("recvThread running ....conn_fd:%d\n", conn);

    while(!pglobal->stop) {
        if (conn > 0) {
            char recv_buf[1024*1024];
            int recv_length = recv(conn, recv_buf, sizeof(recv_buf), 0);
            if (recv_length > 0) {
                DBG("jgp data received,conn_fd:%d data len:%d\n", conn, recv_length);
                //onImageReceived(recv_buf, recv_length);
            }
        }
        sleep(1);
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
        //pglobal->in[plugin_number].timestamp = timestamp;
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
    if (value > 0 && sValue != NULL) {
        char *tmp = malloc((1+strlen(sValue))*sizeof(char));
        sprintf(tmp, "%s", sValue);
        int cmdLength = strlen(sValue);
        if(send(conn_fd, tmp, cmdLength, 0) < 0) {
            DBG("send cmd error! \n");
        }
        free(tmp);
    }
    return 0;
}
