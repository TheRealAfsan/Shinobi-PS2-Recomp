// LOCAL: stand-in for PCSX2's GS/MultiISA.h. PCSX2 compiles some files once per instruction set and picks one at run
// time; here there is a single native build, so every "ISA" namespace is isa_native and the selection is fixed.
#pragma once

#define MULTI_ISA_COMPILE_ONCE 1
#define MULTI_ISA_UNSHARED_START \
	namespace isa_native         \
	{
#define MULTI_ISA_UNSHARED_END }
#define MULTI_ISA_DEF(...) \
	namespace isa_native   \
	{                      \
		__VA_ARGS__        \
	}
#define MULTI_ISA_SELECT(fn) (&isa_native::fn)
