// Shinobi launcher: the Windows-only parts (kept out of the raylib file, whose names clash with windows.h).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace launcher
{
    // Folder of the launcher executable.
    std::string exeDirectory();

    // Starts the game: runner with the ELF as its argument, working folder gameDir, stderr into stderrLog, extra
    // environment variables added to the launcher's own. Returns false and sets error on failure.
    bool launchGame(const std::string &runner, const std::string &elf, const std::string &gameDir, const std::string &stderrLog,
                    const std::vector<std::pair<std::string, std::string>> &environment, std::string &error);

    // Opens a file or folder with its Windows default program.
    void openWithShell(const std::string &path);
}
