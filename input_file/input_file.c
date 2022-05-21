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
static globals     *pglobal;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);
void *senderThread(int);
static double delay = 1.0;
static char *cmd = NULL;
static int cmdLength = 0;

static char *filename = NULL;
static int rm = 0;
static int plugin_number;
static read_mode mode = NewFilesOnly;
static int PORT = 19090;
static char *IP = "127.0.0.1";

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
    return 0;
}


int input_run(int id)
{
    pglobal->in[id].buf = NULL;

    int server_sockfd = socket(AF_INET,SOCK_STREAM, 0);
    int conn;
     
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
        
    printf("bind success, start listen!");
 
    ///listen£¬³É¹¦·µ»Ø0£¬³ö´í·µ»Ø-1
    if(listen(server_sockfd,QUEUE) == -1)
    {
        perror("listen");
        exit(1);
    }
    DBG("ready to create socket thread : %d\n", server_sockfd);
    if(pthread_create(&worker, 0, worker_thread, &server_sockfd) != 0) {
        //free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }

       pthread_detach(worker);

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
    int server_sockfd = *((int*)arg);
    DBG("success create socket thread : %d\n", server_sockfd);
    while(!pglobal->stop)
    {
        pthread_t thread;

        struct sockaddr_in client_addr;
        socklen_t length = sizeof(client_addr);
 
        if (-1 == (conn = accept(server_sockfd, (struct sockaddr*)&client_addr, &length)))
        {
            perror("accept");
            break;
        }
        
        DBG("will pthread_create senderThread()\n");
        if (0!= pthread_create(&thread, NULL, senderThread, conn))
        {
            perror("pthread_create");
            break;
        }
    }

thread_quit:

    DBG("leaving input thread, calling cleanup function now\n");
    /* call cleanup handler, signal with the parameter */
    pthread_cleanup_pop(1);

    return NULL;
}


void *senderThread(int conn_fd)
{
    //char recv_buf[BUFFER_SIZE];
    while(1) {

        if (cmd != NULL && strlen(cmd) > 0) {
            DBG("ready to send cmd : %s\n", cmd);
            if(send(conn_fd, cmd, strlen(cmd), 0) < 0)
            {
                free(cmd);
                cmd = NULL;
                perror("send");
                break;
            }
            free(cmd);
            cmd = NULL;
        }

        sleep(1);
    }
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

    if(pglobal->in[plugin_number].buf != NULL) free(pglobal->in[plugin_number].buf);

}

int input_cmd(int plugin, int command_id, int group, int value, char* sValue)
{
    int res;
    int i;
    DBG("Requested cmd (id: %d) for the %d plugin. Group: %d value: %d string: %s \n", command_id, plugin, group, value, sValue);
    if (value > 0 && sValue != NULL) {
        cmd = malloc((1+strlen(sValue))*sizeof(char));
        sprintf(cmd, "%s", sValue);
        cmdLength = strlen(sValue);
    }
    return 0;
}





