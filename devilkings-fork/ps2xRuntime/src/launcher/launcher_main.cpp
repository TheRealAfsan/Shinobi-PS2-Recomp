// Shinobi (PS2 Recomp) Launcher - by TheRealAfsan.
// A raylib window laid out after the launcher mockup: guides, description and buttons on the left, key art in the
// middle, graphical settings on the right. Settings go to game/settings.ini (the same file the game's F1 menu edits);
// launcher-only choices (diagnostics recording) go to launcher.ini next to the launcher.
// Artwork is loaded from launcher_assets/ (keyart.png, logo.png, playstation.png, sega.png, tra.png, guide1.png,
// guide2.png, icon.png); anything missing gets a drawn placeholder.

#include "launcher_ini.h"
#include "launcher_win.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using launcher::Ini;

namespace
{
    // ------------------------------------------------------------------ look

    const Color kFrame{30, 30, 32, 255};
    const Color kRed{200, 28, 34, 255};
    const Color kDarkRed{120, 14, 18, 255};
    const Color kPanel{226, 226, 228, 255};
    const Color kPanelEdge{70, 70, 74, 255};
    const Color kDarkBox{44, 44, 47, 240};
    const Color kInk{22, 22, 24, 255};
    const Color kSoftInk{70, 70, 74, 255};

    Font g_heading{}; // Bahnschrift (condensed headings)
    Font g_bold{};    // Segoe UI Bold (buttons, labels)
    Font g_body{};    // Segoe UI Bold, small text
    Font g_logo{};    // Impact (placeholder logo)
    Font g_kanji{};   // Yu Gothic Bold (decorative hand seals)
    bool g_haveKanji = false;

    const char *kSeals[] = {"臨", "兵", "闘", "者", "皆", "陣", "列", "在", "前"};

    Font loadFont(const char *path, int size, int *codepoints = nullptr, int count = 0)
    {
        if (!FileExists(path))
            return GetFontDefault();
        Font f = LoadFontEx(path, size, codepoints, count);
        SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
        return f;
    }

    void text(const Font &font, const std::string &s, float x, float y, float size, Color color, float spacing = 0.5f)
    {
        DrawTextEx(font, s.c_str(), {x, y}, size, spacing, color);
    }

    float textWidth(const Font &font, const std::string &s, float size, float spacing = 0.5f)
    {
        return MeasureTextEx(font, s.c_str(), size, spacing).x;
    }

    void textCentered(const Font &font, const std::string &s, Rectangle r, float size, Color color)
    {
        const Vector2 m = MeasureTextEx(font, s.c_str(), size, 0.5f);
        DrawTextEx(font, s.c_str(), {r.x + (r.width - m.x) * 0.5f, r.y + (r.height - m.y) * 0.5f}, size, 0.5f, color);
    }

    void wrapped(const Font &font, const std::string &s, Rectangle r, float size, Color color, float lineGap = 1.15f)
    {
        float y = r.y;
        std::string line, word;
        auto flush = [&]()
        {
            text(font, line, r.x, y, size, color);
            y += size * lineGap;
            line.clear();
        };
        for (size_t i = 0; i <= s.size(); ++i)
        {
            const char c = i < s.size() ? s[i] : ' ';
            if (c == '\n' || c == ' ')
            {
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!word.empty() && textWidth(font, candidate, size) > r.width && !line.empty())
                {
                    flush();
                    line = word;
                }
                else if (!word.empty())
                    line = candidate;
                word.clear();
                if (c == '\n')
                    flush();
            }
            else
                word += c;
        }
        if (!line.empty())
            flush();
    }

    // ------------------------------------------------------------------ images

    struct Images
    {
        Texture2D keyart{}, logo{}, playstation{}, sega{}, tra{}, guide1{}, guide2{};
        bool hasKeyart = false, hasLogo = false, hasPlaystation = false, hasSega = false, hasTra = false, hasGuide1 = false,
             hasGuide2 = false;
    };

    bool loadImage(const fs::path &file, Texture2D &out)
    {
        if (!fs::exists(file))
            return false;
        out = LoadTexture(file.string().c_str());
        if (out.id == 0)
            return false;
        SetTextureFilter(out, TEXTURE_FILTER_BILINEAR);
        return true;
    }

    // Draws the image into r, cropped to fill it (like CSS object-fit: cover).
    void drawCover(const Texture2D &t, Rectangle r, Color tint = WHITE)
    {
        const float sx = r.width / t.width, sy = r.height / t.height;
        const float s = std::max(sx, sy);
        const float w = r.width / s, h = r.height / s;
        const Rectangle src{(t.width - w) * 0.5f, (t.height - h) * 0.5f, w, h};
        DrawTexturePro(t, src, r, {0, 0}, 0.0f, tint);
    }

    // Draws the image inside r, keeping its shape.
    void drawContain(const Texture2D &t, Rectangle r, Color tint = WHITE)
    {
        const float s = std::min(r.width / t.width, r.height / t.height);
        const float w = t.width * s, h = t.height * s;
        DrawTexturePro(t, {0, 0, static_cast<float>(t.width), static_cast<float>(t.height)},
                       {r.x + (r.width - w) * 0.5f, r.y + (r.height - h) * 0.5f, w, h}, {0, 0}, 0.0f, tint);
    }

    // ------------------------------------------------------------------ widgets (immediate mode)

    bool g_inputBlocked = false; // a modal or an open dropdown takes the clicks

    bool hovered(Rectangle r, bool ignoreBlock = false)
    {
        return (ignoreBlock || !g_inputBlocked) && CheckCollisionPointRec(GetMousePosition(), r);
    }

    bool clicked(Rectangle r, bool ignoreBlock = false)
    {
        return hovered(r, ignoreBlock) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    }

    // The mockup's buttons: light grey bevel, red trim at both ends, bold dark caps.
    bool button(Rectangle r, const std::string &label, bool ignoreBlock = false, float size = 22.0f)
    {
        const bool hot = hovered(r, ignoreBlock);
        const bool down = hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        DrawRectangleRounded({r.x - 2, r.y - 2, r.width + 4, r.height + 4}, 0.25f, 6, {20, 20, 22, 255});
        DrawRectangleRounded(r, 0.22f, 6, kRed);
        const Rectangle face{r.x + 6, r.y + 2, r.width - 12, r.height - 4};
        const Color top = down ? Color{196, 196, 198, 255} : hot ? Color{250, 250, 252, 255} : Color{238, 238, 240, 255};
        const Color bottom = down ? Color{226, 226, 228, 255} : hot ? Color{214, 214, 218, 255} : Color{196, 196, 200, 255};
        DrawRectangleGradientV(static_cast<int>(face.x), static_cast<int>(face.y), static_cast<int>(face.width),
                               static_cast<int>(face.height), top, bottom);
        textCentered(g_bold, label, {r.x, r.y + (down ? 1.0f : 0.0f), r.width, r.height}, size, kInk);
        return hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    }

    void checkbox(Rectangle box, const std::string &label, bool &value, bool ignoreBlock = false, Color ink = kInk)
    {
        const Rectangle hit{box.x, box.y, box.width + 8 + textWidth(g_bold, label, 19), box.height};
        if (clicked(hit, ignoreBlock))
            value = !value;
        DrawRectangleRec(box, WHITE);
        DrawRectangleLinesEx(box, 1.5f, kPanelEdge);
        if (value)
        {
            DrawLineEx({box.x + 5, box.y + box.height * 0.55f}, {box.x + box.width * 0.42f, box.y + box.height - 5}, 3.0f, kInk);
            DrawLineEx({box.x + box.width * 0.42f, box.y + box.height - 5}, {box.x + box.width - 4, box.y + 4}, 3.0f, kInk);
        }
        text(g_bold, label, box.x + box.width + 8, box.y + (box.height - 19) * 0.5f - 1, 19, ink);
    }

    // A row of options in one bar, the chosen one dark (the mockup's "OpenGL | Vulkan" and "Off | FXAA | MSAA").
    void segmented(Rectangle r, const std::vector<std::string> &items, int &index, bool ignoreBlock = false)
    {
        DrawRectangleRec(r, Color{250, 250, 252, 255});
        DrawRectangleLinesEx(r, 1.5f, kPanelEdge);
        const float w = r.width / static_cast<float>(items.size());
        for (size_t i = 0; i < items.size(); ++i)
        {
            const Rectangle cell{r.x + w * i, r.y, w, r.height};
            if (clicked(cell, ignoreBlock))
                index = static_cast<int>(i);
            const bool on = static_cast<int>(i) == index;
            if (on)
                DrawRectangleGradientV(static_cast<int>(cell.x + 1), static_cast<int>(cell.y + 1), static_cast<int>(cell.width - 2),
                                       static_cast<int>(cell.height - 2), Color{120, 120, 124, 255}, Color{86, 86, 90, 255});
            else if (hovered(cell, ignoreBlock))
                DrawRectangleRec({cell.x + 1, cell.y + 1, cell.width - 2, cell.height - 2}, Color{232, 232, 236, 255});
            if (i > 0)
                DrawLineEx({cell.x, cell.y + 3}, {cell.x, cell.y + cell.height - 3}, 1.0f, Color{160, 160, 164, 255});
            textCentered(g_bold, items[i], cell, 18, on ? WHITE : kInk);
        }
    }

    // The mockup's sliders: thin track, red fill, round knob.
    void slider(Rectangle r, int &value, int minimum, int maximum, bool ignoreBlock = false)
    {
        static const void *s_dragging = nullptr;
        const float t = static_cast<float>(value - minimum) / static_cast<float>(std::max(1, maximum - minimum));
        const float cy = r.y + r.height * 0.5f;
        const Rectangle hit{r.x - 8, r.y - 8, r.width + 16, r.height + 16};
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hovered(hit, ignoreBlock))
            s_dragging = &value;
        if (s_dragging == &value)
        {
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                s_dragging = nullptr;
            else
            {
                const float pos = std::clamp((GetMousePosition().x - r.x) / r.width, 0.0f, 1.0f);
                value = minimum + static_cast<int>(std::lround(pos * (maximum - minimum)));
            }
        }
        DrawRectangleRounded({r.x, cy - 3, r.width, 6}, 1.0f, 4, Color{200, 200, 204, 255});
        DrawRectangleRounded({r.x, cy - 3, r.width * t, 6}, 1.0f, 4, kRed);
        DrawRectangleRoundedLines({r.x, cy - 3, r.width, 6}, 1.0f, 4, Color{130, 130, 134, 255});
        const Vector2 knob{r.x + r.width * t, cy};
        DrawCircleV(knob, 10, Color{90, 90, 94, 255});
        DrawCircleV(knob, 8.5f, Color{245, 245, 247, 255});
        DrawCircleV(knob, 3, Color{150, 150, 154, 255});
    }

    // Dropdowns draw their list last, on top of everything (see drawOpenDropdown).
    struct OpenDropdown
    {
        int *index = nullptr;
        Rectangle box{};
        std::vector<std::string> items;
    } g_dropdown;

    void dropdown(Rectangle r, const std::vector<std::string> &items, int &index, bool ignoreBlock = false)
    {
        const bool open = g_dropdown.index == &index;
        if (!open && clicked(r, ignoreBlock))
        {
            g_dropdown.index = &index;
            g_dropdown.box = r;
            g_dropdown.items = items;
        }
        DrawRectangleRec(r, WHITE);
        DrawRectangleLinesEx(r, 1.5f, kPanelEdge);
        const std::string &label = items[std::clamp(index, 0, static_cast<int>(items.size()) - 1)];
        text(g_bold, label, r.x + 10, r.y + (r.height - 19) * 0.5f - 1, 19, kInk);
        const float ax = r.x + r.width - 18, ay = r.y + r.height * 0.5f;
        DrawTriangle({ax - 6, ay - 3}, {ax, ay + 4}, {ax + 6, ay - 3}, kInk);
    }

    // Returns true while the list consumed the click this frame.
    bool drawOpenDropdown()
    {
        if (!g_dropdown.index)
            return false;
        const float itemH = 28.0f;
        const Rectangle list{g_dropdown.box.x, g_dropdown.box.y + g_dropdown.box.height,
                             g_dropdown.box.width, itemH * g_dropdown.items.size()};
        DrawRectangleRec({list.x + 3, list.y + 3, list.width, list.height}, Color{0, 0, 0, 70});
        DrawRectangleRec(list, WHITE);
        DrawRectangleLinesEx(list, 1.5f, kPanelEdge);
        for (size_t i = 0; i < g_dropdown.items.size(); ++i)
        {
            const Rectangle row{list.x, list.y + itemH * i, list.width, itemH};
            const bool hot = CheckCollisionPointRec(GetMousePosition(), row);
            if (hot)
                DrawRectangleRec(row, kRed);
            else if (static_cast<int>(i) == *g_dropdown.index)
                DrawRectangleRec(row, Color{232, 232, 236, 255});
            text(g_bold, g_dropdown.items[i], row.x + 10, row.y + 4, 18, hot ? WHITE : kInk);
            if (hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
            {
                *g_dropdown.index = static_cast<int>(i);
                g_dropdown.index = nullptr;
                return true;
            }
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(GetMousePosition(), list) &&
            !CheckCollisionPointRec(GetMousePosition(), g_dropdown.box))
            g_dropdown.index = nullptr;
        return true;
    }

    // The nine hand seals (臨兵闘者皆陣列在前) as faint decoration: launcher_assets/seals.png, a white 48 x 432 strip
    // (made from Yu Gothic, which raylib cannot load from its .ttc).
    Texture2D g_seals{};
    bool g_haveSeals = false;

    void seals(float x, float y, float step, Color color, int count = 9)
    {
        if (!g_haveSeals)
            return;
        for (int i = 0; i < count; ++i)
        {
            const Rectangle src{0, static_cast<float>((i % 9) * 48), 48, 48};
            DrawTexturePro(g_seals, src, {x, y + step * i, 26, 26}, {0, 0}, 0.0f, color);
        }
    }

    // ------------------------------------------------------------------ settings model

    struct Choice
    {
        const char *label;
        const char *value;
    };

    const std::vector<Choice> kResolutions = {{"Native 448p (1x)", "1"}, {"720p+ (2x)", "2"},  {"1080p+ (3x)", "3"},
                                              {"1440p+ (4x)", "4"},      {"4K (6x)", "6"},     {"5K+ (8x)", "8"}};
    const std::vector<Choice> kAspects = {{"16:9", "16:9"}, {"21:9", "21:9"}, {"32:9", "32:9"}, {"Auto", "auto"}};
    const std::vector<std::string> kTextureSteps = {"PS2", "2x", "4x", "8x", "16x", "16x + smooth"};

    struct Settings
    {
        int resolution = 1;  // index into kResolutions (2x)
        int api = 0;         // 0 Direct3D 11, 1 Vulkan
        bool widescreen = false;
        int aspect = 0;      // index into kAspects
        bool vsync = true;
        bool fullscreen = false;
        int fps = 1;         // 0 = 30, 1 = 60
        int texture = 0;     // 0..5, see kTextureSteps
        int sharpening = 0;  // 0..100 (AMD CAS)
        int antialias = 0;   // 0 off, 1 FXAA, 2 edge AA
        // Mod menu
        bool skipIntro = false;
        bool fastVu = false;
        bool showFps = false;
        int soundChip = 0;   // 0 PCSX2 SPU2, 1 legacy
        int volume = 100;
        bool diagnostics = false;
    };

    int indexOf(const std::vector<Choice> &list, const std::string &value, int fallback)
    {
        for (size_t i = 0; i < list.size(); ++i)
            if (value == list[i].value)
                return static_cast<int>(i);
        return fallback;
    }

    bool truthy(const std::string &v)
    {
        return v == "true" || v == "1" || v == "on" || v == "yes";
    }

    Settings readSettings(const Ini &game, const Ini &mine)
    {
        Settings s;
        s.resolution = indexOf(kResolutions, game.get("video", "render_scale", "2"), 1);
        s.api = game.get("video", "pcsx2_renderer", "d3d11") == "vulkan" ? 1 : 0;
        const std::string aspect = game.get("video", "aspect", "4:3");
        s.widescreen = indexOf(kAspects, aspect, -1) >= 0;
        s.aspect = std::max(0, indexOf(kAspects, aspect, 0));
        s.vsync = truthy(game.get("video", "vsync", "true"));
        const std::string window = game.get("video", "window", "windowed");
        s.fullscreen = window == "fullscreen" || window == "borderless";
        s.fps = game.get("video", "fps_target", "60") == "30" ? 0 : 1;
        const std::string aniso = game.get("video", "anisotropic", "off");
        const bool bilinear = game.get("video", "texture_filter", "ps2") == "bilinear";
        s.texture = aniso == "2" ? 1 : aniso == "4" ? 2 : aniso == "8" ? 3 : aniso == "16" ? (bilinear ? 5 : 4) : 0;
        s.sharpening = std::clamp(std::atoi(game.get("video", "sharpening", "0").c_str()), 0, 100);
        s.antialias = truthy(game.get("video", "edge_aa", "false")) ? 2 : truthy(game.get("video", "antialias", "false")) ? 1 : 0;
        s.skipIntro = truthy(game.get("game", "skip_intro", "false"));
        s.fastVu = game.get("performance", "fast_vu", "off") == "on";
        s.showFps = truthy(game.get("performance", "show_fps", "false"));
        s.soundChip = game.get("audio", "sound_chip", "pcsx2") == "legacy" ? 1 : 0;
        s.volume = std::clamp(std::atoi(game.get("audio", "volume", "100").c_str()), 0, 100);
        s.diagnostics = truthy(mine.get("launcher", "diagnostics", "false"));
        return s;
    }

    void writeSettings(const Settings &s, Ini &game, Ini &mine)
    {
        const auto b = [](bool v) { return std::string(v ? "true" : "false"); };
        game.set("video", "renderer", "pcsx2");
        game.set("video", "render_scale", kResolutions[s.resolution].value);
        game.set("video", "pcsx2_renderer", s.api == 1 ? "vulkan" : "d3d11");
        game.set("video", "aspect", s.widescreen ? kAspects[s.aspect].value : "4:3");
        game.set("video", "vsync", b(s.vsync));
        game.set("video", "window", s.fullscreen ? "borderless" : "windowed");
        game.set("video", "fps_target", s.fps == 0 ? "30" : "60");
        static const char *kAniso[] = {"off", "2", "4", "8", "16", "16"};
        game.set("video", "anisotropic", kAniso[std::clamp(s.texture, 0, 5)]);
        game.set("video", "texture_filter", s.texture == 5 ? "bilinear" : "ps2");
        game.set("video", "sharpening", std::to_string(s.sharpening));
        game.set("video", "antialias", b(s.antialias == 1));
        game.set("video", "edge_aa", b(s.antialias == 2));
        game.set("game", "skip_intro", b(s.skipIntro));
        game.set("performance", "fast_vu", s.fastVu ? "on" : "off");
        game.set("performance", "show_fps", b(s.showFps));
        game.set("audio", "sound_chip", s.soundChip == 1 ? "legacy" : "pcsx2");
        game.set("audio", "volume", std::to_string(s.volume));
        mine.set("launcher", "diagnostics", b(s.diagnostics));
    }

    // ------------------------------------------------------------------ modal windows

    enum class Modal
    {
        None,
        Controller,
        Mods,
        Credits,
        Guide1,
        Guide2,
        Message,
    };

    Rectangle modalFrame(const std::string &title, float w, float h)
    {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 150});
        const Rectangle r{(GetScreenWidth() - w) * 0.5f, (GetScreenHeight() - h) * 0.5f, w, h};
        DrawRectangleRounded({r.x - 3, r.y - 3, r.width + 6, r.height + 6}, 0.04f, 6, {20, 20, 22, 255});
        DrawRectangleRounded(r, 0.04f, 6, kPanel);
        DrawRectangleRec({r.x, r.y + 52, r.width, 3}, kRed);
        textCentered(g_heading, title, {r.x, r.y + 8, r.width, 40}, 30, kInk);
        return r;
    }
}

int main()
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1024, 640, "Shinobi (PS2 Recomp) Launcher - by TheRealAfsan");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);

    // Folders: launcher next to game/ and devilkings-fork/ (launcher.ini [paths] can point elsewhere).
    const fs::path root = fs::path(launcher::exeDirectory());
    Ini mine;
    mine.load((root / "launcher.ini").string());
    const fs::path gameDir = fs::path(mine.get("paths", "game", (root / "game").string()));
    const fs::path runner =
        fs::path(mine.get("paths", "runner", (root / "devilkings-fork/out/build/ps2xRuntime/ps2EntryRunner.exe").string()));
    const fs::path elf = gameDir / "SLUS_204.59";
    const fs::path settingsFile = gameDir / "settings.ini";
    const fs::path assets = root / "launcher_assets";

    Ini game;
    game.load(settingsFile.string());
    Settings settings = readSettings(game, mine);
    std::string status;
    double statusUntil = 0.0;

    const char *fontDir = "C:/Windows/Fonts/";
    g_heading = loadFont((std::string(fontDir) + "bahnschrift.ttf").c_str(), 64);
    g_bold = loadFont((std::string(fontDir) + "segoeuib.ttf").c_str(), 48);
    g_body = loadFont((std::string(fontDir) + "segoeuib.ttf").c_str(), 32);
    g_logo = loadFont((std::string(fontDir) + "impact.ttf").c_str(), 128);
    {
        std::vector<int> codepoints;
        for (const char *seal : kSeals)
        {
            int bytes = 0;
            codepoints.push_back(GetCodepoint(seal, &bytes));
        }
        (void)codepoints;
    }

    Images img;
    g_haveSeals = loadImage(assets / "seals.png", g_seals);
    img.hasKeyart = loadImage(assets / "keyart.png", img.keyart);
    img.hasLogo = loadImage(assets / "logo.png", img.logo);
    img.hasPlaystation = loadImage(assets / "playstation.png", img.playstation);
    img.hasSega = loadImage(assets / "sega.png", img.sega);
    img.hasTra = loadImage(assets / "tra.png", img.tra);
    img.hasGuide1 = loadImage(assets / "guide1.png", img.guide1);
    img.hasGuide2 = loadImage(assets / "guide2.png", img.guide2);
    // Placeholders from the player's own screenshots until real art is dropped in.
    Texture2D placeholder1{}, placeholder2{};
    const bool hasPlaceholder1 = loadImage(assets / "placeholder1.png", placeholder1);
    const bool hasPlaceholder2 = loadImage(assets / "placeholder2.png", placeholder2);
    {
        Image icon = LoadImage((assets / "icon.png").string().c_str());
        if (icon.data)
        {
            SetWindowIcon(icon);
            UnloadImage(icon);
        }
    }

    Modal modal = Modal::None;
    std::string message;

    auto save = [&]() -> bool
    {
        writeSettings(settings, game, mine);
        const bool ok = game.save(settingsFile.string()) && mine.save((root / "launcher.ini").string());
        status = ok ? "Settings saved" : "Could not save " + settingsFile.string();
        statusUntil = GetTime() + 2.5;
        return ok;
    };

    auto play = [&]()
    {
        if (!save())
            return;
        std::vector<std::pair<std::string, std::string>> environment;
        if (settings.diagnostics)
        {
            // The profile-session launcher's recording (scripts/session_report.py reads it).
            environment = {{"PS2X_PCSX2_LOG", "1"},
                           {"PS2X_STATS_LOG", "1"},
                           {"PS2X_VU1_STATS", "1"},
                           {"PS2X_PROFILE", "10:600:profile"},
                           {"PS2X_PROFILE_RAW", "captures/profile_raw.txt"},
                           {"PS2X_AUDIO_DUMP", "captures/audio_played.raw"},
                           {"PS2X_SPU2_TRACE", "1"}};
        }
        std::string error;
        if (!fs::exists(runner))
        {
            message = "The game executable was not found:\n" + runner.string() +
                      "\n\nSet [paths] runner = ... in launcher.ini next to this launcher.";
            modal = Modal::Message;
            return;
        }
        if (!launcher::launchGame(runner.string(), elf.string(), gameDir.string(),
                                  (gameDir / "captures" / "last_run.log").string(), environment, error))
        {
            message = error;
            modal = Modal::Message;
            return;
        }
        status = "Starting Shinobi...";
        statusUntil = GetTime() + 4.0;
        MinimizeWindow();
    };

    while (!WindowShouldClose())
    {
        g_inputBlocked = modal != Modal::None || g_dropdown.index != nullptr;
        BeginDrawing();
        ClearBackground(kFrame);
        DrawRectangle(0, 0, 1024, 3, kRed);

        // ---------------- key art (middle)
        const Rectangle art{270, 6, 450, 628};
        if (img.hasKeyart)
            drawCover(img.keyart, art);
        else
        {
            DrawRectangleGradientV(static_cast<int>(art.x), static_cast<int>(art.y), static_cast<int>(art.width),
                                   static_cast<int>(art.height), Color{180, 70, 110, 255}, Color{60, 14, 30, 255});
            if (hasPlaceholder1)
                drawCover(placeholder1, {art.x, art.y + 160, art.width, art.height - 160}, Color{255, 200, 220, 150});
            textCentered(g_bold, "Put your key art in launcher_assets/keyart.png", {art.x, art.y + art.height - 40, art.width, 30},
                         15, Color{255, 255, 255, 160});
        }
        if (img.hasLogo)
            drawContain(img.logo, {art.x + 40, art.y + 10, art.width - 80, 140});
        else
        {
            const std::string logo = "Shinobi";
            const float size = 104;
            const float w = textWidth(g_logo, logo, size, 2);
            const float x = art.x + (art.width - w) * 0.5f, y = art.y + 24;
            DrawLineEx({x - 20, y + 96}, {x + w + 30, y + 40}, 10, kRed);
            for (int dx = -4; dx <= 4; dx += 2)
                for (int dy = -4; dy <= 4; dy += 2)
                    text(g_logo, logo, x + dx, y + dy, size, Color{60, 8, 10, 255}, 2);
            text(g_logo, logo, x, y, size, Color{255, 204, 40, 255}, 2);
            text(g_bold, "TM", x + w + 4, y + 10, 14, Color{255, 220, 120, 255});
        }

        // ---------------- left column
        const Rectangle left{12, 10, 304, 620};
        DrawRectangleRounded(left, 0.03f, 6, Color{58, 58, 62, 235});
        DrawRectangleRoundedLines(left, 0.03f, 6, Color{110, 110, 116, 255});
        seals(16, 300, 36, Color{255, 255, 255, 28});
        text(g_heading, "GUIDES", 28, 20, 24, WHITE);
        const Rectangle g1{28, 48, 132, 84}, g2{170, 58, 132, 84};
        auto thumb = [&](Rectangle r, bool has, const Texture2D &t, bool hasPh, const Texture2D &ph, const char *caption)
        {
            DrawRectangleRec({r.x - 2, r.y - 2, r.width + 4, r.height + 4}, hovered(r) ? kRed : Color{20, 20, 22, 255});
            if (has)
                drawCover(t, r);
            else if (hasPh)
                drawCover(ph, r);
            else
                DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width),
                                       static_cast<int>(r.height), Color{90, 40, 70, 255}, Color{30, 20, 30, 255});
            DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y + r.height - 20), static_cast<int>(r.width), 20,
                          Color{0, 0, 0, 150});
            text(g_body, caption, r.x + 6, r.y + r.height - 19, 15, WHITE);
        };
        thumb(g1, img.hasGuide1, img.guide1, hasPlaceholder1, placeholder1, "Best settings");
        thumb(g2, img.hasGuide2, img.guide2, hasPlaceholder2, placeholder2, "Reporting problems");
        if (clicked(g1))
            modal = Modal::Guide1;
        if (clicked(g2))
            modal = Modal::Guide2;

        const Rectangle about{28, 158, 274, 128};
        DrawRectangleRounded(about, 0.05f, 6, kDarkBox);
        DrawRectangleRoundedLines(about, 0.05f, 6, Color{120, 120, 126, 255});
        text(g_heading, "SHINOBI - PS2 RECOMPILATION", about.x + 10, about.y + 9, 17.5f, WHITE);
        wrapped(g_body,
                "Ignite the fiery vengeance of Hotsuma, last of the Oboro clan. Scale the walls of a demon-filled Tokyo "
                "and beware the deadly curse of your own blade.",
                {about.x + 10, about.y + 36, about.width - 18, about.height - 40}, 15, Color{215, 215, 220, 255});

        if (button({28, 304, 274, 44}, "PLAY GAME (RECOMP)"))
            play();
        if (button({28, 358, 274, 44}, "CONFIGURE CONTROLLER"))
            modal = Modal::Controller;
        if (button({28, 412, 274, 44}, "MOD MENU"))
            modal = Modal::Mods;
        if (button({28, 466, 274, 44}, "CREDITS"))
            modal = Modal::Credits;

        // Logos
        const Rectangle ps{28, 540, 64, 64}, sega{104, 548, 118, 48}, tra{232, 528, 72, 80};
        if (img.hasPlaystation)
            drawContain(img.playstation, ps);
        else
            textCentered(g_heading, "PS2", ps, 26, Color{90, 140, 230, 255});
        if (img.hasSega)
            drawContain(img.sega, sega);
        else
            textCentered(g_logo, "SEGA", sega, 40, Color{30, 90, 210, 255});
        if (img.hasTra)
            drawContain(img.tra, tra);
        else
        {
            text(g_body, "TheRealAfsan", tra.x - 6, tra.y + 4, 12, WHITE);
            textCentered(g_logo, "TRA", {tra.x, tra.y + 18, tra.width, 56}, 46, kRed);
        }

        // ---------------- right column: GRAPHICAL SETTINGS
        const Rectangle right{700, 10, 312, 620};
        DrawRectangleRounded({right.x - 3, right.y - 3, right.width + 6, right.height + 6}, 0.03f, 6, {20, 20, 22, 255});
        DrawRectangleRounded(right, 0.03f, 6, kPanel);
        seals(right.x + right.width - 30, 70, 36, Color{0, 0, 0, 36});
        const Rectangle header{right.x + 10, right.y + 10, right.width - 20, 44};
        DrawRectangleGradientV(static_cast<int>(header.x), static_cast<int>(header.y), static_cast<int>(header.width),
                               static_cast<int>(header.height), Color{250, 250, 252, 255}, Color{206, 206, 210, 255});
        DrawRectangleLinesEx(header, 1.5f, kPanelEdge);
        textCentered(g_heading, "GRAPHICAL SETTINGS", header, 27, kInk);

        const float x0 = right.x + 16, w0 = right.width - 52;
        float y = 74;
        text(g_bold, "RESOLUTION", x0, y, 18, kInk);
        std::vector<std::string> resLabels;
        for (const Choice &c : kResolutions)
            resLabels.push_back(c.label);
        const Rectangle resBox{x0, y + 24, w0, 30};
        y += 62;
        text(g_bold, "RENDERER", x0, y, 18, kInk);
        segmented({x0, y + 24, w0, 30}, {"DIRECT3D 11", "VULKAN"}, settings.api);
        y += 66;
        checkbox({x0, y, 24, 24}, "WIDESCREEN HACK", settings.widescreen);
        std::vector<std::string> aspectLabels;
        for (const Choice &c : kAspects)
            aspectLabels.push_back(c.label);
        const Rectangle aspectBox{x0 + w0 - 76, y - 2, 76, 28};
        y += 36;
        checkbox({x0, y, 24, 24}, "V-SYNC", settings.vsync);
        checkbox({x0 + 130, y, 24, 24}, "FULLSCREEN", settings.fullscreen);
        y += 36;
        text(g_bold, "FRAME RATE", x0, y + 4, 18, kInk);
        segmented({x0 + 120, y, w0 - 120, 28}, {"30 FPS", "60 FPS"}, settings.fps);
        y += 42;
        text(g_bold, "TEXTURE QUALITY", x0, y, 18, kInk);
        const std::string texLabel = kTextureSteps[std::clamp(settings.texture, 0, 5)];
        text(g_body, texLabel, x0 + w0 - textWidth(g_body, texLabel, 15), y + 3, 15, kSoftInk);
        slider({x0, y + 30, w0, 12}, settings.texture, 0, 5);
        y += 56;
        text(g_bold, "EFFECTS QUALITY", x0, y, 18, kInk);
        const std::string fxLabel = settings.sharpening == 0 ? "sharpening off" : "sharpening " + std::to_string(settings.sharpening);
        text(g_body, fxLabel, x0 + w0 - textWidth(g_body, fxLabel, 15), y + 3, 15, kSoftInk);
        slider({x0, y + 30, w0, 12}, settings.sharpening, 0, 100);
        y += 56;
        text(g_bold, "ANTI-ALIASING", x0, y, 18, kInk);
        segmented({x0, y + 24, w0, 30}, {"OFF", "FXAA", "EDGE AA"}, settings.antialias);
        y += 64;
        DrawLineEx({right.x + 10, y}, {right.x + right.width - 10, y}, 1.5f, Color{170, 170, 174, 255});
        if (button({x0, y + 10, w0 + 18, 42}, "SAVE SETTINGS"))
            save();
        if (button({x0, y + 60, w0 + 18, 42}, "RESET TO DEFAULTS"))
        {
            const bool diagnostics = settings.diagnostics;
            settings = Settings{};
            settings.diagnostics = diagnostics;
            status = "Defaults restored (press Save Settings to keep them)";
            statusUntil = GetTime() + 3.0;
        }

        // Dropdown boxes after the rows below them, so their lists are drawn over the rest.
        dropdown(resBox, resLabels, settings.resolution);
        if (settings.widescreen)
            dropdown(aspectBox, aspectLabels, settings.aspect);

        if (!status.empty() && GetTime() < statusUntil)
        {
            const float w = textWidth(g_bold, status, 17) + 24;
            DrawRectangleRounded({(1024 - w) * 0.5f, 600, w, 28}, 0.4f, 6, Color{0, 0, 0, 190});
            textCentered(g_bold, status, {(1024 - w) * 0.5f, 600, w, 28}, 17, WHITE);
        }

        // ---------------- modal windows
        if (modal != Modal::None)
        {
            if (modal == Modal::Controller)
            {
                const Rectangle r = modalFrame("CONFIGURE CONTROLLER", 620, 470);
                float yy = r.y + 70;
                text(g_bold, "Connected controllers", r.x + 24, yy, 19, kInk);
                yy += 28;
                bool any = false;
                for (int pad = 0; pad < 4; ++pad)
                    if (IsGamepadAvailable(pad))
                    {
                        text(g_body, std::string("Slot ") + std::to_string(pad) + ": " + GetGamepadName(pad), r.x + 34, yy, 16, kSoftInk);
                        yy += 22;
                        any = true;
                    }
                if (!any)
                {
                    text(g_body, "None found (plug one in; the game also takes the keyboard).", r.x + 34, yy, 16, kSoftInk);
                    yy += 22;
                }
                yy += 10;
                text(g_bold, "Keyboard keys (settings.ini [controls])", r.x + 24, yy, 19, kInk);
                yy += 28;
                static const char *kButtons[][2] = {{"up", "UP,W"},       {"down", "DOWN,S"},     {"left", "LEFT,A"},
                                                    {"right", "RIGHT,D"}, {"cross", "X,SPACE"},   {"circle", "C,ESCAPE"},
                                                    {"square", "Z,KP_0"}, {"triangle", "V,KP_1"}, {"l1", "Q"},
                                                    {"r1", "E"},          {"l2", "LEFT_SHIFT"},   {"r2", "RIGHT_SHIFT"},
                                                    {"start", "ENTER"},   {"select", "TAB"}};
                for (int i = 0; i < 14; ++i)
                {
                    const float cx = r.x + 34 + (i % 2) * 290, cy = yy + (i / 2) * 22;
                    const std::string keys = game.get("controls", kButtons[i][0], kButtons[i][1]);
                    text(g_body, std::string(kButtons[i][0]) + ": " + (keys.empty() ? "-" : keys), cx, cy, 16, kSoftInk);
                }
                text(g_body, "A controller works without setup. F8 or L3+R3 in game marks a problem for the log.", r.x + 24,
                     r.y + r.height - 98, 15, kSoftInk);
                if (button({r.x + 24, r.y + r.height - 64, 270, 42}, "EDIT KEYBOARD KEYS", true, 19))
                {
                    save();
                    launcher::openWithShell(settingsFile.string());
                }
                if (button({r.x + r.width - 194, r.y + r.height - 64, 170, 42}, "CLOSE", true, 19))
                    modal = Modal::None;
            }
            else if (modal == Modal::Mods)
            {
                const Rectangle r = modalFrame("MOD MENU", 560, 440);
                float yy = r.y + 74;
                checkbox({r.x + 30, yy, 24, 24}, "Skip the intro logos", settings.skipIntro, true);
                yy += 38;
                checkbox({r.x + 30, yy, 24, 24}, "Fast VU maths (a little faster, less exact)", settings.fastVu, true);
                yy += 38;
                checkbox({r.x + 30, yy, 24, 24}, "Show FPS counter", settings.showFps, true);
                yy += 38;
                checkbox({r.x + 30, yy, 24, 24}, "Record diagnostics (profile + sound trace)", settings.diagnostics, true);
                yy += 46;
                text(g_bold, "SOUND CHIP", r.x + 30, yy + 4, 18, kInk);
                segmented({r.x + 170, yy, 350, 30}, {"PCSX2 SPU2", "LEGACY"}, settings.soundChip, true);
                yy += 48;
                text(g_bold, "VOLUME", r.x + 30, yy, 18, kInk);
                text(g_body, std::to_string(settings.volume) + "%", r.x + r.width - 80, yy + 2, 16, kSoftInk);
                slider({r.x + 170, yy + 8, 300, 12}, settings.volume, 0, 100, true);
                if (button({r.x + r.width - 194, r.y + r.height - 64, 170, 42}, "CLOSE", true, 19))
                    modal = Modal::None;
            }
            else if (modal == Modal::Credits)
            {
                const Rectangle r = modalFrame("CREDITS", 600, 430);
                wrapped(g_body,
                        "Shinobi (2002) by Overworks / SEGA. This is an unofficial fan recompilation for personal use; play it "
                        "with your own copy of the game.\n\n"
                        "PS2 recompilation and port: TheRealAfsan.\n"
                        "Built on PS2Recomp (ps2xRecomp / ps2xRuntime) and its contributors.\n"
                        "Graphics: PCSX2's GS through the pcsx2-gs-bridge (GPL-3.0+).\n"
                        "Sound: PCSX2's SPU2 core (GPL-3.0+), PCSX2 Dev Team.\n"
                        "Window, input and this launcher: raylib.",
                        {r.x + 28, r.y + 72, r.width - 56, r.height - 140}, 17, kInk, 1.3f);
                if (button({r.x + r.width - 194, r.y + r.height - 64, 170, 42}, "CLOSE", true, 19))
                    modal = Modal::None;
            }
            else if (modal == Modal::Guide1)
            {
                const Rectangle r = modalFrame("BEST SETTINGS", 640, 460);
                wrapped(g_body,
                        "Resolution: how many times the PS2's resolution the GPU draws at. A modern GPU runs this game at "
                        "4x (about 1800 lines) or 6x without effort; it costs only GPU time.\n\n"
                        "Widescreen hack: widens the camera to 16:9, 21:9, 32:9 or your monitor's exact shape (Auto). The HUD "
                        "and menus are stretched with it, as with PCSX2's widescreen patch.\n\n"
                        "Frame rate: 60 runs as fast as it can. 30 locks to an even 30, which can feel smoother when the game "
                        "cannot hold 60 in heavy scenes.\n\n"
                        "Texture quality: anisotropic filtering, sharper floors and walls. Effects quality: AMD CAS sharpening "
                        "of the finished picture (try 30-50).",
                        {r.x + 28, r.y + 72, r.width - 56, r.height - 140}, 16, kInk, 1.25f);
                if (button({r.x + r.width - 194, r.y + r.height - 64, 170, 42}, "CLOSE", true, 19))
                    modal = Modal::None;
            }
            else if (modal == Modal::Guide2)
            {
                const Rectangle r = modalFrame("REPORTING PROBLEMS", 640, 420);
                wrapped(g_body,
                        "Something looks or sounds wrong? Press F8 on the keyboard, or click both thumbsticks (L3 + R3), the "
                        "moment it happens. The game writes a marked line with the time, the frame rate and the sound "
                        "position to game/captures/last_run.log, and saves a screenshot next to it.\n\n"
                        "For a full recording (performance profile and sound trace), tick \"Record diagnostics\" in the "
                        "Mod Menu before pressing Play.",
                        {r.x + 28, r.y + 72, r.width - 56, r.height - 140}, 16, kInk, 1.25f);
                if (button({r.x + r.width - 194, r.y + r.height - 64, 170, 42}, "CLOSE", true, 19))
                    modal = Modal::None;
            }
            else if (modal == Modal::Message)
            {
                const Rectangle r = modalFrame("SHINOBI LAUNCHER", 600, 300);
                wrapped(g_body, message, {r.x + 28, r.y + 72, r.width - 56, r.height - 140}, 16, kInk, 1.25f);
                if (button({r.x + r.width - 194, r.y + r.height - 64, 170, 42}, "OK", true, 19))
                    modal = Modal::None;
            }
            if (IsKeyPressed(KEY_ESCAPE))
                modal = Modal::None;
        }

        drawOpenDropdown();
        EndDrawing();

        // Test hook: SHINOBI_LAUNCHER_SHOT=<png> saves the window after a few frames and quits.
        static const char *s_shot = std::getenv("SHINOBI_LAUNCHER_SHOT");
        static int s_frames = 0;
        if (s_shot && ++s_frames == 20)
        {
            Image screen = LoadImageFromScreen();
            ExportImage(screen, s_shot);
            UnloadImage(screen);
            break;
        }
    }
    CloseWindow();
    return 0;
}
