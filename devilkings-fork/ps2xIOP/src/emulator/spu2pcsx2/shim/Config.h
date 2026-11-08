// LOCAL: stand-in for PCSX2's Config.h. The SPU2 core only reads configuration in debug builds (PCSX2_DEVBUILD is not
// defined here); EmuFolders::OpenLogFile is likewise only reached from debug code.
#pragma once

#include "common/Pcsx2Types.h"

#include <cstdio>

struct Pcsx2Config;

namespace EmuFolders
{
	inline std::FILE* OpenLogFile(const char*, const char*)
	{
		return nullptr;
	}
} // namespace EmuFolders
