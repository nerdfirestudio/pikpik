#ifndef PC_SYNC_NET_H
#define PC_SYNC_NET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal synchronous HTTP for the save-sync client (pc_sync_net.mm).
//
// Synchronous on purpose. Every caller is either the boot reconcile (before
// the game loop exists), the sync worker thread, or the iOS background
// callback — none of them can make use of a completion handler, and all three
// genuinely want to block until the answer arrives.
//
// Adapted from the prior port's pc_sync_net_nsurl.mm.

typedef struct {
	long status;         // HTTP status; 0 on transport failure
	unsigned char* body; // NUL-terminated for convenience; may hold binary
	size_t len;
} PcSyncHttpResp;

// Returns 0 and fills resp on any HTTP response (including 4xx/5xx); -1 on
// transport failure (no connection, timeout, bad URL). Headers are "Name: value".
int pc_sync_http(const char* method, const char* url, const char* const* headers, int n_headers, const void* body,
                 size_t body_len, int timeout_ms, PcSyncHttpResp* resp);

void pc_sync_http_free(PcSyncHttpResp* resp);

#ifdef __cplusplus
}
#endif

#endif // PC_SYNC_NET_H
