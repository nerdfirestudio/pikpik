// PC: support for parsing pikibank.bx on a 64-bit little-endian host.
// See include/jaudio/bx_pc.h for why this exists.

#if defined(TARGET_PC)

#include "Dolphin/os.h"
#include "jaudio/bx.h"
#include "jaudio/bx_pc.h"
#include "jaudio/heapctrl.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// The whole point of the typed-field scheme is that the host layout still
// matches the GameCube file layout byte for byte. Prove it at compile time —
// a silent mismatch here is the bug class that produced 0xc08e0000e03f0000.
#define JAU_CHECK_SIZE(T, n)   _Static_assert(sizeof(T) == (n), #T " changed size — .bx layout broken")
#define JAU_CHECK_OFF(T, f, n) _Static_assert(offsetof(T, f) == (n), #T "::" #f " moved — .bx layout broken")

JAU_CHECK_SIZE(Vmap_, 0x10);
JAU_CHECK_OFF(Vmap_, mWsysID, 0x04);
JAU_CHECK_OFF(Vmap_, mPitch, 0x0C);
JAU_CHECK_SIZE(InstKeymap_, 0x10);
JAU_CHECK_OFF(InstKeymap_, mVelocityCount, 0x04);
// No size check: the documented 0x18 covers a single trailing velocity slot,
// but mVelocities is really variable-length (mVelocityCount decides), so the
// declared [2] deliberately overshoots. The offset is the load-bearing part.
JAU_CHECK_OFF(PercKeymap_, mVelocities, 0x14);
JAU_CHECK_SIZE(Sense_, 0x0C);
JAU_CHECK_SIZE(Osc_, 0x18);
JAU_CHECK_OFF(Osc_, attackVecOffset, 0x08);
JAU_CHECK_SIZE(Rand_, 0x10);
JAU_CHECK_SIZE(Inst_, 0x40);
JAU_CHECK_OFF(Inst_, mKeyRegionCount, 0x28);
JAU_CHECK_OFF(Inst_, mKeyRegions, 0x2C);
JAU_CHECK_SIZE(Voice_, 0x10);
JAU_CHECK_SIZE(Perc_, 0x408);
JAU_CHECK_OFF(Perc_, mKeyRegions, 0x88);
JAU_CHECK_OFF(Perc_, panTable, 0x288);
JAU_CHECK_OFF(Perc_, releaseTable, 0x308);
JAU_CHECK_SIZE(Bank_, 0x3C4);
JAU_CHECK_OFF(Ibnk_, bank, 0x20);

JAU_CHECK_SIZE(Wsys_, 0x18);
JAU_CHECK_OFF(Wsys_, waveArcBank, 0x10);
JAU_CHECK_SIZE(WaveArchiveBank_, 0x0C);
JAU_CHECK_SIZE(CtrlGroup_, 0x10);
JAU_CHECK_SIZE(SCNE_, 0x1C);
JAU_CHECK_OFF(SCNE_, cdf, 0x0C);
JAU_CHECK_OFF(SCNE_, dependencyIds, 0x18);
JAU_CHECK_SIZE(Ctrl_, 0x0C);
JAU_CHECK_SIZE(WaveID_, 0x38);
JAU_CHECK_OFF(WaveID_, loadStatus, 0x30);
JAU_CHECK_OFF(WaveID_, data, 0x34);
JAU_CHECK_SIZE(WaveArchive_, 0x78);
JAU_CHECK_OFF(WaveArchive_, fileLoadStatus, 0x6C);
JAU_CHECK_OFF(WaveArchive_, waveCount, 0x70);
JAU_CHECK_OFF(WaveArchive_, waves, 0x74);
JAU_CHECK_SIZE(Wave_, 0x28);
JAU_CHECK_OFF(Wave_, loopYN1, 0x20);
JAU_CHECK_OFF(Wave_, fileLoadStatus, 0x24);

uintptr_t jau_bx_base = 0;
u32 jau_bx_size       = 0;

// ------------------------------------------------------------------ offsets

u32 jau_bx_offset_of(const void* p)
{
	if (!jau_bx_contains(p)) {
		OSReport("[JAUDIO] bx: pointer %p is outside the loaded image [%p, +0x%x) — cannot store in a 4-byte file slot\n", p,
		         (void*)jau_bx_base, jau_bx_size);
		OSPanic(__FILE__, __LINE__, "bx pointer slot out of range");
		return 0;
	}
	return (u32)((uintptr_t)p - jau_bx_base);
}

// -------------------------------------------------------------- reloc marks
// Every pointer slot is a 4-byte-aligned word inside the image, so "already
// relocated" is one bit per word — exact, and 6 KB for the real 187 KB .bx.

static u8* s_reloc_bits    = NULL;
static u32 s_reloc_bytes   = 0;
static u32 s_reloc_marked  = 0;
static u32 s_reloc_repeats = 0;

bool jau_reloc_mark(const void* slot)
{
	if (!jau_bx_contains(slot)) {
		OSReport("[JAUDIO] bx: relocation slot %p outside the image — refusing\n", slot);
		return false;
	}
	u32 word = (u32)(((uintptr_t)slot - jau_bx_base) >> 2);
	u32 byte = word >> 3;
	u8 bit   = (u8)(1 << (word & 7));
	if (byte >= s_reloc_bytes) {
		return false;
	}
	if (s_reloc_bits[byte] & bit) {
		s_reloc_repeats++;
		return false; // shared record reached from a second parent
	}
	s_reloc_bits[byte] |= bit;
	s_reloc_marked++;
	return true;
}

// -------------------------------------------------------------- file heaps
// The real jaheap_ for a file record. The record's 0x2C placeholder is runtime
// scratch (its file bytes are meaningless), so the binding is stashed there:
// a magic plus an index, cross-checked against the owner recorded on the heap
// entry, which makes a stale/garbage tag effectively impossible to mistake for
// a live one.

#define JAU_HEAP_TAG 'JHEP'

typedef struct JauHeapTag {
	u32 magic;
	u32 index;
} JauHeapTag;

typedef struct JauHeapEntry {
	const void* owner;
	jaheap_ heap;
} JauHeapEntry;

static JauHeapEntry* s_heaps = NULL;
static u32 s_heap_count      = 0;
static u32 s_heap_capacity   = 0;

jaheap_& jau_file_heap(const JauFileHeap* slot)
{
	JauHeapTag* tag = (JauHeapTag*)slot;

	if (tag->magic == JAU_HEAP_TAG && tag->index < s_heap_count && s_heaps[tag->index].owner == slot) {
		return s_heaps[tag->index].heap;
	}

	if (s_heap_count == s_heap_capacity) {
		u32 cap            = s_heap_capacity ? s_heap_capacity * 2 : 256;
		JauHeapEntry* grow = (JauHeapEntry*)realloc(s_heaps, cap * sizeof(JauHeapEntry));
		if (!grow) {
			OSPanic(__FILE__, __LINE__, "bx: out of memory growing the file-heap table");
		}
		s_heaps        = grow;
		s_heap_capacity = cap;
	}

	u32 idx = s_heap_count++;
	memset(&s_heaps[idx], 0, sizeof(JauHeapEntry));
	s_heaps[idx].owner = slot;
	tag->magic         = JAU_HEAP_TAG;
	tag->index         = idx;
	return s_heaps[idx].heap;
}

// ---------------------------------------------------------- envelope vectors
// Small and write-once: the DOL defines a handful of envelope tables
// (VIB_TABLE, TRE_TABLE, REL_TABLE, OSC_REL, ADS_TABLE) and every Osc_ that
// isn't a .bx record points at one of them.

// Grows on demand. A fixed cap was wrong: Osc_Setup_ADSR points a voice's
// envelope at a PER-TRACK ADSR buffer, so the number of distinct host tables
// scales with active tracks, not with the handful of DOL-resident tables. A
// 32-entry cap panicked ~18 s into gameplay once several tracks were live.
static s16** s_vec_tables = NULL;
static u32 s_vec_count    = 0;
static u32 s_vec_capacity = 0;

u32 jau_vec_register(s16* table)
{
	for (u32 i = 0; i < s_vec_count; i++) {
		if (s_vec_tables[i] == table) {
			return i;
		}
	}
	if (s_vec_count == s_vec_capacity) {
		u32 cap    = s_vec_capacity ? s_vec_capacity * 2 : 64;
		s16** grow = (s16**)realloc(s_vec_tables, cap * sizeof(s16*));
		if (!grow) {
			OSPanic(__FILE__, __LINE__, "bx: out of memory growing the envelope-table registry");
		}
		s_vec_tables   = grow;
		s_vec_capacity = cap;
	}
	// The slot index is stored in a 4-byte file pointer with bit 31 as the
	// host flag, so the usable index space is 31 bits — far beyond any real
	// track count, but assert the boundary rather than silently aliasing.
	if (s_vec_count >= JAU_VEC_HOST) {
		OSPanic(__FILE__, __LINE__, "bx: envelope-table registry index overflow");
	}
	s_vec_tables[s_vec_count] = table;
	return s_vec_count++;
}

s16* jau_vec_host(u32 index)
{
	if (index >= s_vec_count) {
		OSReport("[JAUDIO] bx: host envelope table %u out of range (%u registered)\n", index, s_vec_count);
		return NULL;
	}
	return s_vec_tables[index];
}

// ------------------------------------------------------------------- lifetime

void jau_bx_begin(void* base, u32 size)
{
	jau_bx_base = (uintptr_t)base;
	jau_bx_size = size;

	// Bank_Setup reloads the file, so every previous binding is stale.
	s_heap_count    = 0;
	s_reloc_marked  = 0;
	s_reloc_repeats = 0;

	u32 words  = (size + 3) >> 2;
	u32 needed = (words + 7) >> 3;
	if (needed > s_reloc_bytes) {
		u8* grow = (u8*)realloc(s_reloc_bits, needed);
		if (!grow) {
			OSPanic(__FILE__, __LINE__, "bx: out of memory for the relocation bitmap");
		}
		s_reloc_bits  = grow;
		s_reloc_bytes = needed;
	}
	memset(s_reloc_bits, 0, s_reloc_bytes);

	OSReport("[JAUDIO] bx image at %p, 0x%x bytes\n", base, size);
}

void jau_bx_report()
{
	OSReport("[JAUDIO] bx relocated %u pointer slots (%u shared records revisited), %u file heaps bound\n", s_reloc_marked,
	         s_reloc_repeats, s_heap_count);
}

#endif // TARGET_PC
