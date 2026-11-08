// LOCAL: stand-in for PCSX2's GS/GSVector.h: only the GSVector4i operations the SPU2 reverb resampler uses
// (ReverbResample.cpp, SSE path). _M_SSE below 0x501 keeps it off the AVX2 (GSVector8i) path.
#pragma once

#include "common/Pcsx2Types.h"
#include "GS/MultiISA.h"

#include <immintrin.h>

#ifndef _M_SSE
#define _M_SSE 0x401
#endif

class alignas(16) GSVector4i
{
public:
	union
	{
		__m128i m;
		s8 I8[16];
		s16 I16[8];
		s32 I32[4];
		u16 U16[8];
		u32 U32[4];
	};

	GSVector4i() = default;
	explicit GSVector4i(__m128i v)
		: m(v)
	{
	}

	template <bool aligned>
	static GSVector4i load(const void* p)
	{
		return GSVector4i(aligned ? _mm_load_si128(static_cast<const __m128i*>(p)) : _mm_loadu_si128(static_cast<const __m128i*>(p)));
	}

	GSVector4i mul16hrs(const GSVector4i& v) const { return GSVector4i(_mm_mulhrs_epi16(m, v.m)); }
	GSVector4i adds16(const GSVector4i& v) const { return GSVector4i(_mm_adds_epi16(m, v.m)); }
	GSVector4i hadds16(const GSVector4i& v) const { return GSVector4i(_mm_hadds_epi16(m, v.m)); }
};
