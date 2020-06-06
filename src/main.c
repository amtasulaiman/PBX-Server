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


static void terminate(int status);

void sigh_up_handler(int signum){
    terminate(EXIT_SUCCESS);
}

/*
 * "PBX" telephone exchange simulation.
 *
 * Usage: pbx <port>
 */
int main(int argc, char* argv[]){
    // Option processing should be performed here.
    // Option '-p <port>' is required in order to specify the port number
    // on which the server should listen.
    char *port;
    if(argc != 3){
        debug("Invalid Arguments\n");
        exit(EXIT_FAILURE);
    }
    else{
        if(strcmp(argv[1],"-p") == 0){
            port = argv[2]; //getting port #
        }
        else{
            debug("required -p <port> missing.");
            exit(EXIT_FAILURE);
        }
    }
    //sighup handler


    // Perform required initialization of the PBX module.
    debug("Initializing PBX...");
    pbx = pbx_init();

    // TODO: Set up the server socket and enter a loop to accept connections
    // on this socket.  For each connection, a thread should be started to
    // run function pbx_client_service().  In addition, you should install
    // a SIGHUP handler, so that receipt of SIGHUP will perform a clean
    // shutdown of the server.

    struct sigaction sa, prev;
    sa.sa_handler = sigh_up_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    int sig_result = sigaction(SIGHUP,&sa,&prev);
    if(sig_result < 0){
        debug("sigaction calling failure");
        exit(EXIT_FAILURE);
    }

    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    debug("listening to port");
    listenfd = Open_listenfd(port);
    while(1){
        clientlen = sizeof(struct sockaddr_storage);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd,
        (SA *) &clientaddr, &clientlen);
        Pthread_create(&tid, NULL, pbx_client_service, connfdp);

    }


    fprintf(stderr, "You have to finish implementing main() "
        "before the PBX server will function.\n");

    terminate(EXIT_FAILURE);
}

/*
 * Function called to cleanly shut down the server.
 */
void terminate(int status) {
    debug("Shutting down PBX...");
    pbx_shutdown(pbx);
    debug("PBX server terminating");
    exit(status);
}
