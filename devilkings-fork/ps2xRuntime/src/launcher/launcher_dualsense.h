#pragma once
// LOCAL PATCH: the launcher's own DualSense reader (launcher_dualsense.cpp): menu buttons as "pad:..." page messages.
#include <functional>
#include <string>

namespace launcher::dualsense
{
    // Starts the reader thread; `send` gets "pad:up" etc. (from that thread), "pad:host" when a DualSense is being read
    // and "pad:lost" when it goes away.
    void start(std::function<void(const std::wstring &)> send);
}
