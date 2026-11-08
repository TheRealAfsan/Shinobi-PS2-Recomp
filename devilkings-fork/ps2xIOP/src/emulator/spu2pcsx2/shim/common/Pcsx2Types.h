// LOCAL: stand-in for PCSX2's common/Pcsx2Types.h, just what PCSX2's SPU2 core (third_party/pcsx2/pcsx2/SPU2) uses.
// The SPU2 sources themselves are compiled unmodified; see ps2xIOP/src/emulator/spu2pcsx2/README.md.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using u8 = std::uint8_t;
using s8 = std::int8_t;
using u16 = std::uint16_t;
using s16 = std::int16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using uptr = std::uintptr_t;
using sptr = std::intptr_t;
using uint = unsigned int;

#if defined(_MSC_VER)
#ifndef __forceinline
// MSVC keyword already
#endif
#define __fi __forceinline
#define __ri
#define __noinline __declspec(noinline)
#else
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#define __fi __forceinline
#define __ri
#define __noinline __attribute__((noinline))
#endif

#define pxAssume(cond) ((void)0)
#define pxAssert(cond) ((void)0)
#define pxAssertMsg(cond, msg) ((void)0)
#define pxAssertRel(cond, msg) ((void)0)
#define pxFail(msg) ((void)0)
#define pxFailRel(msg) ((void)0)
#define jNO_DEFAULT \
	default:        \
		break;
