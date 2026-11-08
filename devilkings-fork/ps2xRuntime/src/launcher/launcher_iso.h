// Reads a PS2 disc image (ISO 9660: .iso, or a .bin / .img with raw 2352-byte sectors) so the launcher can check that it
// is the game it needs and copy the game's files out of it. No game data is stored in this program: the user's own disc
// image is the only source.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace launcher::iso
{
    struct Check
    {
        bool ok = false;
        std::string message;  // what to show the user
        std::string bootFile; // e.g. SLUS_204.59
    };

    // Opens the image and reads SYSTEM.CNF. ok only when it boots the expected file (SLUS_204.59, Shinobi, USA).
    Check check(const std::filesystem::path &image, const std::string &expectedBootFile);

    // Copies every file of the image into destination (same folders and names; ";1" version suffixes dropped). Files already
    // there with the right size are skipped. progress(percent, current file) is called now and then; return false from it
    // to cancel. Returns an error text, empty on success.
    std::string extractAll(const std::filesystem::path &image, const std::filesystem::path &destination,
                           const std::function<bool(int, const std::string &)> &progress);
}
