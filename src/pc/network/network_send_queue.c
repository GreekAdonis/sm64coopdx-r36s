#include <pthread.h>
#include <string.h>
#include <zlib.h>

#include "socket/socket.h"  // SOCKET_ERROR, per platform
#include "coopnet/coopnet.h"
#include "network.h"
#include "network_send_queue.h"
#include "packets/packet.h"
#include "pc/configfile.h"
#include "pc/debuglog.h"

// One worker draining a FIFO, so queued packets leave in exactly the order the
// main thread produced them. That is the whole reason this is a queue and not a
// thread pool: the reliable and ordered packet layers assume a sequence.
//
// Note what that does and does not guarantee. The main thread still sends
// synchronously for packets the queue refuses (see network_send_queue_push),
// so two threads can be inside gNetworkSystem->send() at once, and a refused
// packet can overtake queued ones. Neither is new to this file: the transport
// is UDP, so the protocol already has to tolerate reordering and does, via
// sequence ids on reliable packets and orderedGroupId on ordered ones.
//
// The queue is deliberately statically sized rather than heap allocated -- it
// is roughly 380KB, which is worth spending on a handheld to avoid an
// allocation on the send path.

#define SEND_QUEUE_SLOTS 128

struct SendSlot {
    u8  localIndex;
    u16 dataLength;
    u8  buffer[PACKET_LENGTH];
};

static struct SendSlot sQueue[SEND_QUEUE_SLOTS];
static u32 sHead = 0; // producer: main thread
static u32 sTail = 0; // consumer: worker

static pthread_t       sThread;
static pthread_mutex_t sMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  sCond  = PTHREAD_COND_INITIALIZER;
static bool sRunning = false;
static bool sStopping = false;

// The worker's own compression scratch. Deliberately not the static buffer in
// packet.c: that one is shared with packet_decompress(), which the main thread
// runs as packets arrive.
static u8 sWorkerCompBuffer[PACKET_LENGTH * 2];

// Only the socket backend may be sent to from the worker.
//
// Socket is safe enough: its send resolves localIndex against gNetworkPlayers
// and a static sockaddr array, so racing the main thread's join/disconnect
// bookkeeping can at worst produce a torn read of a fixed-size address -- one
// packet sent nowhere useful, which the receiver rejects. Bounded, no
// corruption.
//
// CoopNet is not safe, and this is checked rather than assumed. In
// coop-deluxe/coopnet @9d9b3dd, coopnet_send_to() reaches Client::PeerSendTo(),
// which does `mPeers[aPeerId]` -- std::map::operator[], which default-inserts
// when the key is missing, so it *mutates* the map. The same map is assigned at
// client.cpp:146, erased at :154 and cleared at :168 as peer events are
// processed on the main thread, and the only lock_guard in that file (:103)
// guards mEventsMutex, the event queue, not mPeers. Sending from here while the
// main thread pumps coopnet would be unsynchronised concurrent insert and erase
// on a std::map: undefined behaviour, and the kind that corrupts a tree rather
// than politely failing.
static bool network_send_queue_backend_is_safe(void) {
    return gNetworkSystem == &gNetworkSystemSocket;
}

static void network_send_queue_drain_one(struct SendSlot* slot) {
    struct Packet p = { 0 };
    p.localIndex = slot->localIndex;
    p.dataLength = slot->dataLength;
    p.addr = NULL;
    memcpy(p.buffer, slot->buffer, slot->dataLength + sizeof(u32));

    u8* buffer = NULL;
    u32 len = 0;
    packet_compress_into(&p, sWorkerCompBuffer, sizeof(sWorkerCompBuffer), &buffer, &len);
    if (!buffer || len == 0) {
        LOG_ERROR("Failed to compress on send thread!");
        return;
    }

    int rc = gNetworkSystem->send(slot->localIndex, NULL, buffer, len);
    if (rc == SOCKET_ERROR) { LOG_ERROR("send error %d (send thread)", rc); }
}

static void* network_send_queue_thread(UNUSED void* arg) {
    while (true) {
        struct SendSlot slot;

        pthread_mutex_lock(&sMutex);
        while (sHead == sTail && !sStopping) {
            pthread_cond_wait(&sCond, &sMutex);
        }
        if (sHead == sTail && sStopping) {
            pthread_mutex_unlock(&sMutex);
            break;
        }
        // Copy the slot out and free it immediately, so the producer may reuse
        // it while we do the slow part with the lock released.
        slot = sQueue[sTail];
        sTail = (sTail + 1) % SEND_QUEUE_SLOTS;
        pthread_mutex_unlock(&sMutex);

        network_send_queue_drain_one(&slot);
    }
    return NULL;
}

void network_send_queue_start(void) {
    if (sRunning) { return; }
    if (!configNetworkThreaded) { return; }

    if (!network_send_queue_backend_is_safe()) {
        LOG_INFO("network_threaded is set, but the active network backend cannot"
                 " be sent to off-thread; sending synchronously instead");
        return;
    }

    sHead = sTail = 0;
    sStopping = false;

    if (pthread_create(&sThread, NULL, network_send_queue_thread, NULL) != 0) {
        LOG_ERROR("could not start network send thread; sending synchronously");
        return;
    }
    sRunning = true;
    LOG_INFO("network send thread started");
}

void network_send_queue_stop(void) {
    if (!sRunning) { return; }

    // Drain rather than discard: whatever is queued has already been counted as
    // sent by the reliable layer and by the rate limiter.
    pthread_mutex_lock(&sMutex);
    sStopping = true;
    pthread_cond_signal(&sCond);
    pthread_mutex_unlock(&sMutex);

    pthread_join(sThread, NULL);
    sRunning = false;
    sHead = sTail = 0;
    LOG_INFO("network send thread stopped");
}

bool network_send_queue_push(u8 localIndex, struct Packet* p) {
    if (!sRunning || sStopping) { return false; }

    // Re-checked per packet, not just at start: network_set_system() can swap
    // the backend under a running session, and sending one packet into CoopNet
    // from this thread is one too many.
    if (!network_send_queue_backend_is_safe()) { return false; }

    // Packets carrying their own address are reliable resends, whose addr is
    // heap memory owned by the reliable node and freed when that node is
    // released (see packet_reliable.c). The queue has no way to copy it without
    // knowing the backend's address size, so those keep going out synchronously.
    // They are a small share of traffic -- first sends do not take this path.
    if (p->addr != NULL) { return false; }

    if (p->dataLength + sizeof(u32) > PACKET_LENGTH) { return false; }

    pthread_mutex_lock(&sMutex);

    u32 next = (sHead + 1) % SEND_QUEUE_SLOTS;
    if (next == sTail) {
        // Full. Fall back to a synchronous send rather than dropping: a dropped
        // reliable packet would be resent, but a dropped unreliable one is a
        // silent hole in the stream.
        pthread_mutex_unlock(&sMutex);
        return false;
    }

    struct SendSlot* slot = &sQueue[sHead];
    slot->localIndex = localIndex;
    slot->dataLength = p->dataLength;
    memcpy(slot->buffer, p->buffer, p->dataLength + sizeof(u32));
    sHead = next;

    pthread_cond_signal(&sCond);
    pthread_mutex_unlock(&sMutex);
    return true;
}
