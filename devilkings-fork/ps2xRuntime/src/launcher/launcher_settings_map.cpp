// See launcher_settings_map.h.
#include <cctype>
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

    // "#rrggbb" in lower case, or "" for the original colour (anything else).
    static std::string scarfColour(std::string value)
    {
        for (char &c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (value.size() != 7u || value[0] != '#')
            return std::string();
        for (size_t i = 1; i < 7u; ++i)
            if (!std::isxdigit(static_cast<unsigned char>(value[i])))
                return std::string();
        return value;
    }

    std::string pageValues(const SettingGet &get, bool diagnostics)
    {
        const std::string scale = get("video", "render_scale", "3");
        const std::string res = scale == "1"   ? "Native"
                                : scale == "3" ? "1080p"
                                : scale == "4" ? "1440p"
                                : scale == "12"                  ? "8K"
                                : (scale == "6" || scale == "8") ? "4K"
                                                                 : "720p";
        const std::string aspect = get("video", "aspect", "16:9");
        const bool ws = aspect == "16:9" || aspect == "21:9" || aspect == "32:9" || aspect == "auto";
        const std::string shape = aspect == "auto" ? "Auto" : ws ? aspect : "16:9";
        const std::string aniso = get("video", "anisotropic", "2");
        const int tex = aniso == "2" ? 1 : aniso == "4" ? 2 : aniso == "8" ? 3 : aniso == "16" ? 4 : 0;
        const int sharp = std::atoi(get("video", "sharpening", "20").c_str());
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
           << ",tex:" << tex << ",fx:" << fx << ",aa:" << jsonString(aa)
           << ",blend:" << jsonString(get("video", "blending", "basic"))
           << ",texf:" << jsonString(get("video", "texture_filter", "ps2") == "bilinear" ? "bilinear" : "ps2")
           << ",scrf:" << jsonString(get("video", "filter", "smooth"))
           << ",dith:" << b(get("video", "dithering", "on") != "off") << ",mip:" << b(get("video", "mipmapping", "on") != "off")
           << ",upfix:" << b(get("video", "upscale_fixes", "on") != "off")
           << ",ovl:" << b(truthy(get("performance", "stats_overlay", "false")))
           << ",cam:" << b(truthy(get("game", "close_camera", "false")))
           << ",scarf:" << jsonString(scarfColour(get("game", "scarf_color", "original")))
           << ",win:" << jsonString(get("video", "window", "borderless"))
           << ",haptics:" << b(truthy(get("input", "haptics", "true")))
           << ",hapStr:" << std::clamp((std::atoi(get("input", "haptic_strength", "100").c_str()) + 5) / 10, 1, 10)
           << ",trig:" << b(truthy(get("input", "trigger_resistance", "true"))) << "},binds:{";
        bool first = true;
        for (const Bind &bind : kBinds)
        {
            js << (first ? "" : ",") << bind.key << ":" << jsonString(get("controls", bind.key, bind.fallback));
            first = false;
        }
        const std::string window = get("video", "window", "borderless");
        js << "},mods:{intro:" << b(truthy(get("game", "skip_intro", "false")))
           << ",studio:" << b(truthy(get("game", "studio_intro", "true"))) << ",full:" << b(window != "windowed")
           << ",fastvu:" << b(get("performance", "fast_vu", "off") == "on")
           << ",dbg:" << b(truthy(get("performance", "show_fps", "false"))) << ",diag:" << b(diagnostics)
           << ",legacy:" << b(get("audio", "sound_chip", "pcsx2") == "legacy")
           << ",k8:" << b(truthy(get("video", "allow_8k", "false")) || get("video", "render_scale", "3") == "12")
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
        set("video", "render_scale", res == "Native" ? "1" : res == "1080p" ? "3" : res == "1440p" ? "4" : res == "4K" ? "6" : res == "8K" ? "12" : "2");
        // LOCAL PATCH: the Mod Menu switch that offers 8K in the Resolution list.
        set("video", "allow_8k", b(get("mod.k8", "0") == "1"));
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
        // LOCAL PATCH: PCSX2 blending accuracy, Minimum from the Low-end PC preset (no control of its own on the page).
        {
            // LOCAL PATCH: the Graphics and Effects tabs.
            const std::string blend = get("blend", "basic");
            set("video", "blending",
                (blend == "minimum" || blend == "medium" || blend == "high") ? blend : std::string("basic"));
            set("video", "texture_filter", get("texf", "ps2") == "bilinear" ? "bilinear" : "ps2");
            const std::string scrf = get("scrf", "smooth");
            set("video", "filter",
                (scrf == "sharp" || scrf == "scanlines" || scrf == "crt") ? scrf : std::string("smooth"));
            set("video", "dithering", get("dith", "1") == "1" ? "on" : "off");
            set("video", "mipmapping", get("mip", "1") == "1" ? "on" : "off");
            set("video", "upscale_fixes", get("upfix", "1") == "1" ? "on" : "off");
            set("performance", "stats_overlay", b(get("ovl", "0") == "1"));
            set("game", "close_camera", b(get("cam", "0") == "1"));
        }
        // LOCAL PATCH (mod): Hotsuma's scarf colour, "original" or #rrggbb.
        {
            const std::string scarf = scarfColour(get("scarf", "original"));
            set("game", "scarf_color", scarf.empty() ? std::string("original") : scarf);
        }
        set("game", "skip_intro", b(get("mod.intro", "0") == "1"));
        set("game", "studio_intro", b(get("mod.studio", "1") == "1"));
        const std::string fps = get("fps", "60");
        set("video", "fps_target", fps == "Unlocked" ? std::string("unlocked") : (fps == "30" || fps == "50") ? fps : std::string("60"));
        // Fullscreen on: keep exclusive "fullscreen" if that is what the file has, else borderless.
        // LOCAL PATCH: the Window Mode menu (windowed / borderless / fullscreen). Older pages sent the Mod Menu's
        // Fullscreen switch instead.
        const std::string win = get("win", "");
        if (win == "windowed" || win == "borderless" || win == "fullscreen")
            set("video", "window", win);
        else
        {
            const std::string window = read("video", "window", "borderless");
            set("video", "window", get("mod.full", "1") == "1" ? (window == "fullscreen" ? window : std::string("borderless"))
                                                               : std::string("windowed"));
        }
        // LOCAL PATCH: DualSense haptics (the Controller tab). Strength 1..10 on the page, percent in the file.
        set("input", "haptics", b(get("haptics", "1") == "1"));
        set("input", "haptic_strength", std::to_string(std::clamp(std::atoi(get("hapStr", "10").c_str()), 1, 10) * 10));
        set("input", "trigger_resistance", b(get("trig", "1") == "1"));
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
