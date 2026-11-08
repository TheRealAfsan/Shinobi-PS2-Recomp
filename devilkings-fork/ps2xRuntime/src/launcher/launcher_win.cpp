#include "launcher_win.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <filesystem>

namespace launcher
{
    namespace
    {
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
    }

    std::string exeDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path().string();
    }

    bool launchGame(const std::string &runner, const std::string &elf, const std::string &gameDir, const std::string &stderrLog,
                    const std::vector<std::pair<std::string, std::string>> &environment, std::string &error)
    {
        for (const auto &[name, value] : environment)
            SetEnvironmentVariableW(widen(name).c_str(), value.empty() ? nullptr : widen(value).c_str());

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(widen(stderrLog)).parent_path(), ec);
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE log = CreateFileW(widen(stderrLog).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
        si.hStdError = log != INVALID_HANDLE_VALUE ? log : nullptr;
        PROCESS_INFORMATION pi{};
        std::wstring command = L"\"" + widen(runner) + L"\" \"" + widen(elf) + L"\"";
        const BOOL ok = CreateProcessW(widen(runner).c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       widen(gameDir).c_str(), &si, &pi);
        if (log != INVALID_HANDLE_VALUE)
            CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        if (!ok)
        {
            error = "Could not start the game (error " + std::to_string(GetLastError()) + "):\n" + runner;
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    void openWithShell(const std::string &path)
    {
        ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
