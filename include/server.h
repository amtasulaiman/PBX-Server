/**
 * === DO NOT MODIFY THIS FILE ===
 * If you need some other prototypes or constants in a header, please put them
 * in another header file.
 *
 * When we grade, we will be replacing this file with our own copy.
 * You have been warned.
 * === DO NOT MODIFY THIS FILE ===
 */
#ifndef SERVER_H
#define SERVER_H

/*
 * Definitions of the commands that can be issued by a client.
 */
typedef enum tu_command {
    TU_PICKUP_CMD, TU_HANGUP_CMD, TU_DIAL_CMD, TU_CHAT_CMD
} TU_COMMAND;

/*
 * Array that specifies a printable name for each of the commands that
 * can be issued to a TU by a client.  These names should be used when
 * parsing commands received from a client.  They may also be used for
 * debugging purposes.
 */
extern char *tu_command_names[];

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
void *pbx_client_service(void *arg);

#endif
