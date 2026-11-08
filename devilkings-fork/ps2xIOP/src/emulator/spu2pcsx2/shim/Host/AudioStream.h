// LOCAL: stand-in for PCSX2's Host/AudioStream.h. Output goes through spu2_port.cpp (spu2Output) into the runtime's own
// audio ring instead of a PCSX2 audio stream.
#pragma once

class AudioStream;
