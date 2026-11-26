#pragma once
// LOCAL PATCH: the launcher updates itself from the project's GitHub releases (launcher_update.cpp).
#include <filesystem>
#include <functional>
#include <string>

namespace launcher::update
{
    // Deletes what a previous update left behind (the renamed old launcher).
    void cleanup(const std::filesystem::path &root);
    // In the background: when GitHub has a newer release than version.txt, post("update:<tag>|<zip url>").
    void startCheck(const std::filesystem::path &root, std::function<void(const std::wstring &)> post);
    // In the background: downloads and installs that zip over `root`, reporting update-progress:N, update-error:..., or
    // update-done (then calls restart).
    void install(const std::filesystem::path &root, const std::string &url, std::function<void(const std::wstring &)> post,
                 std::function<void()> restart);

    // LOCAL PATCH (smaller download): the game program for processors without AVX2 (bin/Shinobi-SSE4.exe) is no longer
    // in the release zip. On those PCs it comes from the same GitHub release (Shinobi-SSE4-v<version>.pack), once per
    // version; a Shinobi-SSE4.pack put in bin by hand (offline) is used instead of downloading.
    enum class OldCpuBuild
    {
        Ready,       // bin/Shinobi-SSE4.exe of this version is there
        Downloading, // being fetched now (`percent`)
        Failed,      // the download or unpacking failed (`error`); asking again tries again
    };
    // Starts the fetch when needed (in the background) and says where it is.
    // LOCAL PATCH (GitHub #28): which game program a PC without AVX2 gets: "AVX" (AVX but no AVX2: AMD FX, Intel Sandy /
    // Ivy Bridge) or "SSE4" (older). bin/Shinobi-<tier>.exe, release asset Shinobi-<tier>-v<version>.pack.
    std::string oldCpuTier();
    OldCpuBuild oldCpuBuild(const std::filesystem::path &root, int *percent = nullptr, std::string *error = nullptr);
    // LOCAL PATCH (v0.7): fetches game/intro/studio_intro.mp4 (not in the zip) when the studio intro is switched on.
    // Blocking, about 2 MB; false when it could not be fetched (the game then starts without the intro).
    bool studioIntro(const std::filesystem::path &root, const std::filesystem::path &video);
}
