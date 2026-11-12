// LOCAL PATCH: the in-game menu (see runtime/ps2_ingame_menu.h). A WebView2 child of the game window showing the launcher
// page with window.__launcherInit.ingame = true. The page's messages are the launcher's (see launcher_webview.cpp):
//   resume | save | restart | quit | openfolder | close  (+ key=value lines for save/restart)
// and it gets back: saved:live | saved:restart | error:<text>.
#include "runtime/ps2_ingame_menu.h"

#include "runtime/ps2_settings.h"
#include "../launcher/launcher_settings_map.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#endif

namespace ps2x::pcsx2gs
{
    void applyLiveSettings(); // gs_pcsx2.cpp
}

namespace fs = std::filesystem;

namespace ps2x::ingame
{
#if defined(_WIN32)
    namespace
    {
        using Microsoft::WRL::Callback;
        using Microsoft::WRL::ComPtr;

        enum class State
        {
            Idle,     // nothing created yet
            Creating, // environment / controller on the way
            Ready,
            Failed,
        };

        State g_state = State::Idle;
        bool g_open = false;
        bool g_wantOpen = false; // asked to open while still creating
        bool g_manualMode = false; // LOCAL PATCH: opened with F4: the game manual only
        HWND g_parent = nullptr;
        ComPtr<ICoreWebView2Controller> g_controller;
        ComPtr<ICoreWebView2> g_view;
        std::wstring g_scriptId;
        Request g_request = Request::None;
        RECT g_lastBounds{};

        std::wstring widen(const std::string &text)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            std::wstring out(static_cast<size_t>(n > 0 ? n : 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), n);
            out.resize(wcslen(out.c_str()));
            return out;
        }

        std::string narrow(const std::wstring &text)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(n > 0 ? n : 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), n, nullptr, nullptr);
            out.resize(std::strlen(out.c_str()));
            return out;
        }

        // The page: PS2X_MENU_PAGE, else launcher_assets/launcher.html in this folder or the one above (the game runs in game/).
        fs::path pagePath()
        {
            if (const char *e = std::getenv("PS2X_MENU_PAGE"))
                return fs::path(e);
            std::error_code ec;
            for (const char *candidate : {"launcher_assets/launcher.html", "../launcher_assets/launcher.html"})
                if (fs::exists(candidate, ec))
                    return fs::absolute(candidate, ec);
            return fs::path();
        }

        std::wstring fileUrl(const fs::path &path)
        {
            std::wstring p = path.wstring();
            std::wstring out = L"file:///";
            for (wchar_t c : p)
                out += c == L'\\' ? std::wstring(L"/") : c == L' ' ? std::wstring(L"%20") : std::wstring(1, c);
            return out;
        }

        launcher::SettingGet settingGet()
        {
            return [](const char *section, const char *key, const char *fallback)
            {
                const std::string value = ps2x::settings::get(section, key);
                return value.empty() ? std::string(fallback) : value;
            };
        }

        std::string initScript()
        {
            std::ostringstream js;
            js << "window.__launcherInit={" << launcher::pageValues(settingGet(), false)
               << ",music:null,muted:true,ingame:true,manualOnly:" << (g_manualMode ? "true" : "false") << ",restart:" << (ps2x::settings::restartNeeded() ? "true" : "false") << "};";
            return js.str();
        }

        void post(const wchar_t *message)
        {
            if (g_view)
                g_view->PostWebMessageAsString(message);
        }

        // The page's values into the live settings, then the file. True when a restart is needed for some of them.
        bool applyAndSave(const std::map<std::string, std::string> &values)
        {
            const launcher::SettingSet set = [](const char *section, const char *key, const std::string &value)
            { ps2x::settings::set(section, key, value); };
            launcher::applyPageValues(values, settingGet(), set);
            ps2x::settings::save();
            // LOCAL PATCH: resolution, filtering, edge AA and blending go to the running renderer straight away.
            ps2x::pcsx2gs::applyLiveSettings();
            return ps2x::settings::restartNeeded();
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
            if (command == "resume" || command == "close")
                close();
            else if (command == "save")
                post(applyAndSave(values) ? L"saved:restart" : L"saved:live");
            else if (command == "restart")
            {
                applyAndSave(values);
                g_request = Request::Restart;
            }
            else if (command == "quit")
                g_request = Request::Quit;
            else if (command == "manual")
            {
                // LOCAL PATCH: the manual's pages as the launcher prepared them (game/manual/page_NNN.png).
                std::error_code ec;
                const fs::path folder = fs::current_path(ec) / "manual";
                unsigned pages = 0;
                char name[32];
                for (;;)
                {
                    std::snprintf(name, sizeof(name), "page_%03u.png", pages + 1u);
                    if (!fs::exists(folder / name, ec))
                        break;
                    ++pages;
                }
                if (pages == 0)
                    post(L"manual-error:Open the manual once in the launcher (Manual button) and it will be here too.");
                else
                {
                    const auto stamp = fs::last_write_time(folder / "page_001.png", ec).time_since_epoch().count();
                    post((L"manual:" + std::to_wstring(pages) + L"|" + fileUrl(folder / "page_") + L"|" +
                          std::to_wstring(static_cast<long long>(stamp % 1000000007LL)))
                             .c_str());
                }
            }
            else if (command == "openfolder")
            {
                std::error_code ec;
                ShellExecuteW(nullptr, L"open", fs::current_path(ec).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }

        // The panel: the page's 1000 x 640 layout, as large as 88% of the window allows, centred.
        void layout()
        {
            if (!g_controller || !g_parent)
                return;
            RECT client{};
            GetClientRect(g_parent, &client);
            const double cw = client.right - client.left, ch = client.bottom - client.top;
            if (cw < 50 || ch < 50)
                return;
            const double scale = std::min(cw * 0.88 / 1000.0, ch * 0.88 / 640.0);
            const int w = static_cast<int>(1000.0 * scale), h = static_cast<int>(640.0 * scale);
            RECT bounds{static_cast<LONG>((cw - w) / 2), static_cast<LONG>((ch - h) / 2), 0, 0};
            bounds.right = bounds.left + w;
            bounds.bottom = bounds.top + h;
            if (std::memcmp(&bounds, &g_lastBounds, sizeof(RECT)) == 0)
                return;
            g_lastBounds = bounds;
            g_controller->put_Bounds(bounds);
            const double dpi = GetDpiForWindow(g_parent) / 96.0;
            g_controller->put_ZoomFactor(scale / (dpi > 0.0 ? dpi : 1.0));
        }

        // Fresh values every time the menu opens: the init script is replaced, then the page reloaded.
        void showPage()
        {
            if (!g_view)
                return;
            const fs::path page = pagePath();
            if (page.empty())
            {
                std::fprintf(stderr, "[menu] launcher_assets/launcher.html not found (PS2X_MENU_PAGE sets it)\n");
                close();
                return;
            }
            if (!g_scriptId.empty())
                g_view->RemoveScriptToExecuteOnDocumentCreated(g_scriptId.c_str());
            const std::wstring url = fileUrl(page);
            g_view->AddScriptToExecuteOnDocumentCreated(
                widen(initScript()).c_str(),
                Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                    [url](HRESULT, LPCWSTR id) -> HRESULT
                    {
                        g_scriptId = id ? id : L"";
                        if (g_view)
                            g_view->Navigate(url.c_str());
                        return S_OK;
                    })
                    .Get());
            g_lastBounds = RECT{};
            layout();
            g_controller->put_IsVisible(TRUE);
            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }

        void create()
        {
            g_state = State::Creating;
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (com == RPC_E_CHANGED_MODE)
                std::fprintf(stderr, "[menu] the window thread is not single-threaded COM; the menu may not open\n");
            wchar_t local[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
            const std::wstring dataDir = std::wstring(local) + L"\\ShinobiLauncher\\WebView2Game";
            auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
            const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
                nullptr, dataDir.c_str(), options.Get(),
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [](HRESULT result, ICoreWebView2Environment *env) -> HRESULT
                    {
                        if (FAILED(result) || !env)
                        {
                            std::fprintf(stderr, "[menu] WebView2 is not available (0x%08lx)\n", static_cast<unsigned long>(result));
                            g_state = State::Failed;
                            close();
                            return S_OK;
                        }
                        return env->CreateCoreWebView2Controller(
                            g_parent, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                          [](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT
                                          {
                                              if (FAILED(result) || !controller)
                                              {
                                                  g_state = State::Failed;
                                                  close();
                                                  return S_OK;
                                              }
                                              g_controller = controller;
                                              g_controller->get_CoreWebView2(&g_view);
                                              g_controller->put_IsVisible(FALSE);
                                              ComPtr<ICoreWebView2Controller2> controller2;
                                              if (SUCCEEDED(g_controller.As(&controller2)))
                                                  controller2->put_DefaultBackgroundColor({255, 0x16, 0x16, 0x1a});
                                              ComPtr<ICoreWebView2Settings> settings;
                                              g_view->get_Settings(&settings);
                                              settings->put_AreDefaultContextMenusEnabled(FALSE);
                                              settings->put_IsStatusBarEnabled(FALSE);
                                              settings->put_IsZoomControlEnabled(FALSE);
                                              settings->put_AreDevToolsEnabled(std::getenv("SHINOBI_LAUNCHER_DEVTOOLS") ? TRUE : FALSE);
                                              EventRegistrationToken token;
                                              g_view->add_WebMessageReceived(
                                                  Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                                      [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT
                                                      {
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
                                              g_state = State::Ready;
                                              std::fprintf(stderr, "[menu] ready\n");
                                              if (g_wantOpen)
                                                  showPage();
                                              return S_OK;
                                          })
                                          .Get());
                    })
                    .Get());
            if (FAILED(hr))
            {
                std::fprintf(stderr, "[menu] WebView2 could not start (0x%08lx)\n", static_cast<unsigned long>(hr));
                g_state = State::Failed;
            }
        }
    }

    void openImpl(void *window);

    void open(void *window)
    {
        if (g_open)
            return;
        g_manualMode = false;
        openImpl(window);
    }

    // LOCAL PATCH: F4: the manual alone, rising from the bottom; F4 again slides it down and the game resumes.
    void toggleManual(void *window)
    {
        if (g_open && g_manualMode)
        {
            post(L"manualhide"); // the page slides it away, then asks to resume
            return;
        }
        if (g_open)
            close();
        g_manualMode = true;
        openImpl(window);
    }

    void openImpl(void *window)
    {
        if (g_open)
            return;
        g_parent = static_cast<HWND>(window);
        if (!g_parent || g_state == State::Failed)
            return;
        g_open = true;
        g_wantOpen = true;
        ps2x::settings::setInputBlocked(true);
        std::fprintf(stderr, "[menu] open\n");
        if (g_state == State::Idle)
            create();
        else if (g_state == State::Ready)
            showPage();
    }

    void close()
    {
        if (!g_open)
            return;
        g_open = false;
        g_wantOpen = false;
        if (g_controller)
            g_controller->put_IsVisible(FALSE);
        ps2x::settings::setInputBlocked(false);
        if (g_parent)
            SetFocus(g_parent);
        std::fprintf(stderr, "[menu] closed\n");
    }

    bool isOpen()
    {
        return g_open;
    }

    void update(void *window)
    {
        if (!g_open)
            return;
        g_parent = static_cast<HWND>(window);
        layout();
    }

    void padEvent(const char *name)
    {
        if (!g_open || !name)
            return;
        post((L"pad:" + widen(name)).c_str());
    }

    Request takeRequest()
    {
        const Request request = g_request;
        g_request = Request::None;
        return request;
    }

    bool relaunchGame()
    {
        // The same program, command line and folder; the environment is inherited.
        std::wstring command = GetCommandLineW();
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::error_code ec;
        const std::wstring folder = fs::current_path(ec).wstring();
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &si, &pi))
        {
            std::fprintf(stderr, "[menu] restart failed (Windows error %lu)\n", GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::fprintf(stderr, "[menu] restarting the game\n");
        return true;
    }
#else
    void open(void *) {}
    void close() {}
    bool isOpen() { return false; }
    void update(void *) {}
    void padEvent(const char *) {}
    void toggleManual(void *) {}
    Request takeRequest() { return Request::None; }
    bool relaunchGame() { return false; }
#endif
}
