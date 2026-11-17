// framing.c
// Framing module: sends and receives whole protocol messages over a TCP
// socket.

#include "common/framing.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>

static void discardHandedOutMessage(Framing_reader_t *pReader);
static Framing_result_t toFramingResult(enum proto_result headerResult);
static bool sendAll(int socketFd, const uint8_t *pData, size_t numBytes);

void Framing_initReader(Framing_reader_t *pReader)
{
	assert(pReader != NULL);
	pReader->numBytesHeld = 0;
	pReader->numBytesToDiscard = 0;
}

ssize_t Framing_readFromSocket(Framing_reader_t *pReader, int socketFd)
{
	assert(pReader != NULL);
	discardHandedOutMessage(pReader);

	// The header check caps a message at PROTO_MAX_MSG_LEN, which is the
	// buffer size. If the caller has taken every complete message (like the
	// header says to), what's left is part of one message, so there is
	// always room for more.
	size_t numBytesFree = sizeof(pReader->buffer) - pReader->numBytesHeld;
	assert(numBytesFree > 0);

	ssize_t numBytesRead = recv(socketFd,
	                            pReader->buffer + pReader->numBytesHeld,
	                            numBytesFree,
	                            0);
	if (numBytesRead > 0) {
		pReader->numBytesHeld += (size_t)numBytesRead;
	}
	return numBytesRead;
}

Framing_result_t Framing_getMessage(Framing_reader_t *pReader,
                                    struct proto_header *pHeader,
                                    const uint8_t **ppPayload)
{
	assert(pReader != NULL && pHeader != NULL && ppPayload != NULL);
	discardHandedOutMessage(pReader);

	// We can't know how long the message is until the whole header is here
	if (pReader->numBytesHeld < PROTO_HEADER_LEN) {
		return FRAMING_NEED_MORE_BYTES;
	}

	Framing_result_t result = toFramingResult(proto_unpack_header(pReader->buffer, pHeader));
	if (result == FRAMING_MESSAGE_READY) {
		size_t messageLen = PROTO_HEADER_LEN + pHeader->payload_len;
		if (pReader->numBytesHeld < messageLen) {
			result = FRAMING_NEED_MORE_BYTES;
		} else {
			// The message stays in the buffer until the next reader call, so
			// the caller's payload pointer stays valid until then
			*ppPayload = pReader->buffer + PROTO_HEADER_LEN;
			pReader->numBytesToDiscard = messageLen;
		}
	}
	return result;
}

bool Framing_sendMessage(int socketFd,
                         uint8_t *pMessage,
                         const struct proto_header *pHeader)
{
	assert(pMessage != NULL && pHeader != NULL);
	assert(pHeader->payload_len <= PROTO_MAX_PAYLOAD);
	proto_pack_header(pMessage, pHeader);
	return sendAll(socketFd, pMessage, PROTO_HEADER_LEN + pHeader->payload_len);
}

// Shifts any bytes after the last message handed out down to the start of
// the buffer, so the next message always starts at buffer[0].
static void discardHandedOutMessage(Framing_reader_t *pReader)
{
	if (pReader->numBytesToDiscard > 0) {
		size_t numBytesLeft = pReader->numBytesHeld - pReader->numBytesToDiscard;
		// memmove() instead of memcpy() because the two ranges can overlap
		memmove(pReader->buffer,
		        pReader->buffer + pReader->numBytesToDiscard,
		        numBytesLeft);
		pReader->numBytesHeld = numBytesLeft;
		pReader->numBytesToDiscard = 0;
	}
}

// Turns the result of proto_unpack_header() into the reader's own result, so
// callers only have to deal with one set of values.
static Framing_result_t toFramingResult(enum proto_result headerResult)
{
	Framing_result_t result = FRAMING_BAD_MAGIC;
	switch (headerResult) {
	case PROTO_OK:
		result = FRAMING_MESSAGE_READY;
		break;
	case PROTO_ERR_MAGIC:
		result = FRAMING_BAD_MAGIC;
		break;
	case PROTO_ERR_VERSION:
		result = FRAMING_BAD_VERSION;
		break;
	case PROTO_ERR_LEN:
		result = FRAMING_BAD_LENGTH;
		break;
	default:
		// proto_unpack_header() only returns the values above, so anything
		// else is our bug
		assert(false);
	}
	return result;
}

// send() can return before it has sent everything (for example, when the
// socket's send buffer fills up), so we keep going until it's all out.
// Returns false if the connection failed.
static bool sendAll(int socketFd, const uint8_t *pData, size_t numBytes)
{
	size_t numBytesSent = 0;
	bool isConnected = true;
	while (isConnected && numBytesSent < numBytes) {
		// MSG_NOSIGNAL makes a closed socket return EPIPE, instead of killing
		// the whole process with SIGPIPE
		ssize_t result = send(socketFd,
		                      pData + numBytesSent,
		                      numBytes - numBytesSent,
		                      MSG_NOSIGNAL);
		if (result >= 0) {
			numBytesSent += (size_t)result;
		} else if (errno != EINTR) {
			isConnected = false;
		}
	}
	return isConnected;
}
