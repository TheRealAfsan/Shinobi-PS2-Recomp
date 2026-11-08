// Shinobi launcher page <-> settings.ini. The same page (launcher_assets/launcher.html, from the Claude Design file) is the
// launcher (launcher_webview.cpp, reading settings.ini with launcher::Ini) and the in-game menu (ps2_ingame_menu.cpp,
// reading the game's live ps2x::settings): both use this one mapping, so the two can never disagree.
#pragma once

#include <functional>
#include <map>
#include <string>

namespace launcher
{
    // Reads [section] key, `fallback` when absent.
    using SettingGet = std::function<std::string(const char *section, const char *key, const char *fallback)>;
    using SettingSet = std::function<void(const char *section, const char *key, const std::string &value)>;

    bool truthy(const std::string &value);
    std::string jsonString(const std::string &value);

    // The page's starting values as a JavaScript object literal body: settings:{...},binds:{...},mods:{...}
    // (no braces around it, so callers add their own fields). `diagnostics` is the launcher-only Record diagnostics mod.
    std::string pageValues(const SettingGet &get, bool diagnostics);

    // The page's key=value lines (see send() in the page) into the settings. Returns the page's Record diagnostics choice.
    bool applyPageValues(const std::map<std::string, std::string> &values, const SettingGet &get, const SettingSet &set);
}
