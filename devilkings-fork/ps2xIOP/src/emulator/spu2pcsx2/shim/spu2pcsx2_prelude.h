// LOCAL: force-included into every PCSX2 SPU2 source (CMake: /FI), as PCSX2's precompiled header would be.
#pragma once

#include "common/Pcsx2Types.h"
#include "common/Console.h"
#include "IopMem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

static constexpr bool IsDevBuild = false;
static constexpr bool IsDebugBuild = false;
