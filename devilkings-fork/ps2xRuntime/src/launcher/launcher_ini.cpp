#include "launcher_ini.h"

#include <cctype>
#include <fstream>

namespace launcher
{
    namespace
    {
        std::string trim(const std::string &text)
        {
            size_t begin = 0, end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return text.substr(begin, end - begin);
        }

        bool isSection(const std::string &line, std::string &name)
        {
            const std::string t = trim(line);
            if (t.size() < 2 || t.front() != '[' || t.back() != ']')
                return false;
            name = trim(t.substr(1, t.size() - 2));
            return true;
        }

        bool isKey(const std::string &line, std::string &key, std::string &value)
        {
            const std::string t = trim(line);
            if (t.empty() || t[0] == ';' || t[0] == '#')
                return false;
            const size_t eq = t.find('=');
            if (eq == std::string::npos)
                return false;
            key = trim(t.substr(0, eq));
            value = trim(t.substr(eq + 1));
            return !key.empty();
        }
    }

    bool Ini::load(const std::string &path)
    {
        m_lines.clear();
        std::ifstream in(path);
        if (!in)
            return false;
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m_lines.push_back(line);
        }
        return true;
    }

    bool Ini::save(const std::string &path) const
    {
        std::ofstream out(path, std::ios::trunc);
        if (!out)
            return false;
        for (const std::string &line : m_lines)
            out << line << '\n';
        return static_cast<bool>(out);
    }

    std::string Ini::get(const std::string &section, const std::string &key, const std::string &fallback) const
    {
        std::string current, name, k, v;
        for (const std::string &line : m_lines)
        {
            if (isSection(line, name))
                current = name;
            else if (current == section && isKey(line, k, v) && k == key)
                return v;
        }
        return fallback;
    }

    void Ini::set(const std::string &section, const std::string &key, const std::string &value)
    {
        std::string current, name, k, v;
        int sectionLine = -1, lastInSection = -1;
        for (size_t i = 0; i < m_lines.size(); ++i)
        {
            if (isSection(m_lines[i], name))
            {
                current = name;
                if (current == section)
                {
                    sectionLine = static_cast<int>(i);
                    lastInSection = static_cast<int>(i);
                }
                continue;
            }
            if (current != section)
                continue;
            if (isKey(m_lines[i], k, v))
            {
                lastInSection = static_cast<int>(i);
                if (k == key)
                {
                    m_lines[i] = key + " = " + value;
                    return;
                }
            }
        }
        if (sectionLine < 0)
        {
            if (!m_lines.empty() && !trim(m_lines.back()).empty())
                m_lines.push_back("");
            m_lines.push_back("[" + section + "]");
            m_lines.push_back(key + " = " + value);
            return;
        }
        m_lines.insert(m_lines.begin() + lastInSection + 1, key + " = " + value);
    }
}
