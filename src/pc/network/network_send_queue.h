#ifndef NETWORK_SEND_QUEUE_H
#define NETWORK_SEND_QUEUE_H

#include <stdbool.h>
#include "types.h"

struct Packet;

// Optional worker that takes packet compression and the send syscall off the
// main thread. Off unless network_threaded is set in sm64config.txt.
//
// Measured on the RK3326 in a nine-player room: compression averaged 2.5ms per
// frame and the send syscall 1.5ms, but the tail is what matters -- of a 32.2ms
// p99 network spike, 26.1ms was those two. Those spikes land on the main thread
// as frame hitches, and hitches are what a slow client turns into desync.

void network_send_queue_start(void);
void network_send_queue_stop(void);

// Takes ownership of a copy of the packet and returns true, or returns false if
// the caller should send it synchronously instead. Returning false is normal and
// safe: it happens when the worker is not running, when the queue is full, and
// for packets carrying their own destination address, whose lifetime the queue
// cannot guarantee.
bool network_send_queue_push(u8 localIndex, struct Packet* p);

#endif // NETWORK_SEND_QUEUE_H
