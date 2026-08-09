// pc_sync.cpp — save sync across the user's own devices. See pc_sync.h.
//
// Model: card A is a checkout of a server-side blob.
//
//   boot      enroll (idempotent) -> take the lease -> reconcile
//   play      every card mutation queues an upload; a worker uploads and
//             heartbeats the lease
//   leave     upload if changed, release the lease
//
// The server (the prior port/sync-server) never parses the blob, and its lock is
// per-device-per-town — which is exactly this case: three devices sharing one
// save, where the SAME device may retake its own lease after a crash but any
// other device gets a 409.
//
// NOTHING HERE USES operator new. pc_sync_startup runs from main() before
// gsys->Initialise(), and the game replaces operator new with one that returns
// NULL until the arena exists — with -fno-exceptions a std::string then
// memmoves into a null pointer and it reads as a libc fault. Fixed buffers and
// malloc, same rule as pc_paths.cpp and pc_card.cpp.
//
// NOTHING DESTROYS LOCAL DATA. Every path that replaces card A first writes
// the current contents to save/sync/ as a .pks pack. The rule inherited from
// the prior port's client, and worth keeping: a sync bug should cost disk, not a
// save file.

#include "pc_sync.h"

#include "pc_config.h"
#include "pc_sync_net.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern "C" const char* pc_card_root_public(void); // pc_card.cpp

// ---------------------------------------------------------------- constants

#define PC_SYNC_MAX_FILES 64 // card A holds one save; the cap is paranoia

// Boot is on the critical path to the title screen, so it gets a short
// timeout: a server that is off costs a couple of seconds and we play
// offline. The worker is not on anyone's critical path and can afford to
// wait out a slow link.
#define PC_SYNC_BOOT_TIMEOUT_MS 4000
#define PC_SYNC_API_TIMEOUT_MS 15000
#define PC_SYNC_BLOB_TIMEOUT_MS 45000
// iOS gives ~5s of background execution before the watchdog, extended to ~30s
// by the UIApplication background task pc_window.mm opens around this.
#define PC_SYNC_SUSPEND_TIMEOUT_MS 8000

#define PC_SYNC_HEARTBEAT_SEC 60
#define PC_SYNC_RETRY_SEC 30
// A save is several CARD calls in sequence (write temp, delete old, rename).
// Uploading between them would publish a torn card, so an upload waits for
// the card to go quiet. The real protocol's steps are milliseconds apart;
// three seconds means it has finished.
#define PC_SYNC_QUIET_SEC 3

#define PC_SYNC_PACK_MAGIC "PKMNCARD"
#define PC_SYNC_PACK_VERSION 1u

// ------------------------------------------------------------------- state

enum PcSyncState {
	PC_SYNC_OFF = 0,  // not configured
	PC_SYNC_OFFLINE,  // configured, server unreachable
	PC_SYNC_LOCKED,   // another device holds the save
	PC_SYNC_SYNCED,   // we hold the lease
};

static int s_configured;
static char s_server[192];
static char s_auth_hdr[192];
static char s_id_hex[65];
static char s_auth_secret_hex[65];
static char s_device_id[17];
static char s_device_name[48];
static char s_friend_code[16];

static PcSyncState s_state;
static char s_lock_id[48];
static char s_last_snapshot[48]; // server snapshot id as of our last sync
static char s_last_sha[65];      // pack hash as of our last sync
static char s_status[128] = "off";

static int s_started;
static int s_shutdown_done;
static int s_suspended;

// s_mtx guards the small mutable state above; s_net_mtx serializes whole
// network+pack SEQUENCES so the worker, the lifecycle callbacks and shutdown
// cannot interleave two conversations with the server.
static pthread_mutex_t s_mtx     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_net_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_worker;
static int s_worker_started;
static int s_stop;
static int s_dirty;        // card changed since the last upload
static time_t s_last_change;
static time_t s_last_hb;
static time_t s_retry_at;

static void pc_sync_set_status(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	pthread_mutex_lock(&s_mtx);
	vsnprintf(s_status, sizeof(s_status), fmt, ap);
	pthread_mutex_unlock(&s_mtx);
	va_end(ap);
}

// ------------------------------------------------------------------ sha256
// Compact single-shot SHA-256, ported from the prior port's pc_sync.c. Used for the
// identity digests (a few dozen bytes) and the pack hash (~150 KB), both off
// the hot path.

struct PcSha256Ctx {
	uint8_t data[64];
	uint32_t datalen;
	uint64_t bitlen;
	uint32_t state[8];
};

#define PC_ROTR(a, b) (((a) >> (b)) | ((a) << (32 - (b))))

static const uint32_t s_sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void pc_sha256_transform(PcSha256Ctx* ctx, const uint8_t data[])
{
	uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
	int i, j;

	for (i = 0, j = 0; i < 16; i++, j += 4) {
		m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) | ((uint32_t)data[j + 2] << 8)
		     | (uint32_t)data[j + 3];
	}
	for (; i < 64; i++) {
		uint32_t s0 = PC_ROTR(m[i - 15], 7) ^ PC_ROTR(m[i - 15], 18) ^ (m[i - 15] >> 3);
		uint32_t s1 = PC_ROTR(m[i - 2], 17) ^ PC_ROTR(m[i - 2], 19) ^ (m[i - 2] >> 10);
		m[i]        = m[i - 16] + s0 + m[i - 7] + s1;
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		uint32_t S1  = PC_ROTR(e, 6) ^ PC_ROTR(e, 11) ^ PC_ROTR(e, 25);
		uint32_t ch  = (e & f) ^ (~e & g);
		uint32_t S0  = PC_ROTR(a, 2) ^ PC_ROTR(a, 13) ^ PC_ROTR(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		t1           = h + S1 + ch + s_sha_k[i] + m[i];
		t2           = S0 + maj;
		h            = g;
		g            = f;
		f            = e;
		e            = d + t1;
		d            = c;
		c            = b;
		b            = a;
		a            = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

static void pc_sha256_init(PcSha256Ctx* ctx)
{
	ctx->datalen = 0;
	ctx->bitlen  = 0;
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
}

static void pc_sha256_update(PcSha256Ctx* ctx, const void* in, size_t len)
{
	const uint8_t* p = (const uint8_t*)in;
	for (size_t i = 0; i < len; i++) {
		ctx->data[ctx->datalen++] = p[i];
		if (ctx->datalen == 64) {
			pc_sha256_transform(ctx, ctx->data);
			ctx->bitlen += 512;
			ctx->datalen = 0;
		}
	}
}

static void pc_sha256_final(PcSha256Ctx* ctx, uint8_t out[32])
{
	ctx->bitlen += (uint64_t)ctx->datalen * 8;
	uint32_t i = ctx->datalen;
	ctx->data[i++] = 0x80;
	if (i > 56) {
		while (i < 64) {
			ctx->data[i++] = 0;
		}
		pc_sha256_transform(ctx, ctx->data);
		i = 0;
	}
	while (i < 56) {
		ctx->data[i++] = 0;
	}
	for (int b = 7; b >= 0; b--) {
		ctx->data[i++] = (uint8_t)(ctx->bitlen >> (b * 8));
	}
	pc_sha256_transform(ctx, ctx->data);

	for (i = 0; i < 8; i++) {
		out[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

static void pc_sha256(const void* in, size_t len, uint8_t out[32])
{
	PcSha256Ctx ctx;
	pc_sha256_init(&ctx);
	pc_sha256_update(&ctx, in, len);
	pc_sha256_final(&ctx, out);
}

static void pc_hex(const uint8_t* in, size_t n, char* out)
{
	static const char* hexd = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[i * 2]     = hexd[in[i] >> 4];
		out[i * 2 + 1] = hexd[in[i] & 15];
	}
	out[n * 2] = '\0';
}

// hex(sha256(domain || text)) — domain separation so the id, the auth secret
// and the friend code derived from ONE user phrase cannot be turned into each
// other. The server only ever stores sha256(auth), so a leak of its state
// file yields nothing usable.
static void pc_sync_digest_hex(const char* domain, const char* text, char out[65])
{
	PcSha256Ctx ctx;
	uint8_t digest[32];
	pc_sha256_init(&ctx);
	pc_sha256_update(&ctx, domain, strlen(domain));
	pc_sha256_update(&ctx, text, strlen(text));
	pc_sha256_final(&ctx, digest);
	pc_hex(digest, 32, out);
}

// --------------------------------------------------------------- small io

static int pc_sync_is_dir(const char* p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char* pc_sync_dir(void)
{
	static char dir[1024];
	if (!dir[0]) {
		snprintf(dir, sizeof(dir), "%s/sync", pc_card_root_public());
	}
	if (!pc_sync_is_dir(dir)) {
		mkdir(dir, 0755);
	}
	return dir;
}

static void pc_sync_card_dir(char* out, size_t n) { snprintf(out, n, "%s/card_a", pc_card_root_public()); }

static int pc_sync_write_file(const char* path, const void* data, size_t len)
{
	// Write-then-rename: a half-written card file is indistinguishable from a
	// corrupt save, and this runs on paths that replace a real one.
	char tmp[1200];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return 0;
	}
	int ok = (len == 0) || fwrite(data, 1, len, f) == len;
	if (fclose(f) != 0) {
		ok = 0;
	}
	if (!ok) {
		remove(tmp);
		return 0;
	}
	remove(path);
	return rename(tmp, path) == 0;
}

// ---------------------------------------------------------------- the pack
// One blob = all of card A. Both the .bin and its .stat sidecar go in: the
// sidecar carries the directory slot number, and syncing only the .bin would
// let fileNo drift between devices (pc_card.cpp addresses files by slot).
//
//   "PKMNCARD" u32 version u32 count
//   count x { u32 nameLen, name, u32 dataLen, data }
//
// Little-endian explicitly. Every device here is arm64 Apple so host order
// would work, but a format that crosses machines should not depend on that.

static void pc_put_u32(unsigned char* p, uint32_t v)
{
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
	p[2] = (unsigned char)((v >> 16) & 0xff);
	p[3] = (unsigned char)((v >> 24) & 0xff);
}

static uint32_t pc_get_u32(const unsigned char* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A card file is exactly "<name>.bin" or "<name>.stat" — the same exact-suffix
// rule pc_card.cpp's own directory scan uses, which is what keeps the rotated
// ".bakN" backups out of the pack.
static int pc_sync_is_card_file(const char* name)
{
	size_t n = strlen(name);
	if (n > 4 && strcmp(name + n - 4, ".bin") == 0) {
		return 1;
	}
	return n > 5 && strcmp(name + n - 5, ".stat") == 0;
}

// Packs card A. Returns a malloc'd buffer (caller frees) and sets *out_len,
// or NULL on failure. An EMPTY card packs to a valid header with count 0 and
// sets *out_files to 0 — the caller needs to tell "no save here" apart from
// "could not read the card".
static unsigned char* pc_sync_pack(size_t* out_len, int* out_files)
{
	char dir[1024];
	pc_sync_card_dir(dir, sizeof(dir));

	char names[PC_SYNC_MAX_FILES][256];
	int n = 0;

	DIR* d = opendir(dir);
	if (d) {
		struct dirent* de;
		while ((de = readdir(d)) != NULL && n < PC_SYNC_MAX_FILES) {
			if (!pc_sync_is_card_file(de->d_name)) {
				continue;
			}
			snprintf(names[n], sizeof(names[n]), "%s", de->d_name);
			n++;
		}
		closedir(d);
	}

	// Sort. readdir order is not defined, and an unstable order would change
	// the pack hash without the save changing — which reads downstream as
	// "the save was edited" and triggers a pointless upload or, worse, a
	// spurious conflict against another device.
	for (int i = 1; i < n; i++) {
		char key[256];
		snprintf(key, sizeof(key), "%s", names[i]);
		int j = i - 1;
		while (j >= 0 && strcmp(names[j], key) > 0) {
			snprintf(names[j + 1], sizeof(names[j + 1]), "%s", names[j]);
			j--;
		}
		snprintf(names[j + 1], sizeof(names[j + 1]), "%s", key);
	}

	size_t cap = 16;
	for (int i = 0; i < n; i++) {
		char path[1300];
		struct stat st;
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		if (stat(path, &st) != 0) {
			continue;
		}
		cap += 8 + strlen(names[i]) + (size_t)st.st_size;
	}

	unsigned char* buf = (unsigned char*)malloc(cap);
	if (!buf) {
		return NULL;
	}

	size_t off = 0;
	memcpy(buf, PC_SYNC_PACK_MAGIC, 8);
	off = 8;
	pc_put_u32(buf + off, PC_SYNC_PACK_VERSION);
	off += 4;
	unsigned char* count_at = buf + off; // patched once the files are in
	pc_put_u32(count_at, 0);
	off += 4;

	int written = 0;
	for (int i = 0; i < n; i++) {
		char path[1300];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		FILE* f = fopen(path, "rb");
		if (!f) {
			continue;
		}
		size_t nameLen = strlen(names[i]);
		if (off + 8 + nameLen > cap) {
			fclose(f);
			continue;
		}
		pc_put_u32(buf + off, (uint32_t)nameLen);
		off += 4;
		memcpy(buf + off, names[i], nameLen);
		off += nameLen;

		unsigned char* len_at = buf + off;
		off += 4;
		size_t got = fread(buf + off, 1, cap - off, f);
		fclose(f);
		pc_put_u32(len_at, (uint32_t)got);
		off += got;
		written++;
	}

	pc_put_u32(count_at, (uint32_t)written);
	*out_len   = off;
	*out_files = written;
	return buf;
}

static int pc_sync_pack_sha(const unsigned char* buf, size_t len, char out[65])
{
	uint8_t digest[32];
	pc_sha256(buf, len, digest);
	pc_hex(digest, 32, out);
	return 1;
}

// Unpacks over card A. Returns 1 on success. Validates the whole blob BEFORE
// touching a single file: a truncated download that half-replaced the card
// would be the one way this feature could eat a save.
static int pc_sync_unpack(const unsigned char* buf, size_t len)
{
	if (len < 16 || memcmp(buf, PC_SYNC_PACK_MAGIC, 8) != 0) {
		fprintf(stderr, "[SYNC] *** downloaded blob is not a card pack — ignoring it\n");
		return 0;
	}
	if (pc_get_u32(buf + 8) != PC_SYNC_PACK_VERSION) {
		fprintf(stderr, "[SYNC] *** card pack version %u, this build understands %u — ignoring it\n",
		        pc_get_u32(buf + 8), PC_SYNC_PACK_VERSION);
		return 0;
	}
	uint32_t count = pc_get_u32(buf + 12);
	if (count > PC_SYNC_MAX_FILES) {
		fprintf(stderr, "[SYNC] *** card pack claims %u files — ignoring it\n", count);
		return 0;
	}

	// Pass 1: walk the whole thing without writing anything.
	size_t off = 16;
	for (uint32_t i = 0; i < count; i++) {
		if (off + 4 > len) {
			return 0;
		}
		uint32_t nameLen = pc_get_u32(buf + off);
		off += 4;
		if (nameLen == 0 || nameLen > 200 || off + nameLen + 4 > len) {
			return 0;
		}
		char name[256];
		memcpy(name, buf + off, nameLen);
		name[nameLen] = '\0';
		off += nameLen;
		// A name from the network must not be able to escape card_a.
		if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..") || !pc_sync_is_card_file(name)) {
			fprintf(stderr, "[SYNC] *** card pack contains a suspect name '%s' — ignoring the pack\n", name);
			return 0;
		}
		uint32_t dataLen = pc_get_u32(buf + off);
		off += 4;
		if (off + dataLen > len) {
			return 0;
		}
		off += dataLen;
	}

	// Pass 2: write.
	char dir[1024];
	pc_sync_card_dir(dir, sizeof(dir));
	if (!pc_sync_is_dir(dir)) {
		mkdir(dir, 0755);
	}

	char present[PC_SYNC_MAX_FILES][256];
	uint32_t n_present = 0;

	off = 16;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t nameLen = pc_get_u32(buf + off);
		off += 4;
		char name[256];
		memcpy(name, buf + off, nameLen);
		name[nameLen] = '\0';
		off += nameLen;
		uint32_t dataLen = pc_get_u32(buf + off);
		off += 4;

		char path[1300];
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (!pc_sync_write_file(path, buf + off, dataLen)) {
			fprintf(stderr, "[SYNC] *** could not write %s\n", path);
			return 0;
		}
		off += dataLen;
		snprintf(present[n_present++], sizeof(present[0]), "%s", name);
	}

	// Card files the incoming pack does NOT contain have to go, or the card
	// still holds a save the server does not know about. That matters beyond
	// tidiness: the next pack would hash differently from what we just
	// checked out, which reads as "this device has local changes" and turns
	// every subsequent boot into a false conflict.
	//
	// Renamed, not deleted — ".replaced-<ts>" is matched by neither
	// pc_card.cpp's exact-suffix directory scan nor pc_sync_pack, so the file
	// leaves the card without leaving the disk. The caller has already kept a
	// full pack of the previous card as well; this is the belt to that's braces.
	// The scan is collected in full before anything is renamed: POSIX leaves
	// it unspecified whether an entry created during a directory walk is
	// returned by it, and a missed entry here is a stale save file left on
	// the card — exactly what this sweep exists to prevent.
	char stale[PC_SYNC_MAX_FILES][256];
	int n_stale = 0;

	DIR* d = opendir(dir);
	if (d) {
		struct dirent* de;
		while ((de = readdir(d)) != NULL && n_stale < PC_SYNC_MAX_FILES) {
			if (!pc_sync_is_card_file(de->d_name)) {
				continue;
			}
			int found = 0;
			for (uint32_t i = 0; i < n_present && !found; i++) {
				found = strcmp(present[i], de->d_name) == 0;
			}
			if (!found) {
				snprintf(stale[n_stale++], sizeof(stale[0]), "%s", de->d_name);
			}
		}
		closedir(d);
	}

	const long stamp = (long)time(NULL);
	for (int i = 0; i < n_stale; i++) {
		char from[1300], to[1400];
		snprintf(from, sizeof(from), "%s/%s", dir, stale[i]);
		snprintf(to, sizeof(to), "%s/%s.replaced-%ld", dir, stale[i], stamp);
		if (rename(from, to) == 0) {
			fprintf(stderr, "[SYNC] %s is not in the server's save — set aside as %s\n", stale[i], to);
		}
	}
	return 1;
}

// Keeps a copy of whatever is on the card right now, under save/sync/.
// `tag` is "presync" (routine, before a checkout) or "conflict" (this device
// had changes that were about to be passed over).
static void pc_sync_keep_copy(const char* tag, const unsigned char* buf, size_t len)
{
	if (!buf || len == 0) {
		return;
	}
	char path[1200];
	snprintf(path, sizeof(path), "%s/%s-%ld.pks", pc_sync_dir(), tag, (long)time(NULL));
	if (pc_sync_write_file(path, buf, len)) {
		fprintf(stderr, "[SYNC] kept the local card as %s\n", path);
	}
}

// ------------------------------------------------------------ status file

static void pc_sync_status_load(void)
{
	char path[1200];
	snprintf(path, sizeof(path), "%s/status.ini", pc_sync_dir());
	FILE* f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq       = '\0';
		char* val = eq + 1;
		while (*val == ' ') {
			val++;
		}
		size_t n = strlen(val);
		while (n > 0 && (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ')) {
			val[--n] = '\0';
		}
		char* key = line;
		size_t k  = strlen(key);
		while (k > 0 && (key[k - 1] == ' ' || key[k - 1] == '\t')) {
			key[--k] = '\0';
		}
		if (strcmp(key, "last_snapshot") == 0) {
			snprintf(s_last_snapshot, sizeof(s_last_snapshot), "%s", val);
		} else if (strcmp(key, "last_sha") == 0) {
			snprintf(s_last_sha, sizeof(s_last_sha), "%s", val);
		}
	}
	fclose(f);
}

static void pc_sync_status_save(void)
{
	char path[1200];
	snprintf(path, sizeof(path), "%s/status.ini", pc_sync_dir());
	FILE* f = fopen(path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "# Written by the save sync client; delete to force a full reconcile.\n");
	fprintf(f, "last_snapshot = %s\n", s_last_snapshot);
	fprintf(f, "last_sha = %s\n", s_last_sha);
	fclose(f);
}

// ------------------------------------------------------------- tiny JSON
// The server's replies are small, flat and generated by one known program.
// A real parser would be more code than the rest of this file.

static const char* pc_json_find(const char* from, const char* key)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char* p = from ? strstr(from, pat) : NULL;
	if (!p) {
		return NULL;
	}
	p += strlen(pat);
	while (*p == ' ' || *p == ':') {
		p++;
	}
	return p;
}

static int pc_json_str(const char* from, const char* key, char* out, size_t n)
{
	const char* p = pc_json_find(from, key);
	if (!p || *p != '"') {
		return 0;
	}
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < n - 1) {
		out[i++] = *p++;
	}
	out[i] = '\0';
	return 1;
}

static int pc_json_true(const char* from, const char* key)
{
	const char* p = pc_json_find(from, key);
	return p && strncmp(p, "true", 4) == 0;
}

// --------------------------------------------------------------- API calls

static void pc_sync_url(char* out, size_t n, const char* path) { snprintf(out, n, "%s%s", s_server, path); }

static int pc_sync_api(const char* method, const char* path, const char* json_body, const char* extra_hdr,
                       int timeout_ms, PcSyncHttpResp* resp)
{
	char url[512];
	const char* headers[3];
	int n = 0;

	pc_sync_url(url, sizeof(url), path);
	headers[n++] = s_auth_hdr;
	headers[n++] = "Content-Type: application/json";
	if (extra_hdr) {
		headers[n++] = extra_hdr;
	}
	return pc_sync_http(method, url, headers, n, json_body, json_body ? strlen(json_body) : 0, timeout_ms, resp);
}

static int pc_sync_api_enroll(int timeout_ms)
{
	char body[256];
	PcSyncHttpResp r;
	snprintf(body, sizeof(body), "{\"townId\":\"%s\",\"auth\":\"%s\",\"friendCode\":\"%s\",\"playerName\":\"%s\"}", s_id_hex,
	         s_auth_secret_hex, s_friend_code, s_device_name);
	if (pc_sync_api("POST", "/api/enroll", body, NULL, timeout_ms, &r) != 0) {
		return -1;
	}
	int status = (int)r.status;
	pc_sync_http_free(&r);
	return status;
}

// 200 -> lease held (s_lock_id set). 409 -> someone else has it, holder_out
// gets their device name. -1 -> could not reach the server.
static int pc_sync_api_lock(char* holder_out, size_t holder_n, int timeout_ms)
{
	char body[192];
	PcSyncHttpResp r;
	snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"deviceName\":\"%s\"}", s_device_id, s_device_name);
	if (pc_sync_api("POST", "/api/lock", body, NULL, timeout_ms, &r) != 0) {
		return -1;
	}
	int status = (int)r.status;
	if (status == 200) {
		pc_json_str((const char*)r.body, "lockId", s_lock_id, sizeof(s_lock_id));
	} else if (status == 409 && holder_out) {
		if (!pc_json_str((const char*)r.body, "deviceName", holder_out, holder_n)) {
			snprintf(holder_out, holder_n, "another device");
		}
	}
	pc_sync_http_free(&r);
	return status;
}

static int pc_sync_api_simple(const char* path, int timeout_ms)
{
	char body[96];
	PcSyncHttpResp r;
	snprintf(body, sizeof(body), "{\"lockId\":\"%s\"}", s_lock_id);
	if (pc_sync_api("POST", path, body, NULL, timeout_ms, &r) != 0) {
		return -1;
	}
	int status = (int)r.status;
	pc_sync_http_free(&r);
	return status;
}

static int pc_sync_api_town(char* latest_out, size_t latest_n, int* dirty_out, int timeout_ms)
{
	PcSyncHttpResp r;
	latest_out[0] = '\0';
	if (dirty_out) {
		*dirty_out = 0;
	}
	if (pc_sync_api("GET", "/api/town", NULL, NULL, timeout_ms, &r) != 0) {
		return -1;
	}
	int status = (int)r.status;
	if (status == 200) {
		const char* latest = pc_json_find((const char*)r.body, "latest");
		// "latest": null on a town that has never been uploaded to.
		if (latest && strncmp(latest, "null", 4) != 0) {
			pc_json_str(latest, "id", latest_out, latest_n);
		}
		if (dirty_out) {
			*dirty_out = pc_json_true((const char*)r.body, "dirty");
		}
	}
	pc_sync_http_free(&r);
	return status;
}

static int pc_sync_api_upload(const unsigned char* blob, size_t len, int timeout_ms)
{
	char url[512], lock_hdr[80], name_hdr[96];
	const char* headers[4];
	int n = 0;
	PcSyncHttpResp r;

	pc_sync_url(url, sizeof(url), "/api/save");
	snprintf(lock_hdr, sizeof(lock_hdr), "X-Lock-Id: %s", s_lock_id);
	snprintf(name_hdr, sizeof(name_hdr), "X-Player-Name: %s", s_device_name);
	headers[n++] = s_auth_hdr;
	headers[n++] = "Content-Type: application/octet-stream";
	headers[n++] = lock_hdr;
	headers[n++] = name_hdr;

	if (pc_sync_http("PUT", url, headers, n, blob, len, timeout_ms, &r) != 0) {
		return -1;
	}
	int status = (int)r.status;
	if (status == 200) {
		pc_json_str((const char*)r.body, "snapshot", s_last_snapshot, sizeof(s_last_snapshot));
	}
	pc_sync_http_free(&r);
	return status;
}

// ------------------------------------------------------------- operations

// Uploads the current card if it differs from what the server last got.
// Returns 1 if the server now holds this device's card (uploaded or already
// current), 0 on any failure.
static int pc_sync_push(int timeout_ms)
{
	size_t len   = 0;
	int files    = 0;
	unsigned char* blob = pc_sync_pack(&len, &files);
	if (!blob) {
		return 0;
	}
	if (files == 0) {
		free(blob); // nothing to publish; an empty card is not a save
		return 1;
	}

	char sha[65];
	pc_sync_pack_sha(blob, len, sha);
	if (strcmp(sha, s_last_sha) == 0) {
		free(blob);
		return 1; // already up there
	}

	int status = pc_sync_api_upload(blob, len, timeout_ms);
	free(blob);

	if (status == 200) {
		snprintf(s_last_sha, sizeof(s_last_sha), "%s", sha);
		pc_sync_status_save();
		fprintf(stderr, "[SYNC] uploaded (%zu bytes, snapshot %s)\n", len, s_last_snapshot);
		pc_sync_set_status("synced");
		return 1;
	}
	if (status == 409) {
		// Our lease expired and someone else took it. Do not fight over it —
		// the local card stays, and the next reconcile sorts it out.
		fprintf(stderr, "[SYNC] *** upload refused: the lease is no longer ours\n");
		pthread_mutex_lock(&s_mtx);
		s_state = PC_SYNC_LOCKED;
		pthread_mutex_unlock(&s_mtx);
		pc_sync_set_status("lease lost — playing offline");
	} else {
		fprintf(stderr, "[SYNC] upload failed (status %d)\n", status);
	}
	return 0;
}

// Downloads the server's latest into card A. `local`/`local_len` is the pack
// of what is there now, kept aside first under `tag`.
static int pc_sync_checkout(const char* tag, const unsigned char* local, size_t local_len, int timeout_ms)
{
	char url[512];
	const char* headers[1];
	PcSyncHttpResp r;

	pc_sync_url(url, sizeof(url), "/api/save");
	headers[0] = s_auth_hdr;
	if (pc_sync_http("GET", url, headers, 1, NULL, 0, timeout_ms, &r) != 0) {
		return 0;
	}
	if (r.status != 200) {
		fprintf(stderr, "[SYNC] download failed (status %ld)\n", r.status);
		pc_sync_http_free(&r);
		return 0;
	}

	pc_sync_keep_copy(tag, local, local_len);

	int ok = pc_sync_unpack(r.body, r.len);
	if (ok) {
		pc_sync_pack_sha(r.body, r.len, s_last_sha);
		fprintf(stderr, "[SYNC] checked out the server's save (%zu bytes)\n", r.len);
	}
	pc_sync_http_free(&r);
	return ok;
}

// The reconcile. Server is the source of truth once this device has enrolled,
// but nothing local is ever destroyed.
static void pc_sync_reconcile(int timeout_ms)
{
	char latest[48];
	int dirty = 0;
	if (pc_sync_api_town(latest, sizeof(latest), &dirty, timeout_ms) != 200) {
		return;
	}
	if (dirty) {
		// Someone's session ended without a clean release. Their last upload
		// is still the newest snapshot, so the normal rules below hold; this
		// is worth saying out loud because it usually means a crash.
		fprintf(stderr, "[SYNC] *** the server says the last session ended uncleanly\n");
	}

	size_t len          = 0;
	int files           = 0;
	unsigned char* blob = pc_sync_pack(&len, &files);
	if (!blob) {
		return;
	}
	char sha[65];
	pc_sync_pack_sha(blob, len, sha);

	if (!latest[0]) {
		// Server has nothing. Whatever is here becomes the starting point.
		if (files > 0) {
			fprintf(stderr, "[SYNC] server has no save yet — publishing this device's card\n");
			free(blob);
			pc_sync_push(timeout_ms);
			return;
		}
		fprintf(stderr, "[SYNC] no save here and none on the server — nothing to do yet\n");
	} else if (strcmp(latest, s_last_snapshot) == 0) {
		// The newest snapshot is the one we put there. Anything different on
		// disk is progress made while offline.
		if (files > 0 && strcmp(sha, s_last_sha) != 0) {
			fprintf(stderr, "[SYNC] local save changed while offline — uploading it\n");
			free(blob);
			pc_sync_push(timeout_ms);
			return;
		}
		pc_sync_set_status("synced");
	} else if (files == 0 || strcmp(sha, s_last_sha) == 0) {
		// Another device saved and this one has not changed since its last
		// sync: a plain checkout.
		fprintf(stderr, "[SYNC] another device has a newer save — checking it out\n");
		pc_sync_checkout("presync", blob, len, timeout_ms);
		snprintf(s_last_snapshot, sizeof(s_last_snapshot), "%s", latest);
		pc_sync_status_save();
		pc_sync_set_status("synced");
	} else {
		// Both sides moved. The server wins, but this device's card is kept
		// as a .pks pack — the one case where the user may actually want to
		// go back, so it is said loudly.
		fprintf(stderr, "[SYNC] *** BOTH this device and another have saved since the last sync\n");
		fprintf(stderr, "[SYNC] *** taking the server's copy; this device's is kept in %s\n", pc_sync_dir());
		pc_sync_checkout("conflict", blob, len, timeout_ms);
		snprintf(s_last_snapshot, sizeof(s_last_snapshot), "%s", latest);
		pc_sync_status_save();
		pc_sync_set_status("synced (a conflicting local save was set aside)");
	}
	free(blob);
}

// Enroll + lock + reconcile. Used at boot and again on iOS foreground.
static void pc_sync_connect(int timeout_ms)
{
	int st = pc_sync_api_enroll(timeout_ms);
	if (st < 0) {
		pthread_mutex_lock(&s_mtx);
		s_state    = PC_SYNC_OFFLINE;
		s_retry_at = time(NULL) + PC_SYNC_RETRY_SEC;
		pthread_mutex_unlock(&s_mtx);
		fprintf(stderr, "[SYNC] server unreachable at %s — playing offline\n", s_server);
		pc_sync_set_status("offline");
		return;
	}
	if (st == 403) {
		// Same id, different auth: two different keys hashing to the same id
		// is not a thing, so in practice this is the server's state file
		// having been restored from a backup that predates this device.
		fprintf(stderr, "[SYNC] *** the server rejected this key (403) — sync is off for this session\n");
		pthread_mutex_lock(&s_mtx);
		s_state = PC_SYNC_OFFLINE;
		pthread_mutex_unlock(&s_mtx);
		pc_sync_set_status("key rejected by the server");
		return;
	}
	if (st != 200) {
		fprintf(stderr, "[SYNC] *** enroll failed (status %d) — playing offline\n", st);
		pthread_mutex_lock(&s_mtx);
		s_state    = PC_SYNC_OFFLINE;
		s_retry_at = time(NULL) + PC_SYNC_RETRY_SEC;
		pthread_mutex_unlock(&s_mtx);
		pc_sync_set_status("offline");
		return;
	}

	char holder[64] = "";
	int lock        = pc_sync_api_lock(holder, sizeof(holder), timeout_ms);
	if (lock == 409) {
		fprintf(stderr, "[SYNC] *** the save is in use by '%s' — playing OFFLINE, this session will not upload\n",
		        holder);
		pthread_mutex_lock(&s_mtx);
		s_state    = PC_SYNC_LOCKED;
		s_retry_at = time(NULL) + PC_SYNC_RETRY_SEC;
		pthread_mutex_unlock(&s_mtx);
		pc_sync_set_status("in use by %s", holder);
		return;
	}
	if (lock != 200) {
		pthread_mutex_lock(&s_mtx);
		s_state    = PC_SYNC_OFFLINE;
		s_retry_at = time(NULL) + PC_SYNC_RETRY_SEC;
		pthread_mutex_unlock(&s_mtx);
		pc_sync_set_status("offline");
		return;
	}

	pthread_mutex_lock(&s_mtx);
	s_state   = PC_SYNC_SYNCED;
	s_last_hb = time(NULL);
	s_dirty   = 0;
	pthread_mutex_unlock(&s_mtx);
	pc_sync_set_status("synced");

	pc_sync_reconcile(timeout_ms);
}

// Release the lease so the other devices are not locked out.
static void pc_sync_release(int timeout_ms)
{
	if (!s_lock_id[0]) {
		return;
	}
	if (pc_sync_api_simple("/api/unlock", timeout_ms) == 200) {
		fprintf(stderr, "[SYNC] released the lease\n");
	}
	s_lock_id[0] = '\0';
}

// ----------------------------------------------------------------- worker

static void* pc_sync_worker(void*)
{
	for (;;) {
		sleep(1);

		pthread_mutex_lock(&s_mtx);
		int stop        = s_stop;
		int suspended   = s_suspended;
		PcSyncState st  = s_state;
		int dirty       = s_dirty;
		time_t changed  = s_last_change;
		time_t last_hb  = s_last_hb;
		time_t retry_at = s_retry_at;
		pthread_mutex_unlock(&s_mtx);

		if (stop) {
			break;
		}
		if (suspended) {
			continue; // the app is backgrounded; the lease is already released
		}

		const time_t now = time(NULL);

		if (st == PC_SYNC_SYNCED) {
			if (dirty && now - changed >= PC_SYNC_QUIET_SEC) {
				pthread_mutex_lock(&s_net_mtx);
				// Re-check under the net lock: a suspend may have run the
				// upload already and released the lease.
				pthread_mutex_lock(&s_mtx);
				int still = !s_suspended && s_state == PC_SYNC_SYNCED && s_dirty;
				if (still) {
					s_dirty = 0;
				}
				pthread_mutex_unlock(&s_mtx);
				if (still && !pc_sync_push(PC_SYNC_API_TIMEOUT_MS)) {
					// Put the flag back so a transient failure retries rather
					// than silently dropping the save.
					pthread_mutex_lock(&s_mtx);
					s_dirty = 1;
					pthread_mutex_unlock(&s_mtx);
				}
				pthread_mutex_unlock(&s_net_mtx);
			} else if (now - last_hb >= PC_SYNC_HEARTBEAT_SEC) {
				pthread_mutex_lock(&s_net_mtx);
				int hb = pc_sync_api_simple("/api/heartbeat", PC_SYNC_API_TIMEOUT_MS);
				pthread_mutex_unlock(&s_net_mtx);
				pthread_mutex_lock(&s_mtx);
				s_last_hb = time(NULL);
				if (hb == 404) {
					// The lease expired and was swept. Stop pretending we
					// hold it; the retry path below will try to get it back.
					s_state    = PC_SYNC_OFFLINE;
					s_retry_at = time(NULL);
					s_lock_id[0] = '\0';
				}
				pthread_mutex_unlock(&s_mtx);
				if (hb == 404) {
					fprintf(stderr, "[SYNC] *** our lease expired — trying to retake it\n");
				}
			}
		} else if ((st == PC_SYNC_OFFLINE || st == PC_SYNC_LOCKED) && now >= retry_at) {
			pthread_mutex_lock(&s_mtx);
			s_retry_at = now + PC_SYNC_RETRY_SEC;
			pthread_mutex_unlock(&s_mtx);
			pthread_mutex_lock(&s_net_mtx);
			pc_sync_connect(PC_SYNC_API_TIMEOUT_MS);
			pthread_mutex_unlock(&s_net_mtx);
		}
	}
	return NULL;
}

// ----------------------------------------------------------------- public

int pc_sync_enabled(void) { return s_configured; }

const char* pc_sync_status(void)
{
	static char out[128];
	pthread_mutex_lock(&s_mtx);
	snprintf(out, sizeof(out), "%s", s_status);
	pthread_mutex_unlock(&s_mtx);
	return out;
}

void pc_sync_notify_card_changed(void)
{
	if (!s_configured) {
		return;
	}
	pthread_mutex_lock(&s_mtx);
	s_dirty       = 1;
	s_last_change = time(NULL);
	pthread_mutex_unlock(&s_mtx);
}

void pc_sync_startup(void)
{
	if (s_started) {
		return;
	}
	s_started = 1;

	const char* server = pc_config_str("sync.server", "");
	const char* key    = pc_config_str("sync.key", "");
	if (!server[0] || !key[0]) {
		// Say nothing at all when neither is set — that is the default state
		// of a shipped settings.ini and not worth a line of log. Half-set is
		// almost always a typo, though, and silence there would be cruel.
		if (server[0] || key[0]) {
			fprintf(stderr, "[SYNC] *** settings.ini has [sync] %s but not %s — sync is OFF\n",
			        server[0] ? "server" : "key", server[0] ? "key" : "server");
		}
		return;
	}

	snprintf(s_server, sizeof(s_server), "%s", server);
	// Trailing slash would make every path a double slash. The server's route
	// table is exact-match regexes, so "//api/town" is a 404.
	size_t sl = strlen(s_server);
	while (sl > 0 && s_server[sl - 1] == '/') {
		s_server[--sl] = '\0';
	}

	// Device name: what the other devices are told is holding the save.
	const char* dev = pc_config_str("sync.device", "");
	if (dev[0]) {
		snprintf(s_device_name, sizeof(s_device_name), "%s", dev);
	} else {
		char host[64] = "";
		if (gethostname(host, sizeof(host) - 1) != 0 || !host[0]) {
			snprintf(host, sizeof(host), "a device");
		}
		// Trim the ".local" every Mac and iOS device carries; it is noise in
		// a "the save is in use by X" message.
		size_t hn = strlen(host);
		if (hn > 6 && strcmp(host + hn - 6, ".local") == 0) {
			host[hn - 6] = '\0';
		}
		snprintf(s_device_name, sizeof(s_device_name), "%s", host);
	}

	char dev_hex[65], code_hex[65];
	pc_sync_digest_hex("PIKMIN-SYNC-ID-V1", key, s_id_hex);
	pc_sync_digest_hex("PIKMIN-SYNC-AUTH-V1", key, s_auth_secret_hex);
	pc_sync_digest_hex("PIKMIN-SYNC-DEV-V1", s_device_name, dev_hex);
	pc_sync_digest_hex("PIKMIN-SYNC-CODE-V1", key, code_hex);

	snprintf(s_auth_hdr, sizeof(s_auth_hdr), "Authorization: Bearer %s.%s", s_id_hex, s_auth_secret_hex);

	// Per-device, and DERIVED rather than random: the server lets the same
	// device retake its own lease but nobody else's, so a stable id across
	// runs is what makes crash recovery work instead of locking this device
	// out of its own save for the lease TTL.
	snprintf(s_device_id, sizeof(s_device_id), "%.16s", dev_hex);

	// The server requires a friend code shaped 0000-0000-0000. It is a
	// user-facing handle in the prior port; here it is just a required field, so it
	// is derived from the key and never shown.
	unsigned g[3];
	for (int i = 0; i < 3; i++) {
		char part[5];
		snprintf(part, sizeof(part), "%.4s", code_hex + i * 4);
		g[i] = (unsigned)strtoul(part, NULL, 16) % 10000u;
	}
	snprintf(s_friend_code, sizeof(s_friend_code), "%04u-%04u-%04u", g[0], g[1], g[2]);

	s_configured = 1;
	fprintf(stderr, "[SYNC] %s as '%s'\n", s_server, s_device_name);

	pc_sync_status_load();

	pthread_mutex_lock(&s_net_mtx);
	pc_sync_connect(PC_SYNC_BOOT_TIMEOUT_MS);
	pthread_mutex_unlock(&s_net_mtx);

	if (pthread_create(&s_worker, NULL, pc_sync_worker, NULL) == 0) {
		s_worker_started = 1;
	} else {
		fprintf(stderr, "[SYNC] *** could not start the sync worker — no uploads this session\n");
	}

	// Desktop only in practice: on iOS a suspended app is killed, not exited,
	// so this never runs there and pc_sync_suspend carries the same duty.
	atexit(pc_sync_shutdown);
}

void pc_sync_shutdown(void)
{
	if (!s_configured || s_shutdown_done) {
		return;
	}
	s_shutdown_done = 1;

	pthread_mutex_lock(&s_mtx);
	s_stop         = 1;
	PcSyncState st = s_state;
	pthread_mutex_unlock(&s_mtx);

	pthread_mutex_lock(&s_net_mtx);
	if (st == PC_SYNC_SYNCED) {
		pc_sync_push(PC_SYNC_API_TIMEOUT_MS);
		pc_sync_release(PC_SYNC_API_TIMEOUT_MS);
	}
	pthread_mutex_unlock(&s_net_mtx);

	// Deliberately NOT joining the worker: it sleeps in one-second steps and
	// may be inside a blocking request, and this runs from atexit where a
	// pointless second of hang is worse than an abandoned thread the process
	// is about to drop anyway.
}

void pc_sync_suspend(void)
{
	if (!s_configured) {
		return;
	}
	pthread_mutex_lock(&s_mtx);
	if (s_suspended) {
		pthread_mutex_unlock(&s_mtx);
		return;
	}
	s_suspended         = 1;
	PcSyncState st      = s_state;
	pthread_mutex_unlock(&s_mtx);

	if (st != PC_SYNC_SYNCED) {
		return;
	}

	// The frame loop is already parked by the time this runs (pc_window.mm
	// handles SDL_APP_WILLENTERBACKGROUND synchronously), so the card is
	// quiescent and the usual quiet-period debounce is unnecessary here.
	pthread_mutex_lock(&s_net_mtx);
	pc_sync_push(PC_SYNC_SUSPEND_TIMEOUT_MS);
	pc_sync_release(PC_SYNC_SUSPEND_TIMEOUT_MS);
	pthread_mutex_unlock(&s_net_mtx);

	pthread_mutex_lock(&s_mtx);
	s_dirty = 0;
	pthread_mutex_unlock(&s_mtx);
	pc_sync_set_status("suspended");
}

void pc_sync_resume(void)
{
	if (!s_configured) {
		return;
	}
	pthread_mutex_lock(&s_mtx);
	if (!s_suspended) {
		pthread_mutex_unlock(&s_mtx);
		return;
	}
	s_suspended = 0;
	pthread_mutex_unlock(&s_mtx);

	// Anything could have happened while we were away — including another
	// device taking the lease and saving — so this is a full connect, not
	// just a re-lock.
	pthread_mutex_lock(&s_net_mtx);
	pc_sync_connect(PC_SYNC_API_TIMEOUT_MS);
	pthread_mutex_unlock(&s_net_mtx);
}
