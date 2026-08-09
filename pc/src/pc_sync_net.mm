// pc_sync_net.mm — NSURLSession backend for pc_sync.cpp. See pc_sync_net.h.
//
// Objective-C++ so it rides the project's existing .mm compilation (pc_window.mm)
// rather than adding a libcurl dependency to an app that already links
// Foundation on both platforms.
//
// No ObjC object escapes the completion handler — the response bytes are
// copied into a malloc buffer inside it — so this file is ARC-agnostic and
// does not care that pc_window.mm is built with -fobjc-arc and it is not.
//
// malloc, not new: this can run from the boot reconcile in main(), before
// gsys->Initialise(), where the game's replaced operator new returns NULL
// (see the header comment in pc_paths.cpp for the full trap).
//
// Adapted from the prior port's pc_sync_net_nsurl.mm.

#include "pc_sync_net.h"

#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>

int pc_sync_http(const char* method, const char* url, const char* const* headers, int n_headers, const void* body,
                 size_t body_len, int timeout_ms, PcSyncHttpResp* resp)
{
	memset(resp, 0, sizeof(*resp));

	@autoreleasepool {
		NSString* urlStr = [NSString stringWithUTF8String:url];
		NSURL* u         = urlStr ? [NSURL URLWithString:urlStr] : nil;
		// A URL the user typed into settings.ini can be anything; NSURL returns
		// nil rather than throwing, and a nil host would otherwise reach
		// NSMutableURLRequest and raise.
		if (!u || !u.host) {
			return -1;
		}

		const NSTimeInterval secs = (NSTimeInterval)timeout_ms / 1000.0;

		NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
		req.HTTPMethod           = [NSString stringWithUTF8String:method];
		req.timeoutInterval      = secs;
		// The blob endpoints are the whole point of this client; a cached 200
		// would hand back yesterday's save.
		req.cachePolicy = NSURLRequestReloadIgnoringLocalAndRemoteCacheData;

		for (int i = 0; i < n_headers; i++) {
			NSString* h = [NSString stringWithUTF8String:headers[i]];
			if (!h) {
				continue;
			}
			NSRange colon = [h rangeOfString:@": "];
			if (colon.location == NSNotFound) {
				continue;
			}
			[req setValue:[h substringFromIndex:colon.location + 2] forHTTPHeaderField:[h substringToIndex:colon.location]];
		}
		if (body && body_len) {
			req.HTTPBody = [NSData dataWithBytes:body length:body_len];
		}

		// A private ephemeral session rather than sharedSession, for one
		// reason that matters: timeoutIntervalForResource is a HARD cap on the
		// whole transfer, which makes "the completion handler always fires,
		// once" a guarantee rather than a hope. That is what lets the wait
		// below be unbounded — and an unbounded wait is what keeps the
		// response buffer's ownership simple, with no abandoned-waiter case
		// where a late handler writes into storage nobody will free.
		//
		// Ephemeral also means no on-disk cache or cookie store, which is
		// right for a save-blob endpoint and costs nothing at this call rate.
		NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		cfg.timeoutIntervalForRequest  = secs;
		cfg.timeoutIntervalForResource = secs;
		NSURLSession* session          = [NSURLSession sessionWithConfiguration:cfg];

		dispatch_semaphore_t sem   = dispatch_semaphore_create(0);
		__block long status        = 0;
		__block unsigned char* buf = NULL;
		__block size_t buf_len     = 0;
		__block int alloc_failed   = 0;

		NSURLSessionDataTask* task = [session
		    dataTaskWithRequest:req
		      completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
			      if (!error && [response isKindOfClass:[NSHTTPURLResponse class]]) {
				      status    = (long)((NSHTTPURLResponse*)response).statusCode;
				      size_t n  = data ? (size_t)data.length : 0;
				      buf       = (unsigned char*)malloc(n + 1);
				      if (buf) {
					      if (n) {
						      memcpy(buf, data.bytes, n);
					      }
					      buf[n]  = '\0';
					      buf_len = n;
				      } else {
					      alloc_failed = 1;
				      }
			      }
			      dispatch_semaphore_signal(sem);
		      }];
		[task resume];
		// Release the session once its one task finishes. Without this the
		// session object is retained forever and each call leaks one.
		[session finishTasksAndInvalidate];

		dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

		if (status == 0 || alloc_failed || !buf) {
			free(buf);
			return -1;
		}
		resp->status = status;
		resp->body   = buf;
		resp->len    = buf_len;
		return 0;
	}
}

void pc_sync_http_free(PcSyncHttpResp* resp)
{
	free(resp->body);
	resp->body   = NULL;
	resp->len    = 0;
	resp->status = 0;
}
