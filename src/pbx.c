
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

#include "pbx.h"
#include "debug.h"
#include "csapp.h"
#include "server.h"

sem_t sem;

//registry of clients
struct pbx {
    TU *clients_list[PBX_MAX_EXTENSIONS];
};
//tu object
struct tu {
    volatile TU_STATE status;
    volatile int ext;
    volatile int t_fd;
    volatile int other_ext;
};

/*
* Displaying the current state of TU
*/

void print_current_state(TU *tu){
    if (tu->status == TU_ON_HOOK)
        dprintf(tu->t_fd, "%s %d\r\n", tu_state_names[tu->status], tu->ext);
    else if (tu->status == TU_CONNECTED)
        dprintf(tu->t_fd, "%s %d\r\n", tu_state_names[tu->status], tu->other_ext);
    else
        dprintf(tu->t_fd, "%s\r\n", tu_state_names[tu->status]);
}

/*
* Updating the status of TU
*/

void update_state(TU *tu, TU_STATE new_state){
    tu->status = new_state;
    print_current_state(tu);
}

/*
 * Initialize a new PBX.
 *
 * @return the newly initialized PBX, or NULL if initialization fails.
 */
PBX *pbx_init(){
    pbx = malloc(sizeof(PBX));
    Sem_init(&sem, 0, 1);
    return pbx;
}

/*
 * Shut down a pbx, shutting down all network connections, waiting for all server
 * threads to terminate, and freeing all associated resources.
 * If there are any registered extensions, the associated network connections are
 * shut down, which will cause the server threads to terminate.
 * Once all the server threads have terminated, any remaining resources associated
 * with the PBX are freed.  The PBX object itself is freed, and should not be used again.
 *
 * @param pbx  The PBX to be shut down.
 */
void pbx_shutdown(PBX *pbx){
    int success = 0;
    for(int i = 0; i<PBX_MAX_EXTENSIONS; i++){
        if(pbx->clients_list[i]){

        success = shutdown(pbx->clients_list[i]->t_fd, SHUT_RDWR);
        if(success == -1)
            debug("Error shutting down connection");
        }
    }

    free(pbx);
}

/*
 * Register a TU client with a PBX.
 * This amounts to "plugging a telephone unit into the PBX".
 * The TU is assigned an extension number and it is initialized to the TU_ON_HOOK state.
 * A notification of the assigned extension number is sent to the underlying network
 * client.
 *
 * @param pbx  The PBX.
 * @param fd  File descriptor providing access to the underlying network client.
 * @return A TU object representing the client TU, if registration succeeds, otherwise NULL.
 * The caller is responsible for eventually calling pbx_unregister to free the TU object
 * that was returned.
 */
TU *pbx_register(PBX *pbx, int fd){
    P(&sem);
    TU *client = malloc(sizeof(TU));
    pbx->clients_list[fd] = client;
    pbx->clients_list[fd]->ext = fd;
    pbx->clients_list[fd]->t_fd = fd;
    pbx->clients_list[fd]->other_ext = -1;
    pbx->clients_list[fd]->status = TU_ON_HOOK;
    print_current_state(client);
    V(&sem);
    return pbx->clients_list[fd];
}

/*
 * Unregister a TU from a PBX.
 * This amounts to "unplugging a telephone unit from the PBX".
 *
 * @param pbx  The PBX.
 * @param tu  The TU to be unregistered.
 * This object is freed as a result of the call and must not be used again.
 * @return 0 if unregistration succeeds, otherwise -1.
 */
int pbx_unregister(PBX *pbx, TU *tu){
    P(&sem);
    if(tu->ext >= 0 && tu->ext < PBX_MAX_EXTENSIONS)
    {
        if(tu->other_ext != -1){
            pbx->clients_list[tu->other_ext]->status = TU_DIAL_TONE;
            pbx->clients_list[tu->other_ext]->other_ext = -1;
        }
        close(tu->t_fd);
        pbx->clients_list[tu->ext] = 0;
        free(pbx->clients_list[tu->ext]);
        V(&sem);
        return 0;
    }
    V(&sem);
    return -1;
}

/*
 * Get the file descriptor for the network connection underlying a TU.
 * This file descriptor should only be used by a server to read input from
 * the connection.  Output to the connection must only be performed within
 * the PBX functions.
 *
 * @param tu
 * @return the underlying file descriptor, if any, otherwise -1.
 */
int tu_fileno(TU *tu){
    int fileno = tu->t_fd;
    if(fileno>=0 && fileno< PBX_MAX_EXTENSIONS )
        return fileno;
    return -1;
}

/*
 * Get the extension number for a TU.
 * This extension number is assigned by the PBX when a TU is registered
 * and it is used to identify a particular TU in calls to tu_dial().
 * The value returned might be the same as the value returned by tu_fileno(),
 * but is not necessarily so.
 *
 * @param tu
 * @return the extension number, if any, otherwise -1.
 */
int tu_extension(TU *tu){

    int extension = tu->ext;
    if(extension>=0 && extension < PBX_MAX_EXTENSIONS )
        return extension;
    return -1;
}

/*
 * Take a TU receiver off-hook (i.e. pick up the handset).
 *
 *   If the TU was in the TU_ON_HOOK state, it goes to the TU_DIAL_TONE state.
 *   If the TU was in the TU_RINGING state, it goes to the TU_CONNECTED state,
 *     reflecting an answered call.  In this case, the calling TU simultaneously
 *     also transitions to the TU_CONNECTED state.
 *   If the TU was in any other state, then it remains in that state.
 *
 * In all cases, a notification of the new state is sent to the network client
 * underlying this TU. In addition, if the new state is TU_CONNECTED, then the
 * calling TU is also notified of its new state.
 *
 * @param tu  The TU that is to be taken off-hook.
 * @return 0 if successful, -1 if any error occurs.  Note that "error" refers to
 * an underlying I/O or other implementation error; a transition to the TU_ERROR
 * state (with no underlying implementation error) is considered a normal occurrence
 * and would result in 0 being returned.
 */
int tu_pickup(TU *tu){
    P(&sem);
    if(tu->status == TU_ON_HOOK){
        update_state(tu, TU_DIAL_TONE);
    }
    else if(tu->status == TU_RINGING){
        update_state(tu, TU_CONNECTED);
        update_state(pbx->clients_list[tu->other_ext], TU_CONNECTED);
    }
    else{
        print_current_state(tu);
    }
    V(&sem);
    return 0;
}

/*
 * Hang up a TU (i.e. replace the handset on the switchhook).
 *
 *   If the TU was in the TU_CONNECTED state, then it goes to the TU_ON_HOOK state.
 *     In addition, in this case the peer TU (the one to which the call is currently
 *     connected) simultaneously transitions to the TU_DIAL_TONE state.
 *   If the TU was in the TU_RING_BACK state, then it goes to the TU_ON_HOOK state.
 *     In addition, in this case the calling TU (which is in the TU_RINGING state)
 *     simultaneously transitions to the TU_ON_HOOK state.
 *   If the TU was in the TU_RINGING state, then it goes to the TU_ON_HOOK state.
 *     In addition, in this case the called TU (which is in the TU_RING_BACK state)
 *     simultaneously transitions to the TU_DIAL_TONE state.
 *   If the TU was in the TU_DIAL_TONE, TU_BUSY_SIGNAL, or TU_ERROR state,
 *     then it goes to the TU_ON_HOOK state.
 *   If the TU was in any other state, then there is no change of state.
 *
 * In all cases, a notification of the new state is sent to the network client
 * underlying this TU.  In addition, if the previous state was TU_CONNECTED,
 * TU_RING_BACK, or TU_RINGING, then the peer, called, or calling TU is also
 * notified of its new state.
 *
 * @param tu  The tu that is to be hung up.
 * @return 0 if successful, -1 if any error occurs.  Note that "error" refers to
 * an underlying I/O or other implementation error; a transition to the TU_ERROR
 * state (with no underlying implementation error) is considered a normal occurrence
 * and would result in 0 being returned.
 */
int tu_hangup(TU *tu){
    P(&sem);
    if(tu->status == TU_CONNECTED){
        update_state(tu, TU_ON_HOOK);
        update_state(pbx->clients_list[tu->other_ext],TU_DIAL_TONE);
        pbx->clients_list[tu->other_ext]->other_ext = -1;
        tu->other_ext = -1;
    }
    else if(tu->status == TU_RING_BACK){
        update_state(tu, TU_ON_HOOK);
        update_state(pbx->clients_list[tu->other_ext],TU_ON_HOOK);
        pbx->clients_list[tu->other_ext]->other_ext = -1;
        tu->other_ext = -1;
    }
    else if(tu->status == TU_RINGING){
        update_state(tu, TU_ON_HOOK);
        update_state(pbx->clients_list[tu->other_ext],TU_DIAL_TONE);
        pbx->clients_list[tu->other_ext]->other_ext = -1;
        tu->other_ext = -1;
    }
    else if(tu->status == TU_DIAL_TONE || tu->status == TU_BUSY_SIGNAL || tu->status == TU_ERROR ){
        update_state(tu, TU_ON_HOOK);
    }
    else
        print_current_state(tu);

    V(&sem);
    return 0;
}

/*
 * Dial an extension on a TU.
 *
 *   If the specified extension number does not refer to any currently registered
 *     extension, then the TU transitions to the TU_ERROR state.
 *   Otherwise, if the TU was in the TU_DIAL_TONE state, then what happens depends
 *     on the current state of the dialed extension:
 *       If the dialed extension was in the TU_ON_HOOK state, then the calling TU
 *         transitions to the TU_RING_BACK state and the dialed TU simultaneously
 *         transitions to the TU_RINGING state.
 *       If the dialed extension was not in the TU_ON_HOOK state, then the calling
 *         TU transitions to the TU_BUSY_SIGNAL state and there is no change to the
 *         state of the dialed extension.
 *   If the TU was in any state other than TU_DIAL_TONE, then there is no state change.
 *
 * In all cases, a notification of the new state is sent to the network client
 * underlying this TU.  In addition, if the new state is TU_RING_BACK, then the
 * called extension is also notified of its new state (i.e. TU_RINGING).
 *
 * @param tu  The tu on which the dialing operation is to be performed.
 * @param ext  The extension to be dialed.
 * @return 0 if successful, -1 if any error occurs.  Note that "error" refers to
 * an underlying I/O or other implementation error; a transition to the TU_ERROR
 * state (with no underlying implementation error) is considered a normal occurrence
 * and would result in 0 being returned.
 */
int tu_dial(TU *tu, int ext){
    P(&sem);

    if(!(pbx->clients_list[ext])){
        update_state(tu, TU_ERROR);
    }
    else if(tu->status == TU_DIAL_TONE){
        if(pbx->clients_list[ext]->status == TU_ON_HOOK){
            update_state(tu, TU_RING_BACK);
            update_state(pbx->clients_list[ext], TU_RINGING);
            pbx->clients_list[ext]->other_ext = tu->ext;
            tu->other_ext = ext;
        }
        else{
            update_state(tu, TU_BUSY_SIGNAL);
        }
    }
    else
        print_current_state(tu);
    V(&sem);
    return 0;
}

/*
 * "Chat" over a connection.
 *
 * If the state of the TU is not TU_CONNECTED, then nothing is sent and -1 is returned.
 * Otherwise, the specified message is sent via the network connection to the peer TU.
 * In all cases, the states of the TUs are left unchanged and a notification containing
 * the current state is sent to the TU sending the chat.
 *
 * @param tu  The tu sending the chat.
 * @param msg  The message to be sent.
 * @return 0  If the chat was successfully sent, -1 if there is no call in progress
 * or some other error occurs.
 */
int tu_chat(TU *tu, char *msg){
    P(&sem);
    if(tu->status != TU_CONNECTED){
        print_current_state(tu);
        V(&sem);
        return -1;
    }
    else{
        dprintf(tu->other_ext, "CHAT %s\n", msg);
        print_current_state(tu);
    }
    V(&sem);
    return 0;
}

