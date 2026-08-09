#ifndef _JAUDIO_BX_PC_H
#define _JAUDIO_BX_PC_H

// PC: host-side representation of the pikibank.bx file format.
//
// The .bx is a GameCube disc file that jaudio parses IN PLACE: the structs in
// bx.h are overlaid straight onto the loaded bytes, and Wave_Test/Bank_Test
// walk them rebasing the embedded offsets (PTconvert). Two independent things
// break that on an LP64 little-endian host:
//
//   1. ENDIANNESS. Every scalar in the file is big-endian. Read natively,
//      seqCount 22 becomes 0x16000000 and offsets become nonsense.
//   2. POINTER WIDTH. The file stores 4-byte file-relative offsets in the very
//      slots bx.h declares as pointers. On LP64 a pointer is 8 bytes, so the
//      structs stop lining up with the file entirely — the first symptom was a
//      fault on 0xc08e0000e03f0000, which is two adjacent big-endian file
//      words read as one 64-bit pointer.
//
// The fix keeps the file layout byte-exact and leaves the parsers' in-place
// semantics intact, by making the FIELD TYPES do the work:
//
//   - JauBE32/JauBE16 hold the raw big-endian word and swap on access, so
//      file scalars stay 4/2 bytes and read correctly. No swap pass, so no
//      ordering or double-swap hazards; a field that is both file-initialised
//      and runtime-written stays self-consistent.
//   - JauFilePtr<T> is a 4-byte pointer slot. PTconvert turns the raw
//      big-endian blob-relative offset into an offset from the .bx image base
//      (recorded once by Bank_Setup); dereferencing adds the base back.
//
// Fields that are pure runtime scratch — the DVD status words, and the heap
// structs the loader initialises inside the image — stay host-native. jaheap_
// is the one field that cannot stay inline: it holds six pointers, so it grows
// 0x2C -> 0x48 on LP64 and would shift every field after it. The file record
// therefore carries a 0x2C placeholder and the real jaheap_ lives in a side
// map keyed by record address (jau_file_heap).

#if defined(TARGET_PC)

#include "types.h"

#ifdef __cplusplus

// ---------------------------------------------------------------- image base
// All .bx blobs live inside one allocation made by Bank_Setup, so a single
// base plus a 32-bit offset addresses any record in the image.
extern uintptr_t jau_bx_base;
extern u32 jau_bx_size;

// Called by Bank_Setup before parsing. Resets the relocation set and the file
// heap map, so a second Bank_Setup on freshly loaded bytes starts clean.
void jau_bx_begin(void* base, u32 size);

// True when p points inside the loaded image.
inline bool jau_bx_contains(const void* p)
{
	uintptr_t a = (uintptr_t)p;
	return jau_bx_base != 0 && a >= jau_bx_base && a < jau_bx_base + jau_bx_size;
}

// Loud on violation rather than silently producing a wild pointer — the
// renderer's "failed pointer resolution must be loud" rule applies here too.
u32 jau_bx_offset_of(const void* p);

// ------------------------------------------------------------- big-endian
// Scalars keep their file width and byte order; the swap happens on access.
template <typename T> struct JauBE32 {
	u32 mRaw;

	// Defaulted so file overlays stay trivial; the converting constructor lets
	// the host-authored statics in jamosc.c / oneshot.c be written naturally
	// and still encode to the file's byte order.
	JauBE32() = default;
	JauBE32(T v) { set(v); }

	inline T get() const
	{
		u32 v = __builtin_bswap32(mRaw);
		T out;
		__builtin_memcpy(&out, &v, sizeof(T));
		return out;
	}
	inline void set(T v)
	{
		u32 w;
		__builtin_memcpy(&w, &v, sizeof(T));
		mRaw = __builtin_bswap32(w);
	}
	inline operator T() const { return get(); }
	inline JauBE32& operator=(T v)
	{
		set(v);
		return *this;
	}
};

template <typename T> struct JauBE16 {
	u16 mRaw;

	JauBE16() = default;
	JauBE16(T v) { set(v); }

	inline T get() const
	{
		u16 v = __builtin_bswap16(mRaw);
		T out;
		__builtin_memcpy(&out, &v, sizeof(T));
		return out;
	}
	inline void set(T v)
	{
		u16 w;
		__builtin_memcpy(&w, &v, sizeof(T));
		mRaw = __builtin_bswap16(w);
	}
	inline operator T() const { return get(); }
	inline JauBE16& operator=(T v)
	{
		set(v);
		return *this;
	}
};

// --------------------------------------------------------------- file ptr
// 4-byte pointer slot. Before relocation mSlot holds the raw big-endian,
// blob-relative file word; after relocation it holds an offset from
// jau_bx_base (0 stays null). PTconvert performs the conversion.
template <typename T> struct JauFilePtr {
	u32 mSlot;

	inline T* get() const { return mSlot ? (T*)(jau_bx_base + mSlot) : (T*)0; }
	inline operator T*() const { return get(); }
	inline T* operator->() const { return get(); }
	inline T& operator*() const { return *get(); }
	inline T& operator[](int i) const { return get()[i]; }

	// Runtime assignment of a pointer that must already live inside the image
	// (connect.c hands these around: wave->data, wave->data->fileLoadStatus).
	inline JauFilePtr& operator=(T* p)
	{
		mSlot = p ? jau_bx_offset_of(p) : 0;
		return *this;
	}
};

// The blob-relative offsets are relocated exactly once. A record can be shared
// by two parents (which is why the original PTconvert guarded on "already
// looks absolute"), and here the pre- and post-relocation values are not
// distinguishable by value, so visited slots are tracked explicitly.
bool jau_reloc_mark(const void* slot); // false if already relocated

// One-line summary of what relocation did, for the boot log.
void jau_bx_report();

template <typename T> inline void PTconvertT(JauFilePtr<T>* slot, uintptr_t blobBase)
{
	if (!jau_reloc_mark(slot)) {
		return;
	}
	u32 fileOff = __builtin_bswap32(slot->mSlot);
	if (fileOff == 0) {
		slot->mSlot = 0;
		return;
	}
	slot->mSlot = (u32)(blobBase - jau_bx_base) + fileOff;
}

// ---------------------------------------------------------- envelope vectors
// Osc_ is the one struct that is BOTH a .bx record (Inst_::mOscillators) and a
// host-authored static (jamosc.c's VIBRATO_DEF, oneshot.c's OSC_ENV), so its
// two envelope-vector slots must address either the loaded image or a DOL
// array — while still occupying the file's 4 bytes. Image offsets keep bit 31
// clear; host tables are registered on first use and referenced by index with
// bit 31 set.
#define JAU_VEC_HOST 0x80000000u

s16* jau_vec_host(u32 index);
u32 jau_vec_register(s16* table);

struct JauVecPtr {
	u32 mSlot;

	JauVecPtr() = default;
	JauVecPtr(s16* p) { *this = p; }

	inline s16* get() const
	{
		if (mSlot == 0) {
			return (s16*)0;
		}
		if (mSlot & JAU_VEC_HOST) {
			return jau_vec_host(mSlot & ~JAU_VEC_HOST);
		}
		return (s16*)(jau_bx_base + mSlot);
	}
	inline operator s16*() const { return get(); }
	inline s16& operator[](int i) const { return get()[i]; }

	inline JauVecPtr& operator=(s16* p)
	{
		if (!p) {
			mSlot = 0;
		} else if (jau_bx_contains(p)) {
			mSlot = (u32)((uintptr_t)p - jau_bx_base);
		} else {
			mSlot = JAU_VEC_HOST | jau_vec_register(p);
		}
		return *this;
	}
};

// Envelope-vector slots inside a .bx record relocate exactly like a file
// pointer; the host-table encoding only ever arises from runtime assignment.
inline void PTconvertT(JauVecPtr* slot, uintptr_t blobBase)
{
	if (!jau_reloc_mark(slot)) {
		return;
	}
	u32 fileOff = __builtin_bswap32(slot->mSlot);
	slot->mSlot = fileOff ? (u32)(blobBase - jau_bx_base) + fileOff : 0;
}

// ------------------------------------------------------------- file heaps
// jaheap_ cannot live inline in a file record on LP64 (see header comment).
// The record carries JauFileHeap as a same-size placeholder; the real heap is
// looked up by record address, created on first use.
struct jaheap_;
struct JauFileHeap {
	u8 mFilePad[0x2C];
};
jaheap_& jau_file_heap(const JauFileHeap* slot);

// `arc->heap` reads as a jaheap_& at every existing use site.
#define JAU_HEAP(x) jau_file_heap(&(x))

// The raw contents of a pointer slot, for the places that compare it against a
// file sentinel (connect.c tests WaveID_::data against 0xffffffff, which is the
// file's "no wave here" marker and never a real relocated offset).
#define JAU_PTR_RAW(x) ((x).mSlot)

#endif // __cplusplus

#endif // TARGET_PC

#endif
