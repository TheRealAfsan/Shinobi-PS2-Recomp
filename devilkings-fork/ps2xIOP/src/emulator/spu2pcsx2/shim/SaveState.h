// LOCAL: stand-in for PCSX2's SaveState.h (only the types named by spu2.h; save states are not used).
#pragma once

#include "common/Pcsx2Types.h"

enum class FreezeAction
{
	Load,
	Save,
	Size,
};

struct freezeData
{
	int size;
	u8* data;
};
