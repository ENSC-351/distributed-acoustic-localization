#include "udp.h"
#include "dashboard.h"
#include "demo_source.h"
#include "stop.h"
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PROTOCOL 0
#define MAX_PACKET_SIZE 1500
// Status JSON (nodes + fix history) is a few KB; loopback UDP allows up to 64 KB.
#define STATUS_REPLY_SIZE 8192
#define NO_FLAGS 0
#define WORD_SEPARATORS " \t\r\n"
#define UNKNOWN_COMMAND "Unknown command.\n"
#define STOP_REPLY "Program terminating.\n"
#define HELP_REPLY "Commands: status, sound, help, stop\n"
#define SOUND_REPLY "ok"
#define NOT_DEMO_REPLY "Not in demo mode.\n"
// How often the thread wakes to check whether UDP_cleanup() wants it to end
#define RECEIVE_TIMEOUT_US 200000

static int socketFd = -1;
static pthread_t udpThread;
static bool isInitialized = false;
static atomic_bool isRunning = false;

/**
 * @brief Creates the UDP socket, binds it to UDP_PORT, sets a receive timeout
 */
static void openSocket(void)
{
    socketFd = socket(AF_INET, SOCK_DGRAM, DEFAULT_PROTOCOL);
    if (socketFd < 0) {
        perror("Unable to create UDP socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(UDP_PORT);

    if (bind(socketFd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("Unable to bind UDP port");
        close(socketFd);
        exit(EXIT_FAILURE);
    }

    // Without a timeout, recvfrom() would block forever and a stop from
    // Ctrl+C could never join this thread.
    struct timeval timeout = {.tv_sec = 0, .tv_usec = RECEIVE_TIMEOUT_US};
    if (setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        perror("Unable to set UDP receive timeout");
        close(socketFd);
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Sends a '\0' terminated text reply back to the client
 * @param message text to send
 * @param sender address of the client that sent the command
 */
static void sendReply(const char *message, const struct sockaddr_in *sender)
{
    sendto(socketFd, message, strlen(message), NO_FLAGS,
           (const struct sockaddr *)sender, sizeof(*sender));
}

/* Command handlers */
/**
 * @brief Function signature shared by every UDP command handler
 * @details Each handler applies its command (if any) and sends one reply
 * @param arg Text after the command word
 * @param sender address of the client that sent the command
 * @return true to keep the UDP server running, false to end it
 */
typedef bool (*CommandHandler)(const char *arg, const struct sockaddr_in *sender);

/**
 * @brief Pairs a command word with its handler
 */
typedef struct {
    const char *name;
    CommandHandler handler;
} Command;

/**
 * @brief "status": replies with the dashboard as JSON
 */
static bool handleStatusCommand(const char *arg, const struct sockaddr_in *sender)
{
    (void)arg;
    char reply[STATUS_REPLY_SIZE];
    if (!Dashboard_toJson(reply, sizeof(reply))) {
        fprintf(stderr, "Dashboard JSON did not fit in the reply buffer\n");
    }
    sendReply(reply, sender);
    return true;
}

/**
 * @brief "sound": demo mode only, makes a pretend sound happen now
 */
static bool handleSoundCommand(const char *arg, const struct sockaddr_in *sender)
{
    (void)arg;
    sendReply(DemoSource_trigger() ? SOUND_REPLY : NOT_DEMO_REPLY, sender);
    return true;
}

/**
 * @brief "help": replies with the list of commands
 */
static bool handleHelpCommand(const char *arg, const struct sockaddr_in *sender)
{
    (void)arg;
    sendReply(HELP_REPLY, sender);
    return true;
}

/**
 * @brief "stop": replies "Program terminating." and requests the program to stop
 * @return false to end the receive loop
 */
static bool handleStopCommand(const char *arg, const struct sockaddr_in *sender)
{
    (void)arg;
    sendReply(STOP_REPLY, sender);
    Stop_request();
    return false;
}

/* Command LUT */
static const Command Commands[] = {
    {"status", handleStatusCommand},
    {"sound", handleSoundCommand},
    {"help", handleHelpCommand},
    {"stop", handleStopCommand},
};

#define NUM_COMMANDS (sizeof(Commands) / sizeof(Commands[0]))

/**
 * @brief Looks up the first word of a packet in the command LUT
 * @param word Start of the received command
 * @param wordLength Number of characters in the first word
 * @return Pointer to the matching LUT entry, or NULL if unknown
 */
static const Command *findCommand(const char *word, size_t wordLength)
{
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        const char *name = Commands[i].name;
        bool sameLength = (strlen(name) == wordLength);
        bool sameCharacters = (strncasecmp(word, name, wordLength) == 0);
        if (sameLength && sameCharacters) {
            return &Commands[i];
        }
    }
    return NULL;
}

/**
 * @brief Runs the command in one received packet and replies to the sender
 * @param command '\0' terminated packet text
 * @param sender address of the client that sent the command
 * @return false if the server should stop, true otherwise
 */
static bool handleIncomingCommand(const char *command, const struct sockaddr_in *sender)
{
    size_t wordLength = strcspn(command, WORD_SEPARATORS);
    const Command *toRun = findCommand(command, wordLength);
    if (toRun == NULL) {
        sendReply(UNKNOWN_COMMAND, sender);
        return true;
    }
    const char *arg = command + wordLength;
    return toRun->handler(arg, sender);
}

/**
 * @brief UDP server thread: receives one command per packet and replies
 * @param arg Unused
 */
static void *udpThreadFn(void *arg)
{
    (void)arg;
    char command[MAX_PACKET_SIZE + 1];

    while (atomic_load(&isRunning)) {
        struct sockaddr_in sender;
        socklen_t senderLength = sizeof(sender);
        ssize_t numBytes = recvfrom(socketFd, command, MAX_PACKET_SIZE, NO_FLAGS,
                                    (struct sockaddr *)&sender, &senderLength);
        if (numBytes < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                perror("UDP receive failed");
            }
            continue;
        }
        command[numBytes] = '\0';
        if (!handleIncomingCommand(command, &sender)) {
            atomic_store(&isRunning, false);
        }
    }
    return NULL;
}

void UDP_init(void)
{
    if (isInitialized) {
        return;
    }
    openSocket();
    atomic_store(&isRunning, true);
    int err = pthread_create(&udpThread, NULL, udpThreadFn, NULL);
    if (err != 0) {
        fprintf(stderr, "Unable to create UDP thread: %s\n", strerror(err));
        exit(EXIT_FAILURE);
    }
    isInitialized = true;
}

void UDP_cleanup(void)
{
    if (!isInitialized) {
        return;
    }
    atomic_store(&isRunning, false);
    pthread_join(udpThread, NULL);
    close(socketFd);
    socketFd = -1;
    isInitialized = false;
}
