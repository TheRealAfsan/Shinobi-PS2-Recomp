// Shinobi (PS2 Recom) Launcher - by TheRealAfsan.
// A borderless window that shows the launcher page (launcher_assets/launcher.html, built from the Claude Design file by
// scripts/build_launcher_page.py) in Microsoft Edge WebView2, and does what its buttons ask: settings, key bindings
// and mods are read from / written to game/settings.ini (the game's own settings file) and launcher.ini; Play starts
// the game, Stop ends it, the page hears when its window is up and when it closes.
//
// Page -> host messages: one string, first line the command, then key=value lines (see the page's send()):
//   save | play | stop | drag | minimize | close | openfolder | opensaves | browseiso | installiso
// Host -> page: saved | starting | running | exited | error:<text> | iso:ok|<path>|<text> | iso:bad|<path>|<text> |
//   install:<percent>|<file> | installed | install-error:<text>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl.h>
#include <dxgi.h>
#include <regex>
#pragma comment(lib, "dxgi.lib")

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#include "launcher_dualsense.h"
#include "launcher_pads.h" // LOCAL PATCH: the kind of controller connected (button names on the page)
#include "launcher_update.h"
#include "launcher_pack.h"
#include "launcher_ini.h"
#include "launcher_iso.h"
#include "launcher_manual.h"
#include "launcher_settings_map.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
    // ------------------------------------------------------------------ paths and files

    fs::path g_root;          // folder of the launcher
    fs::path g_gameDir;       // game/ (SLUS_204.59, settings.ini, captures/)
    fs::path g_runner;        // ps2EntryRunner.exe
    fs::path g_page;          // launcher_assets/launcher.html
    launcher::Ini g_game;     // game/settings.ini
    launcher::Ini g_mine;     // launcher.ini

    HWND g_hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> g_controller;
    ComPtr<ICoreWebView2> g_view;
    HANDLE g_gameProcess = nullptr;
    bool g_webviewMissing = false; // WebView2 could not start: the window closes and runSimple() takes over
    DWORD g_gamePid = 0;
    bool g_reportedRunning = false;
    std::wstring g_shotPath;  // SHINOBI_LAUNCHER_SHOT test hook

    constexpr UINT kPageMessage = WM_APP + 1; // a worker thread hands the page a message (lParam: new std::wstring)
    constexpr const char *kBootFile = "SLUS_204.59"; // Shinobi (USA)
    constexpr UINT_PTR kWatchTimer = 1;
    constexpr UINT_PTR kShotTimer = 2;
    constexpr int kPageWidth = 1000;
    constexpr int kPageHeight = 640;

    std::wstring widen(const std::string &text)
    {
        if (text.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), n);
        out.resize(static_cast<size_t>(n > 0 ? n - 1 : 0));
        return out;
    }

    std::string narrow(const std::wstring &text)
    {
        if (text.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), n, nullptr, nullptr);
        out.resize(static_cast<size_t>(n > 0 ? n - 1 : 0));
        return out;
    }

    void post(const std::wstring &message)
    {
        if (g_view)
            g_view->PostWebMessageAsString(message.c_str());
    }

    // From any thread: the page gets the message on the window's own thread.
    void postFromThread(const std::wstring &message)
    {
        if (g_hwnd)
            PostMessageW(g_hwnd, kPageMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
    }

    // ------------------------------------------------------------------ the user's own disc image

    std::atomic<bool> g_installing{false};
    std::thread g_installThread;

    // The repository keeps the game program zipped (GitHub refuses files over 100 MB). It is unpacked by itself, in the
    // background when the launcher starts and again when the game file is set up, so the player never sees or does it.
    std::mutex g_unpackMutex;
    std::thread g_unpackThread;

    // Running on Linux or a Steam Deck through Wine/Proton.
    bool underWine()
    {
        return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    }

    // Unzips `zip` into `folder` with the zlib1.dll shipped in bin, for when Windows' tar.exe is not there (Wine/Proton
    // has none). Stored and deflated entries; every file's CRC is checked and it is written under a temporary name first.
    bool unzipWithZlib(const fs::path &zip, const fs::path &folder, const fs::path &zlibDll = {})
    {
        struct ZStream
        {
            const unsigned char *next_in; unsigned avail_in; unsigned long total_in;
            unsigned char *next_out; unsigned avail_out; unsigned long total_out;
            const char *msg; void *state; void *zalloc; void *zfree; void *opaque;
            int data_type; unsigned long adler; unsigned long reserved;
        };
        using InflateInit2 = int (*)(ZStream *, int, const char *, int);
        using Inflate = int (*)(ZStream *, int);
        using InflateEnd = int (*)(ZStream *);
        using Version = const char *(*)();
        using Crc32 = unsigned long (*)(unsigned long, const unsigned char *, unsigned);
        HMODULE z = LoadLibraryW((zlibDll.empty() ? folder / "zlib1.dll" : zlibDll).wstring().c_str());
        if (!z)
            return false;
        const auto init = reinterpret_cast<InflateInit2>(GetProcAddress(z, "inflateInit2_"));
        const auto inflate = reinterpret_cast<Inflate>(GetProcAddress(z, "inflate"));
        const auto end = reinterpret_cast<InflateEnd>(GetProcAddress(z, "inflateEnd"));
        const auto version = reinterpret_cast<Version>(GetProcAddress(z, "zlibVersion"));
        const auto crc32 = reinterpret_cast<Crc32>(GetProcAddress(z, "crc32"));
        bool ok = false;
        std::ifstream in(zip, std::ios::binary);
        if (init && inflate && end && version && crc32 && in)
        {
            auto u16 = [](const unsigned char *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
            auto u32 = [](const unsigned char *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<uint32_t>(p[3]) << 24); };
            in.seekg(0, std::ios::end);
            const uint64_t length = static_cast<uint64_t>(in.tellg());
            const uint64_t tailSize = std::min<uint64_t>(length, 65557);
            std::vector<unsigned char> tail(static_cast<size_t>(tailSize));
            in.seekg(static_cast<std::streamoff>(length - tailSize));
            in.read(reinterpret_cast<char *>(tail.data()), static_cast<std::streamsize>(tail.size()));
            size_t eocd = std::string::npos;
            for (size_t i = tail.size() >= 22 ? tail.size() - 22 : 0; i != static_cast<size_t>(-1); --i)
                if (u32(&tail[i]) == 0x06054b50u)
                {
                    eocd = i;
                    break;
                }
            if (eocd != std::string::npos)
            {
                const uint32_t count = u16(&tail[eocd + 10]);
                const uint32_t dirSize = u32(&tail[eocd + 12]);
                const uint32_t dirOffset = u32(&tail[eocd + 16]);
                std::vector<unsigned char> dir(dirSize);
                in.clear();
                in.seekg(dirOffset);
                in.read(reinterpret_cast<char *>(dir.data()), dirSize);
                ok = static_cast<bool>(in);
                size_t at = 0;
                std::vector<unsigned char> inBuf(1u << 20), outBuf(1u << 20);
                for (uint32_t n = 0; ok && n < count; ++n)
                {
                    if (at + 46 > dir.size() || u32(&dir[at]) != 0x02014b50u)
                    {
                        ok = false;
                        break;
                    }
                    const uint32_t method = u16(&dir[at + 10]), crc = u32(&dir[at + 16]);
                    const uint32_t packed = u32(&dir[at + 20]), size = u32(&dir[at + 24]);
                    const uint32_t nameLen = u16(&dir[at + 28]), extraLen = u16(&dir[at + 30]), commentLen = u16(&dir[at + 32]);
                    const uint32_t local = u32(&dir[at + 42]);
                    const std::string name(reinterpret_cast<const char *>(&dir[at + 46]), nameLen);
                    at += 46 + nameLen + extraLen + commentLen;
                    if (name.empty() || name.find("..") != std::string::npos || name.find(':') != std::string::npos || name[0] == '/' ||
                        name[0] == '\\')
                    {
                        ok = false;
                        break;
                    }
                    const fs::path target = folder / fs::path(name);
                    std::error_code ec;
                    if (name.back() == '/')
                    {
                        fs::create_directories(target, ec);
                        continue;
                    }
                    fs::create_directories(target.parent_path(), ec);
                    unsigned char header[30];
                    in.clear();
                    in.seekg(local);
                    in.read(reinterpret_cast<char *>(header), 30);
                    if (!in || u32(header) != 0x04034b50u || (method != 0 && method != 8))
                    {
                        ok = false;
                        break;
                    }
                    in.seekg(static_cast<std::streamoff>(local) + 30 + u16(header + 26) + u16(header + 28));
                    const fs::path temp = target.wstring() + L".unzipping";
                    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                    unsigned long sum = crc32(0, nullptr, 0);
                    uint64_t written = 0, left = packed;
                    ZStream s{};
                    if (method == 8 && init(&s, -15, version(), static_cast<int>(sizeof(ZStream))) != 0)
                        ok = false;
                    int status = 0;
                    while (ok && out && left > 0)
                    {
                        const uint32_t now = static_cast<uint32_t>(std::min<uint64_t>(left, inBuf.size()));
                        in.read(reinterpret_cast<char *>(inBuf.data()), now);
                        if (!in)
                        {
                            ok = false;
                            break;
                        }
                        left -= now;
                        if (method == 0)
                        {
                            out.write(reinterpret_cast<const char *>(inBuf.data()), now);
                            sum = crc32(sum, inBuf.data(), now);
                            written += now;
                            continue;
                        }
                        s.next_in = inBuf.data();
                        s.avail_in = now;
                        do
                        {
                            s.next_out = outBuf.data();
                            s.avail_out = static_cast<unsigned>(outBuf.size());
                            status = inflate(&s, 0);
                            if (status != 0 && status != 1)
                            {
                                ok = false;
                                break;
                            }
                            const unsigned got = static_cast<unsigned>(outBuf.size()) - s.avail_out;
                            out.write(reinterpret_cast<const char *>(outBuf.data()), got);
                            sum = crc32(sum, outBuf.data(), got);
                            written += got;
                        } while (s.avail_out == 0 && status != 1);
                    }
                    if (method == 8)
                        end(&s);
                    out.close();
                    if (!ok || !out || written != size || sum != crc)
                    {
                        ok = false;
                        fs::remove(temp, ec);
                        break;
                    }
                    fs::rename(temp, target, ec);
                    if (ec)
                    {
                        fs::remove(target, ec);
                        fs::rename(temp, target, ec);
                        ok = !ec;
                    }
                }
            }
        }
        FreeLibrary(z);
        std::fprintf(stderr, "[launcher] unpacked %s with zlib1.dll: %s\n", narrow(zip.wstring()).c_str(), ok ? "ok" : "FAILED");
        return ok;
    }

    bool unpackGameProgram()
    {
        std::lock_guard<std::mutex> lock(g_unpackMutex);
        std::error_code ec;
        // LOCAL PATCH (smaller download): bin/Shinobi.pack (both game programs, LZMA, launcher_pack.cpp) when it is there,
        // else the older bin/Shinobi.zip (a folder from v0.5.1 or before).
        const fs::path pack = g_runner.parent_path() / "Shinobi.pack";
        const bool usePack = fs::exists(pack, ec);
        const fs::path packed = usePack ? pack : g_runner.parent_path() / "Shinobi.zip";
        // LOCAL PATCH: unpacked again whenever the zip is not the one last unpacked (its size and date are kept next to it
        // in Shinobi.zip.unpacked). A new version extracted over an old folder used to keep the old Shinobi.exe, because
        // the zip was only unpacked when the exe was missing: the update's fixes never reached the game.
        const fs::path marker = g_runner.parent_path() / "Shinobi.zip.unpacked";
        std::string stamp;
        if (fs::exists(packed, ec))
        {
            const auto size = fs::file_size(packed, ec);
            const auto when = fs::last_write_time(packed, ec).time_since_epoch().count();
            stamp = std::to_string(size) + " " + std::to_string(static_cast<long long>(when));
        }
        if (fs::exists(g_runner, ec))
        {
            if (stamp.empty())
                return true; // a development build: no zip
            std::ifstream in(marker);
            std::string seen;
            std::getline(in, seen);
            // LOCAL PATCH (v0.7.3, player report "avutil-59 is not found"): the same package, but a file of it gone (an
            // antivirus quarantine, a store update removing files not in the release zip): unpacked again.
            const std::string gone = (seen == stamp && usePack) ? launcher::pack::firstMissing(pack, g_runner.parent_path())
                                                                : std::string();
            if (seen == stamp && gone.empty())
                return true;
            if (!gone.empty())
                std::fprintf(stderr, "[launcher] %s is missing from bin: unpacking the game program again\n", gone.c_str());
            else
                std::fprintf(stderr, "[launcher] %s is newer than the unpacked game program: unpacking it again\n",
                             usePack ? "Shinobi.pack" : "Shinobi.zip");
        }
        if (!fs::exists(packed, ec))
            return false;
        if (usePack)
        {
            const std::string error = launcher::pack::unpack(pack, g_runner.parent_path());
            std::fprintf(stderr, "[launcher] unpacked Shinobi.pack: %s\n", error.empty() ? "ok" : error.c_str());
            if (!error.empty())
                return false;
            std::ofstream(marker, std::ios::trunc) << stamp << "\n";
            fs::remove(g_runner.parent_path() / "Shinobi.zip", ec); // left over from before the update: not needed any more
            return fs::exists(g_runner, ec);
        }
        wchar_t system[MAX_PATH] = {};
        GetSystemDirectoryW(system, MAX_PATH);
        std::wstring unpack = L"\"" + std::wstring(system) + L"\\tar.exe\" -xf \"" + packed.wstring() + L"\" -C \"" +
                              g_runner.parent_path().wstring() + L"\"";
        STARTUPINFOW info{};
        info.cb = sizeof(info);
        PROCESS_INFORMATION process{};
        DWORD code = 1;
        if (!_wgetenv(L"SHINOBI_LAUNCHER_NO_TAR") && // test hook: take the Wine/Proton path (no tar.exe) on Windows
            CreateProcessW(nullptr, unpack.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &info, &process))
        {
            WaitForSingleObject(process.hProcess, 180000);
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        // No tar.exe (Wine/Proton, old Windows 10) or it failed: unzip it ourselves with bin's zlib1.dll.
        if (code != 0 && unzipWithZlib(packed, g_runner.parent_path()))
            code = 0;
        if (code == 0 && !stamp.empty())
        {
            std::ofstream out(marker, std::ios::trunc);
            out << stamp << "\n";
        }
        return fs::exists(g_runner, ec);
    }

    // The game's files (extracted from the user's disc image) are in the game folder.
    bool gameInstalled()
    {
        std::error_code ec;
        return fs::exists(g_gameDir / kBootFile, ec) && fs::exists(g_gameDir / "MODULE", ec);
    }

    // Where the game keeps its saves: "saves" next to the launcher (the game's [saves] memcard_folder is ../saves/mc0).
    fs::path savesFolder()
    {
        return (g_gameDir.parent_path() / "saves").lexically_normal();
    }

    // The disc image remembered in launcher.ini [game] iso is still where it was.
    bool isoFileExists()
    {
        const std::string saved = g_mine.get("game", "iso", "");
        std::error_code ec;
        return !saved.empty() && fs::exists(fs::path(widen(saved)), ec);
    }

    // A "Select your game file" dialog; empty when cancelled.
    fs::path browseForImage()
    {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return {};
        const COMDLG_FILTERSPEC filters[] = {{L"PS2 disc images (*.iso, *.bin, *.img, *.chd)", L"*.iso;*.bin;*.img;*.chd"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, filters);
        dialog->SetTitle(L"Select your Shinobi (USA) disc image");
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        const std::string last = g_mine.get("game", "iso", "");
        if (!last.empty())
        {
            ComPtr<IShellItem> folder;
            const std::wstring parent = fs::path(widen(last)).parent_path().wstring();
            if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&folder))))
                dialog->SetFolder(folder.Get());
        }
        if (FAILED(dialog->Show(g_hwnd)))
            return {};
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item)))
            return {};
        PWSTR name = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name)
            return {};
        fs::path path(name);
        CoTaskMemFree(name);
        return path;
    }

    // LOCAL PATCH: a "Choose launcher music" dialog; empty when cancelled.
    // forGame: the jukebox (the game decodes mp3, ogg and wav); else the launcher's own music (anything WebView2 plays).
    fs::path browseForMusic(bool forGame = false, const std::wstring &title = L"Choose the launcher's music")
    {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return {};
        const COMDLG_FILTERSPEC launcherFilters[] = {
            {L"Music (*.mp3, *.ogg, *.m4a, *.wav, *.flac, *.opus, *.webm)", L"*.mp3;*.ogg;*.m4a;*.wav;*.flac;*.opus;*.webm"},
            {L"All files", L"*.*"}};
        const COMDLG_FILTERSPEC gameFilters[] = {{L"Music (*.mp3, *.ogg, *.wav)", L"*.mp3;*.ogg;*.wav"}};
        if (forGame)
            dialog->SetFileTypes(1, gameFilters);
        else
            dialog->SetFileTypes(2, launcherFilters);
        dialog->SetTitle(title.c_str());
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        if (FAILED(dialog->Show(g_hwnd)))
            return {};
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item)))
            return {};
        PWSTR name = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name)
            return {};
        fs::path path(name);
        CoTaskMemFree(name);
        return path;
    }

    // LOCAL PATCH: the Jukebox (ps2_jukebox.cpp in the game). A track's replacement is jukebox/<TRACK>.mp3|ogg|wav beside
    // the game folder; the name the player knows it by is in launcher.ini [jukebox] <TRACK> = <file name>.
    bool jukeboxTrack(const std::string &key)
    {
        if (key.size() < 8 || key.size() > 24)
            return false;
        for (char c : key)
            if (!(std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_'))
                return false;
        return key.rfind("BGM", 0) == 0;
    }

    fs::path jukeboxFile(const std::string &key)
    {
        std::error_code ec;
        for (const char *ext : {".mp3", ".ogg", ".wav"})
        {
            const fs::path path = g_root / "jukebox" / (key + ext);
            if (fs::exists(path, ec))
                return path;
        }
        return {};
    }

    // {"BGM01_00_00T":"my song.mp3",...} for the page: the tracks that have a file.
    std::string jukeboxJson()
    {
        std::string out = "{";
        std::error_code ec;
        bool first = true;
        for (const auto &entry : fs::directory_iterator(g_root / "jukebox", ec))
        {
            const std::string key = narrow(entry.path().stem().wstring());
            if (!jukeboxTrack(key) || jukeboxFile(key) != entry.path())
                continue;
            std::string shown = g_mine.get("jukebox", key.c_str(), "");
            if (shown.empty())
                shown = narrow(entry.path().filename().wstring());
            out += (first ? "" : ",") + launcher::jsonString(key) + ":" + launcher::jsonString(shown);
            first = false;
        }
        return out + "}";
    }

    void jukeboxSet(const std::string &key, bool pick)
    {
        if (!jukeboxTrack(key))
            return;
        std::error_code ec;
        fs::path chosen;
        if (pick)
        {
            chosen = browseForMusic(true, L"Choose music for this track (mp3, ogg or wav)");
            if (chosen.empty())
                return;
        }
        fs::create_directories(g_root / "jukebox", ec);
        for (const char *ext : {".mp3", ".ogg", ".wav"})
            fs::remove(g_root / "jukebox" / (key + ext), ec);
        if (pick)
        {
            std::wstring ext = chosen.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            const fs::path target = g_root / "jukebox" / (fs::path(widen(key)).wstring() + ext);
            if (!fs::copy_file(chosen, target, fs::copy_options::overwrite_existing, ec))
            {
                post(L"juke-error:Could not copy that file into the jukebox folder.");
                return;
            }
            g_mine.set("jukebox", key.c_str(), narrow(chosen.filename().wstring()));
        }
        else
            g_mine.set("jukebox", key.c_str(), "");
        g_mine.save((g_root / "launcher.ini").string());
        post(widen("juke:" + key + "|" + (pick ? narrow(chosen.filename().wstring()) : std::string())));
    }

    // "Browse...": pick, check, remember. The page hears iso:ok|<path>|<text> or iso:bad|<path>|<text>.
    void browseGameFile()
    {
        const fs::path path = browseForImage();
        if (path.empty())
            return;
        const launcher::iso::Check check = launcher::iso::check(path, kBootFile);
        if (check.ok)
        {
            g_mine.set("game", "iso", narrow(path.wstring()));
            g_mine.save((g_root / "launcher.ini").string());
        }
        post(widen(std::string(check.ok ? "iso:ok|" : "iso:bad|") + narrow(path.wstring()) + "|" + check.message));
    }

    // "Set up": copy the game's files out of the remembered disc image, on a worker thread.
    void installFromImage()
    {
        if (g_installing.exchange(true))
            return;
        const fs::path image = fs::path(widen(g_mine.get("game", "iso", "")));
        if (image.empty() || !fs::exists(image))
        {
            g_installing = false;
            post(L"install-error:Choose your Shinobi (USA) disc image first (Browse...).");
            return;
        }
        if (g_installThread.joinable())
            g_installThread.join();
        const fs::path destination = g_gameDir;
        g_installThread = std::thread([image, destination]() {
            postFromThread(L"install:0|Preparing the game program...");
            unpackGameProgram();
            const launcher::iso::Check check = launcher::iso::check(image, kBootFile);
            if (!check.ok)
            {
                postFromThread(widen("install-error:" + check.message));
                g_installing = false;
                return;
            }
            int lastPercent = -1;
            const std::string error = launcher::iso::extractAll(image, destination, [&](int percent, const std::string &file) {
                if (percent != lastPercent)
                {
                    lastPercent = percent;
                    postFromThread(widen("install:" + std::to_string(percent) + "|" + file));
                }
                return true;
            });
            postFromThread(error.empty() ? std::wstring(L"installed") : widen("install-error:" + error));
            g_installing = false;
        });
    }

    // ------------------------------------------------------------------ settings <-> page values

    using launcher::jsonString;
    using launcher::truthy;

    std::wstring fileUrl(const fs::path &path);

    // ------------------------------------------------------------------ the game manual (the player's own PDF)

    // LOCAL PATCH: "Manual" shows the manual as a book. The PDF is the player's own (launcher.ini [manual] pdf); its pages
    // are drawn once into game/manual (launcher_manual.cpp). The page hears manual-progress:<i>|<n>, then
    // manual:<pages>|<url of game/manual/page_> or manual-error:<text>.
    std::atomic<bool> g_manualBusy{false};
    std::thread g_manualThread;

    fs::path manualFolder()
    {
        return g_gameDir / "manual";
    }

    fs::path browseForPdf()
    {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return {};
        const COMDLG_FILTERSPEC filters[] = {{L"PDF manual (*.pdf)", L"*.pdf"}};
        dialog->SetFileTypes(1, filters);
        dialog->SetTitle(L"Select your Shinobi manual (PDF)");
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        if (FAILED(dialog->Show(g_hwnd)))
            return {};
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item)))
            return {};
        PWSTR name = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name)
            return {};
        fs::path path(name);
        CoTaskMemFree(name);
        return path;
    }

    void postManualReady(uint32_t pages)
    {
        std::error_code ec;
        const auto stamp = fs::last_write_time(manualFolder() / "page_001.png", ec).time_since_epoch().count();
        postFromThread(widen("manual:" + std::to_string(pages) + "|" + narrow(fileUrl(manualFolder() / "page_")) + "|" +
                             std::to_string(static_cast<long long>(stamp % 1000000007LL))));
    }

    void openManual(bool choose)
    {
        if (g_manualBusy.load())
            return;
        const uint32_t cached = launcher::manual::cachedPages(manualFolder());
        if (!choose && cached > 0u)
        {
            postManualReady(cached);
            return;
        }
        fs::path pdf = fs::path(widen(g_mine.get("manual", "pdf", "")));
        std::error_code ec;
        // The manual that comes with the launcher (launcher_assets/manual.pdf), unless the player chose another one.
        if (!choose && (pdf.empty() || !fs::exists(pdf, ec)) && fs::exists(g_root / "launcher_assets" / "manual.pdf", ec))
            pdf = g_root / "launcher_assets" / "manual.pdf";
        if (choose || pdf.empty() || !fs::exists(pdf, ec))
        {
            pdf = browseForPdf();
            if (pdf.empty())
            {
                post(L"manual-cancel");
                return;
            }
            g_mine.set("manual", "pdf", narrow(pdf.wstring()));
            g_mine.save((g_root / "launcher.ini").string());
        }
        if (g_manualThread.joinable())
            g_manualThread.join();
        g_manualBusy = true;
        post(L"manual-progress:0|0");
        g_manualThread = std::thread([pdf]() {
            const std::string error = launcher::manual::render(pdf, manualFolder(), 1600u, [](uint32_t i, uint32_t n) {
                postFromThread(widen("manual-progress:" + std::to_string(i) + "|" + std::to_string(n)));
            });
            if (error.empty())
                postManualReady(launcher::manual::cachedPages(manualFolder()));
            else
                postFromThread(widen("manual-error:" + error));
            g_manualBusy = false;
        });
    }

    // The launcher's background music as a file:// URL, or empty when there is none.
    // LOCAL PATCH: [launcher] music, chosen with Choose... (UTF-8 in launcher.ini; relative = from the game's folder).
    fs::path chosenMusic()
    {
        const std::string configured = g_mine.get("launcher", "music", "");
        if (configured.empty())
            return {};
        const fs::path path(widen(configured));
        return path.is_absolute() ? path : g_root / path;
    }

    // The name the launcher shows for the music: the chosen file's, or "Original theme".
    std::string musicName()
    {
        const fs::path chosen = chosenMusic();
        std::error_code ec;
        if (!chosen.empty() && fs::exists(chosen, ec))
            return narrow(chosen.stem().wstring());
        return "Original theme";
    }

    std::wstring musicUrl()
    {
        const fs::path chosen = chosenMusic();
        std::error_code ec;
        // A chosen file that has gone (moved, deleted): the original theme instead of silence.
        if (!chosen.empty() && fs::exists(chosen, ec))
            return fileUrl(chosen);
        for (const char *ext : {".mp3", ".ogg", ".m4a", ".wav", ".flac", ".opus", ".webm"})
        {
            const fs::path path = g_root / "launcher_assets" / (std::string("music") + ext);
            if (fs::exists(path))
                return fileUrl(path);
        }
        return std::wstring();
    }

    std::string defaultRenderScale(); // below: the first-run resolution for this PC's graphics card

    // window.__launcherInit for the page: its settings, bindings and mods as they are on disk now.
    std::string initScript()
    {
        const launcher::SettingGet get = [](const char *section, const char *key, const char *fallback)
        { return g_game.get(section, key, fallback); };
        std::ostringstream js;
        js << "window.__launcherInit={" << launcher::pageValues(get, truthy(g_mine.get("launcher", "diagnostics", "false")));
        // Launcher music: [launcher] music = <file>, else launcher_assets/music.(mp3|ogg|m4a|wav|flac|opus|webm).
        const std::wstring music = musicUrl();
        js << ",music:" << (music.empty() ? std::string("null") : jsonString(narrow(music)))
           << ",musicName:" << jsonString(musicName())
           << ",padKind:" << jsonString(launcher::pads::kind())
           // LOCAL PATCH (v0.7.3): Reset to Defaults uses the same resolution as a fresh install on this graphics card.
           << ",defRes:" << jsonString([] {
                  static const std::string scale = defaultRenderScale();
                  return scale == "2" ? "720p" : scale == "3" ? "1080p" : "1440p";
              }())
           << ",version:" << jsonString([] {
                  // LOCAL PATCH: the installed version (version.txt, what the updater compares), for the Feature Set page
                  std::ifstream in(g_root / "version.txt");
                  std::string v;
                  std::getline(in, v);
                  v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char c) { return std::isspace(c) != 0; }), v.end());
                  return v;
              }())
           << ",jukebox:" << jukeboxJson()
           << ",muted:" << (truthy(g_mine.get("launcher", "music_muted", "false")) ? "true" : "false")
           << ",iso:" << jsonString(g_mine.get("game", "iso", ""))
           << ",savesDir:" << jsonString(narrow(savesFolder().wstring()))
           << ",isoFound:" << (isoFileExists() ? "true" : "false") << ",installed:" << (gameInstalled() ? "true" : "false") << "};";
        return js.str();
    }

    // The page's key=value lines into settings.ini / launcher.ini.
    bool applyAndSave(const std::map<std::string, std::string> &v, std::string &error)
    {
        const launcher::SettingGet get = [](const char *section, const char *key, const char *fallback)
        { return g_game.get(section, key, fallback); };
        const launcher::SettingSet set = [](const char *section, const char *key, const std::string &value)
        { g_game.set(section, key, value); };
        const bool diagnostics = launcher::applyPageValues(v, get, set);
        g_mine.set("launcher", "diagnostics", diagnostics ? "true" : "false");
        if (!g_game.save((g_gameDir / "settings.ini").string()))
        {
            error = "Could not write " + (g_gameDir / "settings.ini").string();
            return false;
        }
        g_mine.save((g_root / "launcher.ini").string());
        return true;
    }

    // ------------------------------------------------------------------ the game process

    bool startGame(std::string &error)
    {
        // LOCAL PATCH (v0.7): the studio intro video comes from the release only when it is switched on.
        {
            const std::string on = g_game.get("game", "studio_intro", "false");
            if (on == "true" || on == "1")
                launcher::update::studioIntro(g_root, g_gameDir / "intro" / "studio_intro.mp4");
        }
        // The game program is built twice: for AVX2 (bin/Shinobi.exe, faster) and for any SSE4.1 CPU (bin/Shinobi-SSE4.exe,
        // 2008 and newer, the same minimum as PCSX2). On a CPU without what a program needs Windows would close it at once
        // with no message, so the right one is picked here. SHINOBI_FORCE_SSE4=1 starts the fallback on any PC (testing).
        const bool avx2 = IsProcessorFeaturePresent(40 /* PF_AVX2_INSTRUCTIONS_AVAILABLE */) && !_wgetenv(L"SHINOBI_FORCE_SSE4");
        if (!avx2 && !IsProcessorFeaturePresent(37 /* PF_SSE4_1_INSTRUCTIONS_AVAILABLE */))
        {
            error = "This PC's processor is too old for the game: it needs SSE4.1 (Intel Core 2 from 2008, AMD from 2011, "
                    "or newer), the same as the PCSX2 emulator.";
            return false;
        }
        if (!gameInstalled())
        {
            error = "The game's files are not set up yet. Open Game File, choose your Shinobi (USA) disc image and press Set up.";
            return false;
        }
        unpackGameProgram(); // normally done long before (at start-up and when the game file is set up)
        fs::path runner = g_runner;
        if (!avx2)
        {
            // LOCAL PATCH (smaller download): fetched once from the GitHub release (started at launcher start-up).
            int percent = 0;
            std::string fetchError;
            const auto state = launcher::update::oldCpuBuild(g_root, &percent, &fetchError);
            if (state == launcher::update::OldCpuBuild::Downloading)
            {
                error = "Getting the version of the game for your processor (one time only, about 19 MB): " +
                        std::to_string(percent) + "%. Press Play again in a moment.";
                return false;
            }
            if (state == launcher::update::OldCpuBuild::Failed)
            {
                error = fetchError;
                return false;
            }
            runner = g_runner.parent_path() / widen("Shinobi-" + launcher::update::oldCpuTier() + ".exe"); // (GitHub #28: AVX or SSE4)
            if (!fs::exists(runner))
            {
                error = "This PC's processor does not support AVX2, and the version of the game for older processors "
                        "(bin\\Shinobi-" + launcher::update::oldCpuTier() + ".exe) is missing. Download the latest release again.";
                return false;
            }
            std::fprintf(stderr, "[launcher] no AVX2: starting %s\n", narrow(runner.wstring()).c_str());
        }
        if (!fs::exists(runner))
        {
            error = "Game not found: " + narrow(runner.wstring());
            return false;
        }
        if (truthy(g_mine.get("launcher", "diagnostics", "false")))
        {
            // Same recording as "Play Shinobi (PCSX2 GS, profile session).bat" (scripts/session_report.py reads it).
            const char *env[][2] = {{"PS2X_PCSX2_LOG", "1"},
                                    {"PS2X_STATS_LOG", "1"},
                                    {"PS2X_VU1_STATS", "1"},
                                    {"PS2X_PROFILE", "10:600:profile"},
                                    {"PS2X_PROFILE_RAW", "captures/profile_raw.txt"},
                                    {"PS2X_AUDIO_DUMP", "captures/audio_played.raw"},
                                    {"PS2X_SPU2_TRACE", "1"},
                                    {"PS2X_FRAME_TIMELINE", "1"},
                                    {"PS2X_ADX_CHECK", "1"},
                                    {"PS2X_IOP_WATCH", "1"},
                                    {"PS2X_IOP_PC_STATS", "1"}}; // [iop-pc]: where the sound processor's time goes
            for (const auto &e : env)
                SetEnvironmentVariableA(e[0], e[1]);
        }
        std::error_code ec;
        fs::create_directories(g_gameDir / "captures", ec);
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE log = CreateFileW((g_gameDir / "captures" / "last_run.log").wstring().c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
        si.hStdError = log != INVALID_HANDLE_VALUE ? log : nullptr;
        PROCESS_INFORMATION pi{};
        std::wstring command = L"\"" + runner.wstring() + L"\" \"" + (g_gameDir / "SLUS_204.59").wstring() + L"\"";
        const BOOL ok = CreateProcessW(runner.wstring().c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                       nullptr, g_gameDir.wstring().c_str(), &si, &pi);
        if (log != INVALID_HANDLE_VALUE)
            CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        if (!ok)
        {
            error = "Could not start the game (Windows error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(pi.hThread);
        g_gameProcess = pi.hProcess;
        g_gamePid = pi.dwProcessId;
        g_reportedRunning = false;
        SetTimer(g_hwnd, kWatchTimer, 400, nullptr);
        return true;
    }

    BOOL CALLBACK findGameWindow(HWND hwnd, LPARAM found)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == g_gamePid && IsWindowVisible(hwnd))
        {
            *reinterpret_cast<bool *>(found) = true;
            return FALSE;
        }
        return TRUE;
    }

    void watchGame()
    {
        if (!g_gameProcess)
        {
            KillTimer(g_hwnd, kWatchTimer);
            return;
        }
        if (WaitForSingleObject(g_gameProcess, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(g_gameProcess);
            g_gameProcess = nullptr;
            KillTimer(g_hwnd, kWatchTimer);
            post(L"exited");
            return;
        }
        if (!g_reportedRunning)
        {
            bool found = false;
            EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&found));
            if (found)
            {
                g_reportedRunning = true;
                post(L"running");
            }
        }
    }

    // ------------------------------------------------------------------ messages from the page

    // After an update: start the new launcher and close this one.
    void restartLauncher()
    {
        const std::wstring exe = (g_root / "Shinobi Launcher.exe").wstring();
        std::wstring cmd = L"\"" + exe + L"\"";
        STARTUPINFOW info{};
        info.cb = sizeof(info);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.wstring().c_str(), &info, &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }

    void onMessage(const std::string &message)
    {
        std::istringstream in(message);
        std::string command, line;
        std::getline(in, command);
        std::map<std::string, std::string> values;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
                values[line.substr(0, eq)] = line.substr(eq + 1);
        }
        std::string error;
        if (command == "save")
        {
            post(applyAndSave(values, error) ? L"saved" : widen("error:" + error));
        }
        else if (command == "play")
        {
            if (g_gameProcess)
                return;
            if (!applyAndSave(values, error) || !startGame(error))
                post(widen("error:" + error));
            else
                post(L"starting");
        }
        else if (command == "stop")
        {
            if (g_gameProcess)
            {
                // A normal close first (the game saves its settings), then force it after a moment if needed.
                bool closed = false;
                EnumWindows(
                    [](HWND hwnd, LPARAM closedFlag) -> BOOL {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(hwnd, &pid);
                        if (pid == g_gamePid && IsWindowVisible(hwnd))
                        {
                            PostMessageW(hwnd, WM_CLOSE, 0, 0);
                            *reinterpret_cast<bool *>(closedFlag) = true;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&closed));
                if (!closed || WaitForSingleObject(g_gameProcess, 3000) == WAIT_TIMEOUT)
                    TerminateProcess(g_gameProcess, 0);
            }
        }
        else if (command == "mute")
        {
            g_mine.set("launcher", "music_muted", values["muted"] == "1" ? "true" : "false");
            g_mine.save((g_root / "launcher.ini").string());
        }
        else if (command == "hap")
        {
            // LOCAL PATCH: menu haptics (the page drives them: a thump per move, beads while the right stick scrolls),
            // following the haptics switch and strength. kind: move | bead | tab | press | back; v: 0..100 (bead speed).
            const std::string on = g_game.get("input", "haptics", "true");
            if (on == "false" || on == "0" || on == "off")
                return;
            const float strength = std::clamp(std::atoi(g_game.get("input", "haptic_strength", "100").c_str()), 0, 100) / 100.0f;
            const std::string kind = values["kind"];
            const float speed = std::clamp(std::atoi(values["v"].c_str()), 0, 100) / 100.0f;
            float left = 0.0f, right = 0.0f;
            int ms = 20;
            // Both actuators together, so every tap is felt in the centre of the pad as one thump (the actuators need
            // ~40 ms and a strong level to be felt clearly in vibration-emulation mode).
            if (kind == "move")
                left = 0.95f, right = 1.0f, ms = 60;          // a good centred thump per move
            else if (kind == "bead")
                left = right = 0.75f + 0.25f * speed, ms = 45; // one clear bead (the page sends one every 0.35 s)
            else if (kind == "tab")
                left = 1.0f, right = 1.0f, ms = 80;           // a page turn: heavier and longer
            else if (kind == "press")
                left = 1.0f, right = 1.0f, ms = 65;           // a firm click
            else if (kind == "back")
                left = 0.80f, right = 0.80f, ms = 50;
            else
                return;
            launcher::dualsense::tap(left * strength, right * strength, ms);
        }
        else if (command == "eudiscpick" || command == "eudiscclear")
        {
            // LOCAL PATCH: Languages. The player's Shinobi (Europe) disc image: French, German, Italian and Spanish are
            // read from it by the game (ps2_lang_overlay.cpp); only its place is remembered ([language] eu_disc).
            // The page hears eudisc:ok|<path>|<text>, eudisc:bad|<path>|<text> or eudisc:none.
            if (command == "eudiscclear")
            {
                g_game.set("language", "eu_disc", "");
                g_game.save((g_gameDir / "settings.ini").string());
                post(L"eudisc:none");
                return;
            }
            ComPtr<IFileOpenDialog> dialog;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
                return;
            const COMDLG_FILTERSPEC filters[] = {{L"PS2 disc images (*.iso, *.bin, *.img)", L"*.iso;*.bin;*.img"}, {L"All files", L"*.*"}};
            dialog->SetFileTypes(2, filters);
            dialog->SetTitle(L"Select your Shinobi (Europe) disc image");
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
            ComPtr<IShellItem> item;
            PWSTR picked = nullptr;
            if (FAILED(dialog->Show(g_hwnd)) || FAILED(dialog->GetResult(&item)) ||
                FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &picked)) || !picked)
                return;
            const fs::path path(picked);
            const std::wstring pickedPath = path.wstring();
            CoTaskMemFree(picked);
            // The in-game language overlay reads this second disc as raw ISO bytes, not through the launcher's CHD reader.
            std::string ext = narrow(path.extension().wstring());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".chd")
            {
                post(L"eudisc:bad|" + pickedPath +
                     L"|CHD images cannot be used for the European language disc; choose a .iso, .bin or .img instead.");
                return;
            }
            const launcher::iso::Check check = launcher::iso::check(path, "SCES_514.28");
            std::string text;
            if (check.ok)
            {
                g_game.set("language", "eu_disc", narrow(path.wstring()));
                g_game.save((g_gameDir / "settings.ini").string());
                text = "Shinobi (Europe) found: French, German, Italian and Spanish are ready.";
            }
            else if (check.bootFile.empty())
                text = check.message;
            else if (check.bootFile == "SLUS_204.59")
                text = "That is your US disc. Choose your Shinobi (Europe) disc image (SCES_514.28).";
            else
                text = "This is a different disc (" + check.bootFile + "). Languages need Shinobi (Europe), SCES_514.28.";
            post(widen(std::string(check.ok ? "eudisc:ok|" : "eudisc:bad|") + narrow(path.wstring()) + "|" + text));
        }
        else if (command == "reload")
        {
            // LOCAL PATCH: the Refresh button. The page starts again (loading screen, intro) with its settings read
            // from disk now: a fresh copy of window.__launcherInit is added (it runs after the first one, so it wins;
            // the previous refresh's copy is removed), then the page reloads. Not while the game runs: the page would
            // forget the game is running.
            if (g_gameProcess)
                post(L"juke-error:Close the game first, then refresh the launcher.");
            else if (g_view)
            {
                static std::wstring s_refreshScript;
                if (!s_refreshScript.empty())
                    g_view->RemoveScriptToExecuteOnDocumentCreated(s_refreshScript.c_str());
                g_view->AddScriptToExecuteOnDocumentCreated(
                    widen(initScript()).c_str(),
                    Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                        [](HRESULT, LPCWSTR id) -> HRESULT {
                            if (id)
                                s_refreshScript = id;
                            if (g_view)
                                g_view->Reload();
                            return S_OK;
                        })
                        .Get());
            }
        }
        else if (command == "jukepick" || command == "jukedefault")
            jukeboxSet(values["key"], command == "jukepick");
        else if (command == "jukefolder")
        {
            std::error_code ec;
            fs::create_directories(g_root / "jukebox", ec);
            ShellExecuteW(nullptr, L"open", (g_root / "jukebox").wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (command == "musicpick" || command == "musicdefault")
        {
            // LOCAL PATCH: the launcher's music. Choose... keeps the file where it is and remembers its place in
            // launcher.ini (which updates never replace); Default goes back to launcher_assets/music.*.
            if (command == "musicpick")
            {
                const fs::path path = browseForMusic();
                if (path.empty())
                    return;
                g_mine.set("launcher", "music", narrow(path.wstring()));
            }
            else
                g_mine.set("launcher", "music", "");
            g_mine.save((g_root / "launcher.ini").string());
            post(L"music:" + musicUrl() + L"|" + widen(musicName()));
        }
        else if (command == "minimize")
            ShowWindow(g_hwnd, SW_MINIMIZE);
        else if (command == "maximize")
            ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        else if (command == "update")
        {
            // Only a release download from GitHub is ever installed.
            const std::string url = values["url"];
            if (g_gameProcess)
                post(L"update-error:Close the game first, then press Update again.");
            else if (url.rfind("https://github.com/", 0) != 0 || url.find("/releases/download/") == std::string::npos)
                post(L"update-error:That is not a release download.");
            else
                launcher::update::install(g_root, url, [](const std::wstring &m) { postFromThread(m); }, restartLauncher);
        }
        else if (command == "close")
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        else if (command == "manual")
            openManual(false);
        else if (command == "manualpick")
            openManual(true);
        else if (command == "browseiso")
            browseGameFile();
        else if (command == "installiso")
            installFromImage();
        else if (command == "opensaves")
        {
            std::error_code ec;
            fs::create_directories(savesFolder(), ec);
            ShellExecuteW(nullptr, L"open", savesFolder().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (command == "openfolder")
            ShellExecuteW(nullptr, L"open", g_gameDir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ------------------------------------------------------------------ WebView2

    // The page is a fixed 1000 x 640 design: fill the web view and scale the page to fit it (1.0 at the normal size,
    // larger when maximised; the page centres itself and the spare edge is the page background).
    void fitPage()
    {
        if (!g_controller)
            return;
        RECT bounds;
        GetClientRect(g_hwnd, &bounds);
        g_controller->put_Bounds(bounds);
        const double scale = GetDpiForWindow(g_hwnd) / 96.0;
        const double w = (bounds.right - bounds.left) / scale;
        const double h = (bounds.bottom - bounds.top) / scale;
        if (w < 1.0 || h < 1.0)
            return; // minimised
        const double zoom = std::min(w / kPageWidth, h / kPageHeight);
        g_controller->put_ZoomFactor(std::max(zoom, 0.25));
        static int s_lastMax = -1;
        const int maxed = IsZoomed(g_hwnd) ? 1 : 0;
        if (maxed != s_lastMax && g_view)
        {
            s_lastMax = maxed;
            post(maxed ? L"max:1" : L"max:0");
        }
    }

    std::wstring fileUrl(const fs::path &path)
    {
        std::wstring p = path.wstring();
        std::replace(p.begin(), p.end(), L'\\', L'/');
        std::wstring out = L"file:///";
        for (wchar_t c : p)
        {
            if (c == L' ')
                out += L"%20";
            else
                out += c;
        }
        return out;
    }

    void saveShotAndQuit()
    {
        if (!g_view)
            return;
        ComPtr<IStream> stream;
        if (FAILED(SHCreateStreamOnFileEx(g_shotPath.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr,
                                          &stream)))
        {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            return;
        }
        g_view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
                               Callback<ICoreWebView2CapturePreviewCompletedHandler>([stream](HRESULT) -> HRESULT {
                                   stream->Commit(STGC_DEFAULT);
                                   PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
                                   return S_OK;
                               }).Get());
    }

    void createWebView()
    {
        wchar_t local[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
        const std::wstring dataDir = std::wstring(local) + L"\\ShinobiLauncher\\WebView2";
        const std::string init = initScript();
        // The launcher music may start on its own (no click first). Note: launchers sharing the user data folder must
        // use the same arguments, so this is fixed rather than depending on whether music exists.
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        options->put_AdditionalBrowserArguments(L"--autoplay-policy=no-user-gesture-required");
        CreateCoreWebView2EnvironmentWithOptions(
            nullptr, dataDir.c_str(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [init](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
                    if (FAILED(result) || !env)
                    {
                        // No WebView2 (a Windows 10 without it): the game still starts, through runSimple().
                        g_webviewMissing = true;
                        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
                        return S_OK;
                    }
                    return env->CreateCoreWebView2Controller(
                        g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                    [init](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT {
                                        if (FAILED(result) || !controller)
                                            return S_OK;
                                        g_controller = controller;
                                        g_controller->get_CoreWebView2(&g_view);
                                        ComPtr<ICoreWebView2Controller2> controller2;
                                        if (SUCCEEDED(g_controller.As(&controller2)))
                                            controller2->put_DefaultBackgroundColor({255, 0x16, 0x16, 0x1a});
                                        RECT bounds;
                                        GetClientRect(g_hwnd, &bounds);
                                        g_controller->put_Bounds(bounds);
                                        ComPtr<ICoreWebView2Settings> settings;
                                        g_view->get_Settings(&settings);
                                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                                        settings->put_IsStatusBarEnabled(FALSE);
                                        settings->put_IsZoomControlEnabled(FALSE);
                                        settings->put_AreDevToolsEnabled(std::getenv("SHINOBI_LAUNCHER_DEVTOOLS") ? TRUE : FALSE);
                                        // The page's title bar (CSS app-region: drag) acts as the window's own: drag,
                                        // double-click to maximise, Snap. Its buttons are app-region: no-drag.
                                        ComPtr<ICoreWebView2Settings9> settings9;
                                        if (SUCCEEDED(settings.As(&settings9)))
                                            settings9->put_IsNonClientRegionSupportEnabled(TRUE);
                                        fitPage();
                                        g_view->AddScriptToExecuteOnDocumentCreated(widen(init).c_str(), nullptr);
                                        EventRegistrationToken token;
                                        g_view->add_WebMessageReceived(
                                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                                [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                                    LPWSTR text = nullptr;
                                                    if (SUCCEEDED(args->TryGetWebMessageAsString(&text)) && text)
                                                    {
                                                        onMessage(narrow(text));
                                                        CoTaskMemFree(text);
                                                    }
                                                    return S_OK;
                                                })
                                                .Get(),
                                            &token);
                                        g_view->add_NavigationCompleted(
                                            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                                [](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) -> HRESULT {
                                                    post(IsZoomed(g_hwnd) ? L"max:1" : L"max:0");
                                                    if (!g_shotPath.empty())
                                                    {
                                                        // Test hook: run a script against the page (e.g. open a menu) before the picture.
                                                        if (const char *js = std::getenv("SHINOBI_LAUNCHER_JS"))
                                                            g_view->ExecuteScript(widen("setTimeout(()=>{" + std::string(js) + "},1200);").c_str(),
                                                                                  nullptr);
                                                        SetTimer(g_hwnd, kShotTimer, [] { const char *ms = std::getenv("SHINOBI_LAUNCHER_SHOT_MS"); return ms ? static_cast<UINT>(std::atoi(ms)) : 3000u; }(), nullptr);
                                                    }
                                                    return S_OK;
                                                })
                                                .Get(),
                                            &token);
                                        g_view->Navigate(fileUrl(g_page).c_str());
                                        return S_OK;
                                    })
                                    .Get());
                })
                .Get());
    }

    // LOCAL PATCH (first-run resolution by graphics card, player reports 2026-11-24: GTX 1650 Ti laptop and RX 570 found
    // 4x too heavy): the card with the most video memory is the one the game will use (a laptop's own GPU, not the
    // built-in one). Older / entry cards start at 2x (1280x896), mid-range at 3x, the rest at 4x. Only a fresh install;
    // the player's own choice is never changed.
    std::string defaultRenderScale()
    {
        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
            return "4";
        std::wstring name;
        SIZE_T bestMemory = 0;
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                desc.DedicatedVideoMemory >= bestMemory)
            {
                bestMemory = desc.DedicatedVideoMemory;
                name = desc.Description;
            }
            adapter->Release();
        }
        factory->Release();
        std::string n;
        for (wchar_t c : name)
            n.push_back(c < 128 ? static_cast<char>(std::toupper(static_cast<int>(c))) : '?');
        const double gb = static_cast<double>(bestMemory) / (1024.0 * 1024.0 * 1024.0);
        static const std::regex weak(
            "INTEL|UHD|IRIS|\\bMX ?\\d|GT \\d|GTX ?(7|9)\\d\\d|GTX ?10[35]0|GTX ?1060|GTX ?1650|RX ?[45]\\d\\d\\b|\\bR[579] |VEGA|"
            "RADEON\\(TM\\) GRAPHICS|RADEON GRAPHICS|RX ?6[45]\\d\\dM|RX ?640\\b");
        static const std::regex mid("GTX ?10[78]0|GTX ?16[67]0|RTX ?20[56]0|RTX ?[34]050|RX ?5[56]00|RX ?6[56]00|RX ?7600|ARC\\(TM\\) A[37]|ARC A[37]");
        std::string scale = "4";
        if (n.empty() || gb < 3.0 || std::regex_search(n, weak))
            scale = "2";
        else if (gb < 6.0 || std::regex_search(n, mid))
            scale = "3";
        std::fprintf(stderr, "[launcher] graphics card %s (%.1f GB): first-run resolution %sx\n", n.c_str(), gb,
                     scale.c_str());
        return scale;
    }

    // No settings.ini yet (never saved from the launcher): the launcher's defaults, so the game does not start on its own
    // built-in ones. Only keys the file does not have yet are written.
    void writeDefaultSettings()
    {
        if (fs::exists(g_gameDir / "settings.ini") && !g_game.get("video", "window", "").empty())
            return;
        if (g_game.get("video", "render_scale", "").empty())
            g_game.set("video", "render_scale", defaultRenderScale());
        const char *defaults[][3] = {{"video", "renderer", "pcsx2"},       {"video", "render_scale", "4"},
                                     {"video", "pcsx2_renderer", "d3d11"}, {"video", "fps_target", "60"},
                                     {"video", "aspect", "16:9"},          {"video", "vsync", "true"},
                                     {"video", "anisotropic", "2"},        {"video", "sharpening", "20"},
                                     {"video", "antialias", "true"},      {"video", "edge_aa", "false"}, {"video", "window", "borderless"},
                                     {"audio", "stereo_repair", "copy_left"}, {"saves", "memcard_folder", "../saves/mc0"},
                                     {"video", "frame_generation", "false"}, {"video", "low_latency", "true"},
                                     {"performance", "stats_overlay", "true"}, {"game", "hide_cinema_bars", "true"},
                                     {"game", "fair_final_boss", "true"}, {"game", "scarf_collision", "false"},
                                     {"game", "skip_intro", "true"}, {"game", "studio_intro", "false"},
                                     {"game", "scarf_color", "original"},
                                     {"video", "hd_textures", "true"}}; // LOCAL PATCH: HD texture packs on from the start
        for (const auto &d : defaults)
            if (g_game.get(d[0], d[1], "").empty())
                g_game.set(d[0], d[1], d[2]);
        g_game.save((g_gameDir / "settings.ini").string());
    }

    // Starts the game and waits for it to close; its exit code, or -1 when it could not start (the reason is shown).
    int playAndWait()
    {
        writeDefaultSettings();
        std::string error;
        if (!startGame(error))
        {
            MessageBoxW(nullptr, widen(error).c_str(), L"Shinobi", MB_ICONERROR);
            return -1;
        }
        WaitForSingleObject(g_gameProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(g_gameProcess, &code);
        CloseHandle(g_gameProcess);
        g_gameProcess = nullptr;
        return static_cast<int>(code);
    }

    // The launcher without its window, for Wine/Proton (Linux, Steam Deck), where the WebView2 page does not work, and
    // for a Windows without WebView2: the first time it asks for the disc image and copies the game's files out of it
    // (a small progress box), then it starts the game. Settings are changed in game (Mod Menu) or in game/settings.ini.
    int runSimple()
    {
        unpackGameProgram();
        if (!gameInstalled())
        {
            fs::path image = fs::path(widen(g_mine.get("game", "iso", "")));
            std::error_code ec;
            for (;;)
            {
                if (image.empty() || !fs::exists(image, ec))
                {
                    if (MessageBoxW(nullptr,
                                    L"First start: choose your disc image (.iso, .bin, .img or .chd) of Shinobi for "
                                    L"PlayStation 2, USA release (SLUS-20459).\n\nThe game's files (about 1.2 GB) are "
                                    L"copied out of it once.",
                                    L"Shinobi", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                        return 1;
                    image = browseForImage();
                    if (image.empty())
                        return 1;
                }
                const launcher::iso::Check check = launcher::iso::check(image, kBootFile);
                if (check.ok)
                    break;
                MessageBoxW(nullptr, widen(check.message).c_str(), L"Shinobi", MB_ICONWARNING);
                image.clear();
            }
            g_mine.set("game", "iso", narrow(image.wstring()));
            g_mine.save((g_root / "launcher.ini").string());

            const int w = 460, h = 90;
            HWND box = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"Copying the game's files...",
                                       WS_POPUP | WS_CAPTION | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                       (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                                       nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            std::atomic<int> percent{0};
            std::atomic<bool> done{false};
            std::string error;
            std::thread copy([&]() {
                error = launcher::iso::extractAll(image, g_gameDir, [&](int p, const std::string &) {
                    percent = p;
                    return true;
                });
                done = true;
            });
            int shown = -1;
            while (!done)
            {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                if (box && percent != shown)
                {
                    shown = percent;
                    SetWindowTextW(box, (L"Copying the game's files from your disc image... " + std::to_wstring(shown) + L"%").c_str());
                }
            }
            copy.join();
            if (box)
                DestroyWindow(box);
            if (!error.empty())
            {
                MessageBoxW(nullptr, widen(error).c_str(), L"Shinobi", MB_ICONERROR);
                return 4;
            }
        }
        // LOCAL PATCH: no AVX2 -> the game program for older processors, fetched once (waited for here).
        if (!IsProcessorFeaturePresent(40 /* PF_AVX2_INSTRUCTIONS_AVAILABLE */) || _wgetenv(L"SHINOBI_FORCE_SSE4"))
        {
            int percent = 0;
            std::string fetchError;
            while (launcher::update::oldCpuBuild(g_root, &percent, &fetchError) == launcher::update::OldCpuBuild::Downloading)
                Sleep(200);
            if (!fetchError.empty())
            {
                MessageBoxW(nullptr, widen(fetchError).c_str(), L"Shinobi", MB_ICONERROR);
                return 6;
            }
        }
        return playAndWait() < 0 ? 5 : 0;
    }

    LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_SIZE:
            fitPage();
            return 0;
        case WM_NCCALCSIZE:
            // LOCAL PATCH: a resizable window (edges, Windows snap layouts, Win+arrows) that still looks frameless: the
            // standard frame minus its caption. The side and bottom borders Windows 10/11 draw are invisible resize
            // grips outside the picture; the title bar is the page's own.
            if (wp)
            {
                auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lp);
                const LONG top = params->rgrc[0].top;
                DefWindowProcW(hwnd, msg, wp, lp);
                params->rgrc[0].top = top + (IsZoomed(hwnd) ? GetSystemMetricsForDpi(SM_CYFRAME, GetDpiForWindow(hwnd)) +
                                                                  GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(hwnd))
                                                            : 0);
                return 0;
            }
            break;
        case WM_GETMINMAXINFO:
        {
            // Not smaller than half the page's own size.
            {
                auto *minInfo = reinterpret_cast<MINMAXINFO *>(lp);
                const UINT wdpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
                minInfo->ptMinTrackSize = {MulDiv(kPageWidth / 2, static_cast<int>(wdpi), 96),
                                           MulDiv(kPageHeight / 2, static_cast<int>(wdpi), 96)};
            }
            // (Maximised, Windows keeps the framed window to the work area itself.)
            return 0;
        }
        case WM_DPICHANGED:
        {
            const RECT *suggested = reinterpret_cast<const RECT *>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case kPageMessage:
        {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lp));
            if (text)
                post(*text);
            return 0;
        }
        case WM_TIMER:
            if (wp == kWatchTimer)
                watchGame();
            else if (wp == kShotTimer)
            {
                KillTimer(hwnd, kShotTimer);
                saveShotAndQuit();
            }
            return 0;
        case WM_DESTROY:
            if (g_controller)
                g_controller->Close();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_root = fs::path(exe).parent_path();
    g_mine.load((g_root / "launcher.ini").string());
    g_gameDir = fs::path(g_mine.get("paths", "game", (g_root / "game").string()));
    // The game program: bin\Shinobi.exe in a release folder, else the one the build makes (development).
    {
        std::error_code ec;
        const fs::path released = g_root / "bin" / "Shinobi.exe";
        const fs::path built = g_root / "devilkings-fork/out/build/ps2xRuntime/ps2EntryRunner.exe";
        // A release folder has bin\Shinobi.exe, or (just downloaded) bin\Shinobi.pack / bin\Shinobi.zip that is unpacked
        // into it. LOCAL PATCH (GitHub issue #13): Shinobi.pack counts too (a fresh v0.6.0 install has only that, so the
        // launcher took itself for a development folder), and anything that is not a development folder (no build there)
        // is a release folder.
        const bool inRelease = fs::exists(released, ec) || fs::exists(g_root / "bin" / "Shinobi.pack", ec) ||
                               fs::exists(g_root / "bin" / "Shinobi.zip", ec) || !fs::exists(built, ec);
        g_runner = fs::path(g_mine.get("paths", "runner", (inRelease ? released : built).string()));
    }
    g_page = fs::path(g_mine.get("paths", "page", (g_root / "launcher_assets/launcher.html").string()));
    if (const wchar_t *dir = _wgetenv(L"SHINOBI_LAUNCHER_GAMEDIR")) // test hook: work on a copy of settings.ini
        g_gameDir = fs::path(dir);
    if (const wchar_t *page = _wgetenv(L"SHINOBI_LAUNCHER_PAGE"))
        g_page = fs::path(page);
    g_game.load((g_gameDir / "settings.ini").string());
    // LOCAL PATCH (first-run resolution by graphics card): shown on the page from the start (written with the
    // other defaults at the first Play or Save).
    if (g_game.get("video", "render_scale", "").empty())
        g_game.set("video", "render_scale", defaultRenderScale());
    if (const char *shot = std::getenv("SHINOBI_LAUNCHER_SHOT"))
        g_shotPath = widen(shot);

    // Windowless modes for RecompLauncher (recomplauncher.json). Without these arguments nothing changes.
    //   --prepare --game <disc image>   unpack the game program, check the image, copy the game's files out of it
    //   --play                          start the game the same way Play does and wait for it to close
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool prepare = false, play = false;
        fs::path image;
        for (int i = 1; argv && i < argc; ++i)
        {
            const std::wstring a = argv[i];
            if (a == L"--prepare")
                prepare = true;
            else if (a == L"--play")
                play = true;
            else if (a == L"--game" && i + 1 < argc)
            {
                std::wstring p = argv[++i];
                // Linux (Proton/Wine): RecompLauncher passes a Unix path such as /home/you/Shinobi.iso, which Windows
                // code reads as \home\you\... on the current drive and cannot open. Turn it into Wine's DOS path (Z:\...).
                if (p.size() > 1 && p[0] == L'/' && p[1] != L'/')
                {
                    using DosName = WCHAR *(__cdecl *)(const char *);
                    const auto toDos = reinterpret_cast<DosName>(
                        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
                    WCHAR *dos = toDos ? toDos(narrow(p).c_str()) : nullptr;
                    if (dos)
                    {
                        p = dos;
                        HeapFree(GetProcessHeap(), 0, dos);
                    }
                    else
                    {
                        std::replace(p.begin(), p.end(), L'/', L'\\');
                        p = L"Z:" + p;
                    }
                }
                image = p;
            }
        }
        if (argv)
            LocalFree(argv);
        auto say = [](const std::string &line) {
            const std::string text = line + "\n";
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        };
        if (prepare)
        {
            if (image.empty())
                image = fs::path(widen(g_mine.get("game", "iso", "")));
            say("progress 0.02 Unpacking the game program");
            if (!unpackGameProgram())
            {
                say("error The game program (bin\\Shinobi.pack) could not be unpacked. Re-download the game, or check that "
                    "your antivirus has not quarantined files in the bin folder.");
                return 2;
            }
            const launcher::iso::Check check = launcher::iso::check(image, kBootFile);
            if (!check.ok)
            {
                say("error " + check.message);
                return 3;
            }
            g_mine.set("game", "iso", narrow(image.wstring()));
            g_mine.save((g_root / "launcher.ini").string());
            int lastPercent = -1;
            const std::string error = launcher::iso::extractAll(image, g_gameDir, [&](int percent, const std::string &file) {
                if (percent != lastPercent)
                {
                    lastPercent = percent;
                    char line[64];
                    snprintf(line, sizeof(line), "progress %.2f ", 0.05 + 0.95 * percent / 100.0);
                    say(line + file);
                }
                return true;
            });
            if (!error.empty())
            {
                say("error " + error);
                return 4;
            }
            say("progress 1.00 Done");
            return 0;
        }
        if (play)
        {
            const int code = playAndWait();
            return code < 0 ? 5 : code;
        }
    }

    // Wine/Proton (Linux, Steam Deck): the WebView2 page does not run there, so no window: set up once, then play.
    // SHINOBI_LAUNCHER_WEBVIEW=1 tries the normal window anyway.
    if (underWine() && !_wgetenv(L"SHINOBI_LAUNCHER_WEBVIEW"))
        return runSimple();

    if (g_shotPath.empty())
        g_unpackThread = std::thread([]() { unpackGameProgram(); }); // the zipped game program, unpacked in the background
    // LOCAL PATCH: updates from the project's GitHub releases (launcher_update.cpp). The check waits a moment so the page
    // is up to receive its answer.
    launcher::update::cleanup(g_root);
    // LOCAL PATCH: a PC without AVX2 starts fetching its game program at once, so it is there by the time Play is pressed.
    if (g_shotPath.empty() &&
        (!IsProcessorFeaturePresent(40 /* PF_AVX2_INSTRUCTIONS_AVAILABLE */) || _wgetenv(L"SHINOBI_FORCE_SSE4")))
        launcher::update::oldCpuBuild(g_root);
    if (g_shotPath.empty())
        launcher::update::startCheck(g_root, [](const std::wstring &message) {
            Sleep(3000);
            postFromThread(message);
            // Test hook: SHINOBI_LAUNCHER_AUTOUPDATE=<png path> installs the update at once (no click), and the restarted
            // launcher saves a screenshot there and quits, so a whole update can be checked unattended on a copy.
            if (const wchar_t *shot = _wgetenv(L"SHINOBI_LAUNCHER_AUTOUPDATE"))
            {
                const std::wstring shotPath = shot;
                const std::string m = narrow(message);
                const size_t bar = m.find('|');
                if (m.rfind("update:", 0) == 0 && bar != std::string::npos)
                {
                    std::fprintf(stderr, "[update] test hook: installing %s\n", m.substr(7, bar - 7).c_str());
                    launcher::update::install(
                        g_root, m.substr(bar + 1),
                        [](const std::wstring &p) {
                            std::fprintf(stderr, "[update] %s\n", narrow(p).c_str());
                            postFromThread(p);
                        },
                        [shotPath]() {
                            SetEnvironmentVariableW(L"SHINOBI_LAUNCHER_AUTOUPDATE", nullptr);
                            SetEnvironmentVariableW(L"SHINOBI_LAUNCHER_SHOT", shotPath.c_str());
                            restartLauncher();
                        });
                }
            }
        });

    if (!fs::exists(g_page))
    {
        // Started from inside the downloaded .zip (Windows shows the zip like a folder and runs a lone copy of the program):
        // say what to do instead of failing on a missing file.
        if (!fs::exists(g_root / "launcher_assets"))
        {
            MessageBoxW(nullptr,
                        L"This launcher was started from inside the downloaded .zip file.\n\n"
                        L"Please unzip the whole download first: right-click the .zip file, choose \"Extract All...\", then open "
                        L"the new folder and double-click Shinobi Launcher.exe there.",
                        L"Shinobi Launcher", MB_ICONINFORMATION);
            return 1;
        }
        MessageBoxW(nullptr, (L"The launcher page is missing:\n" + g_page.wstring()).c_str(), L"Shinobi Launcher", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0x16, 0x16, 0x1a));
    wc.lpszClassName = L"ShinobiLauncher";
    // The icon built into the exe (launcher_icon.rc), else launcher_assets/icon.ico.
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon)
        wc.hIcon = static_cast<HICON>(LoadImageW(nullptr, (g_root / "launcher_assets" / "icon.ico").wstring().c_str(),
                                                 IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    // The page is 1000 x 640 CSS pixels and scales to fit the window (fitPage). LOCAL PATCH: it opens large, 85% of the
    // screen's height (or width, whichever fits) at the page's shape, centred; never smaller than the page at the
    // monitor's scale.
    const UINT dpi = GetDpiForSystem();
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workW = work.right - work.left, workH = work.bottom - work.top;
    int w = MulDiv(kPageWidth, static_cast<int>(dpi), 96);
    int h = MulDiv(kPageHeight, static_cast<int>(dpi), 96);
    {
        int bigH = workH * 85 / 100;
        int bigW = MulDiv(bigH, kPageWidth, kPageHeight);
        if (bigW > workW * 90 / 100)
        {
            bigW = workW * 90 / 100;
            bigH = MulDiv(bigW, kPageHeight, kPageWidth);
        }
        if (bigW > w)
        {
            w = bigW;
            h = bigH;
        }
        // LOCAL PATCH (small / high-DPI screens, player report 2026-11-24 "the UI scales oddly, I can't see the whole
        // menu"): the page at the monitor's scale can be bigger than the screen (1080p laptop at 150-175%); the
        // window then went past the bottom and the Save / Low-end PC buttons were cut off. Never bigger than the work
        // area: the page shrinks to fit (fitPage).
        if (h > workH)
        {
            h = workH;
            w = MulDiv(h, kPageWidth, kPageHeight);
        }
        if (w > workW)
        {
            w = workW;
            h = MulDiv(w, kPageHeight, kPageWidth);
        }
    }
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Shinobi (PS2 Recom) Launcher - by TheRealAfsan",
                             WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - w) / 2,
                             work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, instance, nullptr);
    const int corner = 2; // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    // LOCAL PATCH: opens maximised (windowed, filling the screen); Restore gives the large centred window above.
    ShowWindow(g_hwnd, g_shotPath.empty() ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);
    createWebView();
    launcher::dualsense::start([](const std::wstring &m) { postFromThread(m); });
    launcher::pads::watch([](const std::wstring &m) { postFromThread(m); }); // LOCAL PATCH: L1 / LB / L on the page

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_unpackThread.joinable())
        g_unpackThread.join();
    if (g_installThread.joinable())
        g_installThread.join(); // a running set-up finishes (the copy is resumable: files already there are skipped)
    if (g_manualThread.joinable())
        g_manualThread.join();
    if (g_gameProcess)
        CloseHandle(g_gameProcess);
    if (g_webviewMissing)
    {
        g_hwnd = nullptr;
        return runSimple();
    }
    CoUninitialize();
    return 0;
}

// LOCAL PATCH: for launcher_update.cpp: unzip with bin's zlib1.dll when tar.exe is missing (Wine/Proton) or fails.
bool launcherUnzipWithZlib(const std::filesystem::path &zip, const std::filesystem::path &folder,
                           const std::filesystem::path &zlibDll)
{
    return unzipWithZlib(zip, folder, zlibDll);
}
