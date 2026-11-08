// Shinobi launcher: a small INI editor that keeps the file's comments, order and unknown keys (game/settings.ini is
// also written by the game's own F1 menu).
#pragma once

#include <string>
#include <vector>

namespace launcher
{
    class Ini
    {
    public:
        bool load(const std::string &path);
        bool save(const std::string &path) const;
        [[nodiscard]] std::string get(const std::string &section, const std::string &key, const std::string &fallback) const;
        void set(const std::string &section, const std::string &key, const std::string &value);

    private:
        std::vector<std::string> m_lines;
    };
}
