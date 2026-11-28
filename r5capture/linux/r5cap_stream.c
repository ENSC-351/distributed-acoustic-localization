// r5cap_stream.c
// Streams the R5's capture ring over TCP, for tools/live_spectrum.py on a PC.
// One client at a time. The R5 never waits for it: a slow client loses blocks,
// and is told how many. The R5 must be sampling (r5cap_ctl start).
//
// Usage on the BeagleY-AI (root, for /dev/mem):
//   sudo ./r5cap_stream [address]     default 192.168.7.2 (the USB network only);
//                                     0.0.0.0 = every network, unencrypted
// Testing without a board: R5CAP_MEM=<file> is used instead of /dev/mem.
#include "r5cap_heartbeat.h"
#include "r5cap_reader.h"
#include "r5cap_shared.h"
#include "r5cap_stream.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_ADDRESS "192.168.7.2"
#define POLL_US 2000
#define WAIT_US 200000
#define SEND_TIMEOUT_S 5

static volatile sig_atomic_t keepRunning = 1;

static void handleSignal(int signum)
{
    (void)signum;
    keepRunning = 0;
}

static bool sendAll(int fd, const void *data, size_t length)
{
    const uint8_t *p = data;
    while (length > 0) {
        ssize_t sent = send(fd, p, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR && keepRunning) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        p += sent;
        length -= (size_t)sent;
    }
    return true;
}

// True if the client closed its end (it never sends anything)
static bool clientGone(int fd)
{
    uint8_t byte;
    ssize_t n = recv(fd, &byte, 1, MSG_DONTWAIT | MSG_PEEK);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

// Streams until the client leaves, the R5 stops, or it restarts (the ring then has
// new settings, so the client must reconnect)
static void serveClient(int fd, void *region)
{
    const volatile struct r5cap_header *h = region;
    struct R5CapReader reader;
    while (keepRunning && !(R5CAPREADER_open(&reader, region, R5CAP_SHM_SIZE) &&
                            h->state == R5CAP_STATE_RUNNING)) {
        if (clientGone(fd)) {
            return;
        }
        usleep(WAIT_US);
    }
    if (!keepRunning) {
        return;
    }

    struct r5cap_stream_header header = {
        .magic = R5CAP_STREAM_MAGIC,
        .version = R5CAP_STREAM_VERSION,
        .numChannels = h->numChannels,
        .blockFrames = h->blockFrames,
        .tickHz = h->tickHz,
        .framePeriodTicks = h->framePeriodTicks,
        .channel1OffsetTicks = h->channelOffsetTicks[1],
    };
    size_t sampleBytes = r5cap_samplesPerBlock(header.numChannels, header.blockFrames) * sizeof(uint16_t);
    uint8_t *message = malloc(sizeof(struct r5cap_stream_block) + sampleBytes);
    if (message == NULL || !sendAll(fd, &header, sizeof(header))) {
        free(message);
        return;
    }

    while (keepRunning) {
        bool restarted = (int32_t)(h->writeSeq - reader.nextBlock) < 0 ||
                         h->framePeriodTicks != header.framePeriodTicks;
        struct R5CapBlockInfo info;
        uint16_t *samples = (uint16_t *)(message + sizeof(struct r5cap_stream_block));
        enum R5CapReadResult result = restarted ? R5CAP_READ_STOPPED
                                                : R5CAPREADER_read(&reader, samples, &info);
        if (result == R5CAP_READ_STOPPED) {
            break;
        }
        if (result == R5CAP_READ_NONE) {
            if (clientGone(fd)) {
                break;
            }
            usleep(POLL_US);
            continue;
        }
        struct r5cap_stream_block block = {
            .blockNumber = info.blockNumber, .flags = info.flags, .skipped = info.skipped };
        memcpy(message, &block, sizeof(block));
        if (!sendAll(fd, message, sizeof(block) + sampleBytes)) {
            break;
        }
    }
    free(message);
}

static int openServer(const char *address)
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(R5CAP_STREAM_PORT) };
    if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad IPv4 address: %s\n", address);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
        perror("unable to listen");
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    const char *address = (argc > 1) ? argv[1] : DEFAULT_ADDRESS;
    const char *memory = getenv("R5CAP_MEM");
    int memFd = open(memory != NULL ? memory : "/dev/mem", O_RDONLY | O_SYNC);   // O_SYNC: uncached mapping
    if (memFd < 0) {
        perror("open /dev/mem (run with sudo)");
        return EXIT_FAILURE;
    }
    void *region = mmap(NULL, R5CAP_SHM_SIZE, PROT_READ, MAP_SHARED, memFd, (off_t)R5CAP_SHM_PHYS);
    if (region == MAP_FAILED) {
        perror("mmap the r5capture shared region");
        return EXIT_FAILURE;
    }

    struct sigaction action = { .sa_handler = handleSignal };   // no SA_RESTART: accept() returns
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    int server = openServer(address);
    if (server < 0) {
        return EXIT_FAILURE;
    }
    printf("streaming on %s:%d (Ctrl+C to stop)\n", address, R5CAP_STREAM_PORT);
    fflush(stdout);

    while (keepRunning) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            continue;
        }
        struct timeval timeout = { .tv_sec = SEND_TIMEOUT_S };
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        printf("client connected\n");
        fflush(stdout);
        serveClient(client, region);
        close(client);
        printf("client disconnected\n");
        fflush(stdout);
    }
    close(server);
    return EXIT_SUCCESS;
}
