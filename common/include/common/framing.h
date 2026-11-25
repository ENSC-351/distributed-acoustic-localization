// framing.h
// Framing module: sends and receives whole protocol messages over a TCP
// socket.
// TCP is a byte stream with no message boundaries, so one message can arrive
// split across several recv() calls, or merged with the next message. A
// Framing_reader_t collects the bytes from one socket and hands them back one
// complete message at a time, so every connection needs its own reader.

#ifndef FRAMING_H
#define FRAMING_H

#include "common/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
	uint8_t buffer[PROTO_MAX_MSG_LEN];
	size_t numBytesHeld;
	// Size of the message handed out last. It stays in the buffer (so the
	// caller's payload pointer is still valid) until the next reader call.
	size_t numBytesToDiscard;
} Framing_reader_t;

typedef enum {
	FRAMING_MESSAGE_READY,
	FRAMING_NEED_MORE_BYTES,
	// The rest are errors. All of them mean the stream can't be trusted, so
	// the connection should be closed.
	FRAMING_BAD_MAGIC,      // stream is out of sync
	FRAMING_BAD_VERSION,    // header is filled in, so the master can reply first
	FRAMING_BAD_LENGTH,     // payload_len is over PROTO_MAX_PAYLOAD
} Framing_result_t;

// Call before using a reader, and again for every new connection.
void Framing_initReader(Framing_reader_t *pReader);

// Reads whatever the socket has ready, with one recv() call. Returns the
// number of bytes read, 0 if the other side closed the connection, or -1 on
// an error (check errno).
// Only call this after Framing_getMessage() has returned
// FRAMING_NEED_MORE_BYTES, so that the buffer is never full.
ssize_t Framing_readFromSocket(Framing_reader_t *pReader, int socketFd);

// Takes the next complete message out of the reader. On FRAMING_MESSAGE_READY,
// *ppPayload points at pHeader->payload_len bytes, which stay valid until the
// next call to either reader function.
// One read can hold several messages, so keep calling this until it returns
// FRAMING_NEED_MORE_BYTES.
Framing_result_t Framing_getMessage(Framing_reader_t *pReader,
                                    struct proto_header *pHeader,
                                    const uint8_t **ppPayload);

// Packs pHeader into the first PROTO_HEADER_LEN bytes of pMessage, then sends
// it along with the pHeader->payload_len bytes of payload that the caller has
// already packed at pMessage + PROTO_HEADER_LEN. Blocks until it's all sent.
// Returns false (with errno set) if the connection failed.
bool Framing_sendMessage(int socketFd,
                         uint8_t *pMessage,
                         const struct proto_header *pHeader);

#endif
