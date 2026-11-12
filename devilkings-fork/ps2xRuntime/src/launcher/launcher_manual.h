#pragma once
// LOCAL PATCH (Shinobi launcher): the game manual from the player's own PDF (launcher_manual.cpp).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace launcher::manual
{
    // Pages already drawn in `folder` (page_001.png, page_002.png, ...).
    uint32_t cachedPages(const std::filesystem::path &folder);

    // Draws every page of `pdf` into `folder` as page_NNN.png, `height` pixels tall. Empty on success, else the reason.
    std::string render(const std::filesystem::path &pdf, const std::filesystem::path &folder, uint32_t height,
                       const std::function<void(uint32_t, uint32_t)> &progress);
}
