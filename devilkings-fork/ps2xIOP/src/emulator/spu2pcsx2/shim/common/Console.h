// LOCAL: stand-in for PCSX2's common/Console.h. Messages go to stderr only with PS2X_SPU2_LOG=1.
#pragma once

#include "common/Pcsx2Types.h"

#include <cstdarg>

namespace spu2pcsx2_console
{
	bool enabled();

	struct Sink
	{
		void WriteLn(const char* fmt, ...) const;
		void Warning(const char* fmt, ...) const;
		void Error(const char* fmt, ...) const;
		void WriteLn() const {}
	};
} // namespace spu2pcsx2_console

inline constexpr spu2pcsx2_console::Sink Console{};
inline constexpr spu2pcsx2_console::Sink DevCon{};
