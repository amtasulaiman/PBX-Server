#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "pbx.h"
#include "server.h"
#include "debug.h"
#include "csapp.h"

/*
 * Thread function for the thread that handles a particular client.
 *
 * @param  Pointer to a variable that holds the file descriptor for
 * the client connection.  This variable must be freed once the file
 * descriptor has been retrieved.
 * @return  NULL
 *
 * This function executes a "service loop" that receives messages from
 * the client and dispatches to appropriate functions to carry out
 * the client's requests.  The service loop ends when the network connection
 * shuts down and EOF is seen.  This could occur either as a result of the
 * client explicitly closing the connection, a timeout in the network causing
 * the connection to be closed, or the main thread of the server shutting
 * down the connection as part of graceful termination.
 */
void *pbx_client_service(void *arg){
    int connfd = *((int *)arg);
    Pthread_detach(pthread_self());
    Free(arg);

    // todo

    TU *client = pbx_register(pbx, connfd);
    int c;
    char *buff;
    FILE *rfile = Fdopen(connfd, "r");

    while((c = fgetc(rfile)) != EOF){
        int i =  0;
        buff = malloc(sizeof(char));

        while (c != '\r' && c != EOF){
            buff = realloc(buff, i+1);
            *(buff+i) = c;
            i++;
            c = fgetc(rfile);
        }
        if(c != EOF){
            fgetc(rfile);
        }
        buff = realloc(buff, i+1);
        *(buff+i) = '\0';

        int success = -1;
        //specified command execution
        debug("checking command now");
        if(strcmp(buff,tu_command_names[TU_PICKUP_CMD])==0){
            debug("pickup command parsed!");
            success = tu_pickup(client);
        }
        else if(strcmp(buff,tu_command_names[TU_HANGUP_CMD])==0){
            debug("hangup command parsed!");
            success = tu_hangup(client);
        }
        else
            {
            char *buff2 = malloc(sizeof(char)*5);
            int j=0;
            while(j<4){
                *(buff2+j) = buff[j];
                j++;
            }
            *(buff2+4)= '\0';

           // buff = buff+5;
            if(strcmp(buff2,tu_command_names[TU_DIAL_CMD])==0){
                debug("dial command parsed!");
                success = tu_dial(client, atoi(buff+5));
            }
            else if(strcmp(buff2,tu_command_names[TU_CHAT_CMD])==0){
                debug("chat command parsed!");
                success = tu_chat(client, buff+5);
            }
            free(buff2);
        }
        if(success == -1){
            //fprintf(stdout,"Value of buffer is:%s",buff);
            debug("ERror executing command");
        }

        free(buff);

    }

    Close(connfd);
    return NULL;

}