/**
 * === DO NOT MODIFY THIS FILE ===
 * If you need some other prototypes or constants in a header, please put them
 * in another header file.
 *
 * When we grade, we will be replacing this file with our own copy.
 * You have been warned.
 * === DO NOT MODIFY THIS FILE ===
 */

#include "pbx.h"
#include "server.h"

char *tu_state_names[] = {
    [TU_ON_HOOK]       "ON HOOK",
    [TU_RINGING]       "RINGING",
    [TU_DIAL_TONE]     "DIAL TONE",
    [TU_RING_BACK]     "RING BACK",
    [TU_BUSY_SIGNAL]   "BUSY SIGNAL",
    [TU_CONNECTED]     "CONNECTED",
    [TU_ERROR]         "ERROR"
};

char *tu_command_names[] = {
    [TU_PICKUP_CMD]	"pickup",
    [TU_HANGUP_CMD]	"hangup",
    [TU_DIAL_CMD]	"dial",
    [TU_CHAT_CMD]	"chat"
};

/*
 * Object that maintains the state of the Private Branch Exchange (PBX).
 */
PBX *pbx;

