// See launcher_settings_map.h.
#include "launcher_settings_map.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace launcher
{
    namespace
    {
        struct Bind
        {
            const char *key;
            const char *fallback; // the game's default (ps2_settings.cpp)
        };
        const Bind kBinds[] = {{"up", "UP,W"},       {"down", "DOWN,S"},     {"left", "LEFT,A"},   {"right", "RIGHT,D"},
                               {"cross", "X,SPACE"}, {"circle", "C,ESCAPE"}, {"square", "Z,KP_0"}, {"triangle", "V,KP_1"},
                               {"l1", "Q"},          {"r1", "E"},            {"l2", "LEFT_SHIFT"}, {"r2", "RIGHT_SHIFT"},
                               {"start", "ENTER"},   {"select", "TAB"},
                               {"camera_up", "I"},   {"camera_down", "K"},   {"camera_left", "J"}, {"camera_right", "L"}};
    }

    bool truthy(const std::string &v)
    {
        return v == "true" || v == "1" || v == "on" || v == "yes";
    }

    std::string jsonString(const std::string &v)
    {
        std::string out = "\"";
        for (char c : v)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            if (static_cast<unsigned char>(c) >= 0x20)
                out += c;
        }
        return out + "\"";
    }

    std::string pageValues(const SettingGet &get, bool diagnostics)
    {
        const std::string scale = get("video", "render_scale", "2");
        const std::string res = scale == "1"   ? "Native"
                                : scale == "3" ? "1080p"
                                : scale == "4" ? "1440p"
                                : (scale == "6" || scale == "8") ? "4K"
                                                                 : "720p";
        const std::string aspect = get("video", "aspect", "4:3");
        const bool ws = aspect == "16:9" || aspect == "21:9" || aspect == "32:9" || aspect == "auto";
        const std::string shape = aspect == "auto" ? "Auto" : ws ? aspect : "16:9";
        const std::string aniso = get("video", "anisotropic", "off");
        const int tex = aniso == "2" ? 1 : aniso == "4" ? 2 : aniso == "8" ? 3 : aniso == "16" ? 4 : 0;
        const int sharp = std::atoi(get("video", "sharpening", "0").c_str());
        const int fx = sharp <= 0 ? 0 : sharp <= 25 ? 1 : sharp <= 50 ? 2 : sharp <= 75 ? 3 : 4;
        const std::string aa = truthy(get("video", "edge_aa", "false"))     ? "Edge AA"
                               : truthy(get("video", "antialias", "false")) ? "FXAA"
                                                                            : "Off";
        const std::string fps = get("video", "fps_target", "60");
        const auto b = [](bool x) { return x ? "true" : "false"; };
        std::ostringstream js;
        js << "settings:{res:" << jsonString(res)
           << ",renderer:" << jsonString(get("video", "pcsx2_renderer", "d3d11") == "vulkan" ? "Vulkan" : "D3D11")
           << ",fps:" << jsonString(fps == "unlocked" ? "Unlocked" : (fps == "30" || fps == "50") ? fps : "60")
           << ",ws:" << b(ws) << ",wsShape:" << jsonString(shape) << ",vsync:" << b(truthy(get("video", "vsync", "true")))
           << ",tex:" << tex << ",fx:" << fx << ",aa:" << jsonString(aa) << "},binds:{";
        bool first = true;
        for (const Bind &bind : kBinds)
        {
            js << (first ? "" : ",") << bind.key << ":" << jsonString(get("controls", bind.key, bind.fallback));
            first = false;
        }
        const std::string window = get("video", "window", "windowed");
        js << "},mods:{intro:" << b(truthy(get("game", "skip_intro", "false")))
           << ",studio:" << b(truthy(get("game", "studio_intro", "true"))) << ",full:" << b(window != "windowed")
           << ",fastvu:" << b(get("performance", "fast_vu", "off") == "on")
           << ",dbg:" << b(truthy(get("performance", "show_fps", "false"))) << ",diag:" << b(diagnostics)
           << ",legacy:" << b(get("audio", "sound_chip", "pcsx2") == "legacy")
           << ",nscale:" << b(get("effects", "native_scaling", "normal") != "off")
           << ",rr5fix:" << b(truthy(get("effects", "rr5_dof_fix", "false")))
           << ",dropst:" << b(get("effects", "large_st", "keep") == "drop")
           << ",record:" << b(truthy(get("effects", "record_drawing", "false"))) << "}";
        return js.str();
    }

    bool applyPageValues(const std::map<std::string, std::string> &v, const SettingGet &read, const SettingSet &set)
    {
        auto get = [&](const char *k, const char *fallback) {
            const auto it = v.find(k);
            return it == v.end() ? std::string(fallback) : it->second;
        };
        const auto b = [](bool x) { return std::string(x ? "true" : "false"); };
        const std::string res = get("res", "720p");
        set("video", "renderer", "pcsx2");
        set("video", "render_scale", res == "Native" ? "1" : res == "1080p" ? "3" : res == "1440p" ? "4" : res == "4K" ? "6" : "2");
        set("video", "pcsx2_renderer", get("renderer", "D3D11") == "Vulkan" ? "vulkan" : "d3d11");
        const std::string shape = get("wsShape", "16:9");
        set("video", "aspect", get("ws", "0") == "1" ? (shape == "Auto" ? std::string("auto") : shape) : std::string("4:3"));
        set("video", "vsync", b(get("vsync", "1") == "1"));
        static const char *kAniso[] = {"off", "2", "4", "8", "16"};
        set("video", "anisotropic", kAniso[std::clamp(std::atoi(get("tex", "0").c_str()), 0, 4)]);
        static const char *kSharp[] = {"0", "25", "50", "75", "100"};
        set("video", "sharpening", kSharp[std::clamp(std::atoi(get("fx", "0").c_str()), 0, 4)]);
        const std::string aa = get("aa", "Off");
        set("video", "antialias", b(aa == "FXAA"));
        set("video", "edge_aa", b(aa == "Edge AA"));
        set("game", "skip_intro", b(get("mod.intro", "0") == "1"));
        set("game", "studio_intro", b(get("mod.studio", "1") == "1"));
        const std::string fps = get("fps", "60");
        set("video", "fps_target", fps == "Unlocked" ? std::string("unlocked") : (fps == "30" || fps == "50") ? fps : std::string("60"));
        // Fullscreen on: keep exclusive "fullscreen" if that is what the file has, else borderless.
        const std::string window = read("video", "window", "windowed");
        set("video", "window", get("mod.full", "0") == "1" ? (window == "fullscreen" ? window : std::string("borderless"))
                                                           : std::string("windowed"));
        set("performance", "fast_vu", get("mod.fastvu", "0") == "1" ? "on" : "off");
        set("performance", "show_fps", b(get("mod.dbg", "0") == "1"));
        set("audio", "sound_chip", get("mod.legacy", "0") == "1" ? "legacy" : "pcsx2");
        // Effects (Mod Menu): see the [effects] settings in ps2_settings.cpp. Native scaling on keeps "aggressive" if set.
        const std::string scaling = read("effects", "native_scaling", "normal");
        set("effects", "native_scaling",
            get("mod.nscale", "1") == "1" ? (scaling == "aggressive" ? scaling : std::string("normal")) : std::string("off"));
        set("effects", "rr5_dof_fix", b(get("mod.rr5fix", "0") == "1"));
        set("effects", "large_st", get("mod.dropst", "0") == "1" ? "drop" : "keep");
        set("effects", "record_drawing", b(get("mod.record", "0") == "1"));
        for (const Bind &bind : kBinds)
        {
            const auto it = v.find(std::string("bind.") + bind.key);
            if (it != v.end())
                set("controls", bind.key, it->second == "None" ? std::string() : it->second);
        }
        return get("mod.diag", "0") == "1";
    }
}
