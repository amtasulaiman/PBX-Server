#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "pbx.h"
#include "server.h"
#include "debug.h"

/* Default values. */
#define PORT 3333            // Default server port number
#define EXTENSION_MIN 4      // Default minimum extension number to dial
#define EXTENSION_MAX 5      // Default maximum extension number to dial
#define BASIC_DELAY 100000   // Default basic delay time in microseconds

/*
 * Action table for the tester.
 * The entries specify the probabilities of the possible actions that
 * can be taken from a state.  There are small probabilities for taking
 * "senseless" actions, in order to exercise edge cases.
 */

#define NUM_STATES 7
#define NUM_COMMANDS 5
#define DELAY_COMMAND (NUM_COMMANDS-1)

double action_probs[NUM_STATES][NUM_COMMANDS] = {
//		  TU_PICKUP_CMD   TU_HANGUP_CMD   TU_DIAL_CMD   TU_CHAT_CMD   DELAY
[TU_ON_HOOK]     {     0.1,            0.01,          0.01,         0.01,      0.87  },
[TU_RINGING]     {     0.5,            0.01,          0.01,         0.01,      0.47  },
[TU_DIAL_TONE]   {     0.01,           0.1,           0.67,         0.01,      0.2   },
[TU_RING_BACK]   {     0.01,           0.1,           0.01,         0.01,      0.87  },
[TU_BUSY_SIGNAL] {     0.01,           0.87,          0.01,         0.01,      0.1   },
[TU_CONNECTED]   {     0.01,           0.1,           0.01,         0.1,       0.78  },
[TU_ERROR]       {     0.01,           0.87,          0.01,         0.01,      0.1   }
};

/*
 * Table of expected next states.
 * Each entry is a bitmap that specifies a set of possible next states, given
 * the current state and the last command that was issued.
 *
 * An issue that this tester has to handle is that commands to the server can
 * "cross in transit" asynchronous state-change notifications coming back from the server.
 * If we are currently in the TU_ON_HOOK state and we send a TU_PICKUP_CMD, it might
 * be that the TU_PICKUP_CMD crosses in transit a TU_RINGING notification being sent
 * back to us.  What we will see is a next-state notification of TU_RINGING, rather
 * than the TU_DIAL_TONE notification that we would otherwise expect.
 *
 * To handle this, there are two classes of expected states encoded in each entry of
 * the table.  The "normal case" encodes a TU_STATE s as the bit value 1<<s, and it
 * indicates a state that we would expect to see if there were no "crossing in transit".
 * The "abnormal case" encodes additional states that we might see when messages
 * cross in transit.  These are encoded as 1<<(s+RESYNC), where RESYNC is larger than
 * any TU_STATE value.  When we receive a state notification, it is checked against
 * the expected state bitmap.  If we find that state among the "normal case" states,
 * then nothing special happens and we proceed on to selecting the next command to send.
 * On the other hand, if we find that state among the "abnormal case" states, then
 * a "resync" flag is set and we do not immediately select a new command to send.
 * Instead, we assume that what we have just received is an asynchronous state-change
 * notification that crossed in transit our last command, and that the response to
 * our last command is still forthcoming.  In this situation, we redetermine the set
 * of expected events based on the new state, but the last command that we sent.
 * When we finally do receive a "normal case" response, then the resynchronization is
 * over and we proceed to send another command.
 *
 * A deficiency in the current implementation is that there ought to be a timeout after
 * which we declare failure if a resynchronization has not completed within a short
 * period of time.
 *
 * Another deficiency at the moment is that the tester tests that "bad things don't happen",
 * but it doesn't really check that "good things do happen" (e.g. that calls get connected).
 *
 * One other deficiency is in the treatment of delays.  When the action chosen from a state
 * is to delay, the delays will continue until a non-delay action is chosen, without reading
 * any notifications from the server until the delay period is over.  It would be better if
 * the arrival of notifications from the server was checked after each basic delay, but that
 * would further complicate the program and it has not been implemented at this time.
 */

#define RESYNC NUM_STATES

int next_states[NUM_STATES][NUM_COMMANDS] = {
  [TU_ON_HOOK] {
      1<<TU_DIAL_TONE | 1<<(TU_RINGING+RESYNC) | 1<<(TU_ON_HOOK+RESYNC),    // TU_PICKUP_CMD
      1<<TU_ON_HOOK | 1<<(TU_RINGING+RESYNC),                               // TU_HANGUP_CMD
      1<<TU_ON_HOOK | 1<<(TU_RINGING+RESYNC),                               // TU_DIAL_CMD
      1<<TU_ON_HOOK | 1<<(TU_RINGING+RESYNC),                               // TU_CHAT_CMD
      1<<(TU_ON_HOOK+RESYNC) | 1<<(TU_RINGING+RESYNC)                       // DELAY
  },
  [TU_RINGING] {
      1<<TU_CONNECTED | 1<<(TU_ON_HOOK+RESYNC) | 1<<(TU_RINGING+RESYNC),    // TU_PICKUP_CMD
      1<<TU_ON_HOOK | 1<<(TU_RINGING+RESYNC),                               // TU_HANGUP_CMD
      1<<TU_RINGING | 1<<(TU_ON_HOOK+RESYNC),                               // TU_DIAL_CMD
      1<<TU_RINGING | 1<<(TU_ON_HOOK+RESYNC),                               // TU_CHAT_CMD
      1<<(TU_RINGING+RESYNC) | 1<<(TU_ON_HOOK+RESYNC)                       // DELAY
  },
  [TU_DIAL_TONE] {
      1<<TU_DIAL_TONE,                                                      // TU_PICKUP_CMD
      1<<TU_ON_HOOK | 1<<(TU_DIAL_TONE+RESYNC),                             // TU_HANGUP_CMD
      1<<TU_RING_BACK | 1<<TU_BUSY_SIGNAL | 1<<TU_ERROR
                      | 1<<(TU_DIAL_TONE+RESYNC),                           // TU_DIAL_CMD
      1<<TU_DIAL_TONE,                                                      // TU_CHAT_CMD
      1<<(TU_DIAL_TONE+RESYNC)                                              // DELAY
  },
  [TU_RING_BACK] {
      1<<TU_RING_BACK | 1<<(TU_CONNECTED+RESYNC) | 1<<(TU_DIAL_TONE+RESYNC),// TU_PICKUP_CMD
      1<<TU_ON_HOOK | 1<<(TU_CONNECTED+RESYNC) | 1<<(TU_DIAL_TONE+RESYNC)
                    | 1<<(TU_RING_BACK+RESYNC),                             // TU_HANGUP_CMD
      1<<TU_RING_BACK | 1<<(TU_CONNECTED+RESYNC) | 1<<(TU_DIAL_TONE+RESYNC),// TU_DIAL_CMD
      1<<TU_RING_BACK | 1<<(TU_CONNECTED+RESYNC) | 1<<(TU_DIAL_TONE+RESYNC),// TU_CHAT_CMD
      1<<(TU_RING_BACK+RESYNC) | 1<<(TU_CONNECTED+RESYNC) | 1<<(TU_DIAL_TONE+RESYNC) // DELAY
  },
  [TU_BUSY_SIGNAL] {
      1<<TU_BUSY_SIGNAL,                                                    // TU_PICKUP_CMD
      1<<TU_ON_HOOK | 1<<(TU_BUSY_SIGNAL+RESYNC),                           // TU_HANGUP_CMD
      1<<TU_BUSY_SIGNAL,                                                    // TU_DIAL_CMD
      1<<TU_BUSY_SIGNAL,                                                    // TU_CHAT_CMD
      1<<(TU_BUSY_SIGNAL+RESYNC)                                            // DELAY
  },
  [TU_CONNECTED] {
      1<<TU_CONNECTED | 1<<(TU_DIAL_TONE+RESYNC) | 1<<(TU_CONNECTED+RESYNC),// TU_PICKUP_CMD
      1<<TU_ON_HOOK | 1<<(TU_DIAL_TONE+RESYNC) | 1<<(TU_CONNECTED+RESYNC),  // TU_HANGUP_CMD
      1<<TU_CONNECTED | 1<<(TU_DIAL_TONE+RESYNC),                           // TU_DIAL_CMD
      1<<TU_CONNECTED | 1<<(TU_DIAL_TONE+RESYNC),                           // TU_CHAT_CMD
      1<<(TU_CONNECTED+RESYNC) | 1<<(TU_DIAL_TONE+RESYNC)                   // DELAY
  },
  [TU_ERROR] {
      1<<TU_ERROR,                                                          // TU_PICKUP_CMD
      1<<TU_ON_HOOK | 1<<(TU_ERROR+RESYNC),                                 // TU_HANGUP_CMD
      1<<TU_ERROR,                                                          // TU_DIAL_CMD
      1<<TU_ERROR,                                                          // TU_CHAT_CMD
      1<<(TU_ERROR+RESYNC)                                                  // DELAY
  }
};

/* The current state of the TU simulated by the tester. */
TU_STATE current_state;

/*
 * A bitmap that specifies the set of next states we expect, as described above.
 * A transition to a state not in this set results in a test failure.
 */
int expected_states;

/*
 * Flag that indicates whether we are currently resynchronizing, as a result of
 * due to messages that "crossed in transit".
 */
int resync;

/*
 * The last command sent to the server.  This is used during resynchronization,
 * to determine a new set of expected states as each successive asynchronous
 * notification is received.
 */
TU_COMMAND last_command = TU_HANGUP_CMD;

/* Option variables. */
int port = PORT;
int cmds = 0;
int min_ext = EXTENSION_MIN, max_ext = EXTENSION_MAX;
int basic_delay = BASIC_DELAY;

/* Prototypes for functions that appear below. */
static void test(FILE *in, FILE *out, int cmds);
static int choose_action(void);
static TU_STATE parse_message(char *msg);
static int connect_to_server(struct in_addr *addr, int port);
static char *unparse_state_set(int set);
static void trim_eol(char *msg);
static char *timestamp(void);

/*
 * Entry point for the tester program.
 *
 * Command-line options:
 *   -h <hostname>                (default "localhost")
 *   -p <port>                    (default 3333)
 *   -l <length_of_test>          (default 0 -- means infinite)
 *   -x <min_extension>           (default 4)
 *   -y <max_extension>           (default 5)
 *   -d <microseconds>            (basic delay time: default 100000)
 */
int main(int argc, char *argv[]) {
    char *hostname = "localhost";
    struct in_addr sa;
    struct hostent *he;
    int sfd;
    FILE *in, *out;
    int option;
    while((option = getopt(argc, argv, "h:p:l:x:y:d:")) != EOF) {
	switch(option) {
	case 'h':
	    hostname = optarg++;
	    break;
	case 'p':
	    port = atoi(optarg++);
	    break;
	case 'l':
	    cmds = atoi(optarg++);
	    break;
	case 'x':
	    min_ext = atoi(optarg++);
	    break;
	case 'y':
	    max_ext = atoi(optarg++);
	    break;
	case 'd':
	    basic_delay = atoi(optarg++);
	    break;
	}
    }

    // Connect to server on specified port and set sfd.
    if((he = gethostbyname(hostname)) == NULL) {
	herror("gethostbyname");
	exit(EXIT_FAILURE);
    }
    memcpy(&sa, he->h_addr, sizeof(sa));
    if((sfd = connect_to_server(&sa, port)) == -1) {
	perror("proto_connect");
	exit(EXIT_FAILURE);
    }
    struct sockaddr_in s;
    socklen_t sl = sizeof(s);
    getsockname(sfd, (struct sockaddr *)&s, &sl);
    port = s.sin_port;
    fprintf(stdout, "%s: Connected to server %s:%d\n", timestamp(), hostname, port);

    // Set up streams to the socket file descriptor.
    in = fdopen(sfd, "r");
    out = fdopen(sfd, "w");

    // Perform testing.
    test(in, out, cmds);
}

/* There isn't really a maximum message length, but this is just a test driver... */
#define MAX_MESSAGE_LEN 256

/*
 * Perform testing.
 *
 * @param in  Stream from PBX.
 * @param out Stream to PBX.
 * @param cmds  Total number of commands to be sent, or -1 for infinite.
 */
void test(FILE *in, FILE *out, int cmds) {
    int n = 0;
    if(cmds == 0)
	cmds = -1;

    // Initial expected state notification is TU_ON_HOOK
    expected_states = 1<<TU_ON_HOOK;
    while(cmds == -1 || cmds > 0) {
	TU_STATE new;
	TU_COMMAND cmd;
	char msg[MAX_MESSAGE_LEN];
	if(fgets(msg, MAX_MESSAGE_LEN, in) == NULL) {
	    fprintf(stderr, "%s: EOF reading message from server\n", timestamp());
	    if(resync) {
		fprintf(stderr, "%s: Premature disconnection during resync\n", timestamp());
		exit(EXIT_FAILURE);
	    } else {
		fprintf(stderr, "%s: Tester terminating\n", timestamp());
		exit(EXIT_SUCCESS);
	    }
	}
	trim_eol(msg);
	fprintf(stderr, "%s: Message from server: %s\n", timestamp(), msg);
	new = parse_message(msg);
	if(new == NUM_STATES) {
	    // The message is chat.  There is no state transition, but we must be
	    // in the connected state.
	    if(current_state != TU_CONNECTED) {
		fprintf(stderr, "Chat received when not in state %s\n",
			tu_state_names[current_state]);
		exit(EXIT_FAILURE);
	    }
	    continue;
	}

	// Check state transition to see if it is as expected.
	if(1<<new & expected_states) {
	    // OK
	    resync = 0;
	} else if(1<<(new+RESYNC) & expected_states) {
	    // OK, but set resync because messages crossed in transit.
	    fprintf(stderr, "%s: Resync: state %s, expecting %s\n",
		    timestamp(), tu_state_names[new], unparse_state_set(expected_states));
	    resync = 1;
	} else {
	    // New state is not one that is expected -- testing fails.
	    fprintf(stderr, "%s: New state %s is not in expected set %s\n",
		    timestamp(), tu_state_names[new], unparse_state_set(expected_states));
	    exit(EXIT_FAILURE);
	}

	// Update current state to that specified in message
	fprintf(stderr, "%s: Change state: %s -> %s\n",
		timestamp(), tu_state_names[current_state], tu_state_names[new]);
	current_state = new;

	// Determine action to be performed.
	if(resync) {
	    //   If resyncing, then no new action is performed.
	    fprintf(stderr, "%s: Resync\n", timestamp());
	} else {
	    // Delay until a non-delay command is chosen.
	    int delayed = 0;
	    while(1) {
		cmd = choose_action();
		if(cmd == DELAY_COMMAND) {
		    delayed = 1;
		    struct timespec ts = { basic_delay / 1000000, (basic_delay % 1000000) * 1000 };
		    fprintf(stderr, ".");
		    nanosleep(&ts, NULL);
		} else {
		    break;
		}
	    }
	    if(delayed)
		fprintf(stderr, "\n");
	    n++;
	    if(cmds > 0)
		cmds--;
	    if(cmd == TU_DIAL_CMD) {
		// Choose an extension at random from the valid range.
		int ext = min_ext + random() % (max_ext - min_ext + 1);
		fprintf(stderr, "%s: Sending command (%d): %s %d\n",
			timestamp(), n, tu_command_names[cmd], ext);
		fprintf(out, "%s %d%s", tu_command_names[cmd], ext, EOL);
	    } else {
		fprintf(stderr, "%s: Sending command (%d): %s\n",
			timestamp(), n, tu_command_names[cmd]);
		fprintf(out, "%s%s", tu_command_names[cmd], EOL);
	    }
	    fflush(out);
	}

	// Update expected states:
	if(resync) {
	    // If resync, then do it based on last command sent.
	    expected_states = next_states[new][last_command];
	} else {
	    // Otherwise, do it based on current command.
	    expected_states = next_states[new][cmd];
	    last_command = cmd;
	}
	fprintf(stderr, "%s: Expecting: %s\n", timestamp(), unparse_state_set(expected_states));
    }
}

/*
 * Choose a random action based on the current state.
 */
static int choose_action(void) {
    double r = (double)(random()) / RAND_MAX;
    double p = 0.0;
    for(int i = 0; i < NUM_COMMANDS; i++) {
	p += action_probs[current_state][i];
	if(r <= p)
	    return i;
    }
    // Probably should not happen...
    return NUM_COMMANDS-1;
}

/*
 * Parse a message from the PBX, determining the new state.
 */
static TU_STATE parse_message(char *msg) {
    for(int i = 0; i < NUM_STATES; i++) {
	if(strstr(msg, tu_state_names[i]) == msg)
	    return i;
    }
    if(strstr(msg, "CHAT") == msg)
	return NUM_STATES;
    fprintf(stderr, "%s: Unrecognized message: %s\n", timestamp(), msg);
    abort();
}

/*
 * Connect to the server at a specified address.
 *
 * Returns: connection file descriptor in case of success.
 * Returns -1 and sets errno in case of error.
 */
static int connect_to_server(struct in_addr *addr, int port) {
    struct sockaddr_in sa;
    int sfd;

    if((sfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	return(-1);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr.s_addr, addr, sizeof(struct in_addr));
    if(connect(sfd, (struct sockaddr *)(&sa), sizeof(sa)) < 0) {
	close(sfd);
	return(-1);
    }
    return sfd;
}

/*
 * Construct a string representation of an expected state bitmap.
 */
static char *unparse_state_set(int set) {
    static char buf[100];
    buf[0] = '\0';
    strcat(buf, "{ ");
    for(int i = 0; i < NUM_STATES; i++) {
	if(set & (1<<i) || set & (1<<(i+RESYNC))) {
	    strcat(buf, tu_state_names[i]);
	    if(set & (1<<(i+RESYNC)))
		strcat(buf, "*");
	    strcat(buf, " ");
	}
    }
    strcat(buf, "}");
    return buf;
}

/*
 * Trim EOL characters from the end of a message.
 */
static void trim_eol(char *msg) {
    for(char *mp = msg; *mp != '\0'; mp++) {
	if(*mp == '\n' || *mp == '\r')
	    *mp = '\0';
    }
}

/*
 * Construct a timestamp string for tracing printout.
 */
static char *timestamp() {
    static char buf[100];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    sprintf(buf, "%ld.%06ld", tv.tv_sec, tv.tv_usec);
    return buf;
}
