#include "runtime/ps2_settings.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

namespace ps2x::settings
{
    namespace
    {
        std::string trim(const std::string &text)
        {
            size_t begin = 0;
            size_t end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return text.substr(begin, end - begin);
        }

        std::string lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        void setEnvironment(const char *name, const std::string &value)
        {
#ifdef _WIN32
            _putenv_s(name, value.c_str());
#else
            setenv(name, value.c_str(), 1);
#endif
        }

        std::string keyOf(const char *section, const char *key)
        {
            return std::string(section) + "." + key;
        }

        std::atomic<uint64_t> g_version{1};

        struct KeyName
        {
            int code;
            const char *name;
        };

        // Keys a binding may use (F1 and F2 open the menu and the debug panel).
        const std::vector<KeyName> &keyNames()
        {
            static const std::vector<KeyName> s_names = []()
            {
                std::vector<KeyName> names = {
                    {KEY_UP, "UP"}, {KEY_DOWN, "DOWN"}, {KEY_LEFT, "LEFT"}, {KEY_RIGHT, "RIGHT"},
                    {KEY_SPACE, "SPACE"}, {KEY_ENTER, "ENTER"}, {KEY_TAB, "TAB"}, {KEY_ESCAPE, "ESCAPE"},
                    {KEY_BACKSPACE, "BACKSPACE"}, {KEY_INSERT, "INSERT"}, {KEY_DELETE, "DELETE"},
                    {KEY_HOME, "HOME"}, {KEY_END, "END"}, {KEY_PAGE_UP, "PAGE_UP"}, {KEY_PAGE_DOWN, "PAGE_DOWN"},
                    {KEY_LEFT_SHIFT, "LEFT_SHIFT"}, {KEY_RIGHT_SHIFT, "RIGHT_SHIFT"},
                    {KEY_LEFT_CONTROL, "LEFT_CONTROL"}, {KEY_RIGHT_CONTROL, "RIGHT_CONTROL"},
                    {KEY_LEFT_ALT, "LEFT_ALT"}, {KEY_RIGHT_ALT, "RIGHT_ALT"},
                    {KEY_COMMA, "COMMA"}, {KEY_PERIOD, "PERIOD"}, {KEY_SLASH, "SLASH"}, {KEY_SEMICOLON, "SEMICOLON"},
                    {KEY_MINUS, "MINUS"}, {KEY_EQUAL, "EQUAL"}, {KEY_APOSTROPHE, "APOSTROPHE"}, {KEY_GRAVE, "GRAVE"},
                    {KEY_LEFT_BRACKET, "LEFT_BRACKET"}, {KEY_RIGHT_BRACKET, "RIGHT_BRACKET"},
                    {KEY_BACKSLASH, "BACKSLASH"}, {KEY_KP_ENTER, "KP_ENTER"},
                };
                static const char *const kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                                       "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
                for (int index = 0; index < 26; ++index)
                    names.push_back({KEY_A + index, kLetters[index]});
                static const char *const kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
                static const char *const kPad[] = {"KP_0", "KP_1", "KP_2", "KP_3", "KP_4",
                                                   "KP_5", "KP_6", "KP_7", "KP_8", "KP_9"};
                for (int index = 0; index < 10; ++index)
                {
                    names.push_back({KEY_ZERO + index, kDigits[index]});
                    names.push_back({KEY_KP_0 + index, kPad[index]});
                }
                static const char *const kFunctions[] = {"F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
                for (int index = 0; index < 10; ++index)
                    names.push_back({KEY_F3 + index, kFunctions[index]});
                return names;
            }();
            return s_names;
        }

        struct State
        {
            std::mutex mutex;
            bool loaded = false;
            bool disabled = false;
            std::filesystem::path file;
            std::filesystem::path elfDirectory;
            std::map<std::string, std::string> values;    // section.key -> value
            std::map<std::string, std::string> atLoad;    // values the runtime started with
            std::map<std::string, bool> fromEnvironment;  // section.key -> the environment set it
            // section.key -> the file's own value (or the default) of a setting the environment
            // overrides: save() writes it back, so a variable set for one run does not stick.
            std::map<std::string, std::string> fileValue;
            std::vector<CheatGroup> cheats;
            std::shared_ptr<const std::vector<FrameWrite>> frameWrites =
                std::make_shared<const std::vector<FrameWrite>>();
        };

        State &state()
        {
            static State s_state;
            return s_state;
        }

        const Definition *find(const char *section, const char *key)
        {
            for (const Definition &definition : definitions())
            {
                if (std::string(definition.section) == section && std::string(definition.key) == key)
                    return &definition;
            }
            return nullptr;
        }

        // The PS2X_* value for a setting, or empty for "leave the runtime default".
        std::string environmentValue(const Definition &definition, const std::string &value)
        {
            if (value == "auto")
                return std::string();
            if (definition.kind == Kind::Choice && !definition.choiceEnvironment.empty())
            {
                for (size_t index = 0; index < definition.choices.size(); ++index)
                {
                    if (definition.choices[index] == value && index < definition.choiceEnvironment.size())
                        return definition.choiceEnvironment[index];
                }
                return std::string();
            }
            if (definition.kind == Kind::Bool)
                return value == "true" ? "1" : "0";
            if (definition.kind == Kind::Number)
                return value == "0" && definition.autoLabel ? std::string() : value;
            return value;
        }

        std::string fromEnvironmentValue(const Definition &definition, const char *value)
        {
            if (definition.kind == Kind::Bool)
                return (value[0] == '0' || value[0] == '\0') ? "false" : "true";
            if (definition.kind == Kind::Choice && !definition.choiceEnvironment.empty())
            {
                for (size_t index = 0; index < definition.choiceEnvironment.size(); ++index)
                {
                    if (!definition.choiceEnvironment[index].empty() && definition.choiceEnvironment[index] == value)
                        return definition.choices[index];
                }
                return definition.defaultValue;
            }
            return value;
        }

        // PCSX2 .pnach patches: patch=<place>,EE,<address>,<byte|short|word|extended>,<value>.
        bool parsePatch(const std::string &line, FrameWrite &write)
        {
            std::vector<std::string> fields;
            std::stringstream stream(line.substr(line.find('=') + 1));
            std::string field;
            while (std::getline(stream, field, ','))
                fields.push_back(trim(field));
            if (fields.size() < 5 || lower(fields[1]) != "ee")
                return false;
            uint32_t address = static_cast<uint32_t>(std::strtoul(fields[2].c_str(), nullptr, 16));
            const uint32_t value = static_cast<uint32_t>(std::strtoul(fields[4].c_str(), nullptr, 16));
            const std::string type = lower(fields[3]);
            uint8_t size = 0;
            if (type == "byte")
                size = 1;
            else if (type == "short")
                size = 2;
            else if (type == "word")
                size = 4;
            else if (type == "extended")
            {
                // CodeBreaker-style constant writes: 0aaaaaaa (8 bit), 1aaaaaaa (16), 2aaaaaaa (32).
                const uint32_t code = address >> 28;
                size = code == 0u ? 1u : code == 1u ? 2u : code == 2u ? 4u : 0u;
                address &= 0x0FFFFFFFu;
            }
            if (size == 0u)
                return false;
            write.address = address & 0x01FFFFFFu & ~(static_cast<uint32_t>(size) - 1u);
            write.value = value;
            write.size = size;
            return true;
        }

        void loadCheats(State &s)
        {
            s.cheats.clear();
            const std::filesystem::path folder = s.elfDirectory / "cheats";
            std::error_code error;
            if (!std::filesystem::is_directory(folder, error))
                return;
            std::vector<std::filesystem::path> files;
            for (const auto &entry : std::filesystem::directory_iterator(folder, error))
            {
                if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".pnach")
                    files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            for (const std::filesystem::path &path : files)
            {
                std::ifstream in(path);
                std::string line;
                CheatGroup current;
                current.name = path.stem().string();
                current.file = path.filename().string();
                auto flush = [&]()
                {
                    if (!current.writes.empty())
                        s.cheats.push_back(current);
                };
                while (std::getline(in, line))
                {
                    line = trim(line);
                    if (line.empty() || line[0] == ';' || line.rfind("//", 0) == 0)
                        continue;
                    if (line.front() == '[' && line.back() == ']')
                    {
                        flush();
                        current = CheatGroup{};
                        current.name = trim(line.substr(1, line.size() - 2));
                        current.file = path.filename().string();
                        continue;
                    }
                    const std::string lowered = lower(line);
                    if (lowered.rfind("description=", 0) == 0)
                        current.description = trim(line.substr(12));
                    else if (lowered.rfind("patch=", 0) == 0)
                    {
                        FrameWrite write{};
                        if (parsePatch(line, write))
                            current.writes.push_back(write);
                        else
                            std::cerr << "[settings] " << path.filename().string() << ": ignored '" << line << "'"
                                      << std::endl;
                    }
                }
                flush();
            }
        }

        // Rebuilds the enabled flags and the frame writes from [hacks] enabled.
        void refreshCheats(State &s)
        {
            const std::string list = s.values[keyOf("hacks", "enabled")];
            auto enabled = [&](const std::string &name)
            {
                std::stringstream stream(list);
                std::string item;
                while (std::getline(stream, item, '|'))
                {
                    if (trim(item) == name)
                        return true;
                }
                return false;
            };
            auto writes = std::make_shared<std::vector<FrameWrite>>();
            for (CheatGroup &group : s.cheats)
            {
                group.enabled = enabled(group.name);
                if (group.enabled)
                    writes->insert(writes->end(), group.writes.begin(), group.writes.end());
            }
            s.frameWrites = writes;
        }
    }

    const std::vector<Definition> &definitions()
    {
        static const std::vector<Definition> s_definitions = []()
        {
            std::vector<Definition> list;
            auto add = [&](Definition definition) { list.push_back(std::move(definition)); };
            // Vídeo
            {
                // Vulkan is paraLLEl-GS, in builds configured with PS2X_PGS_DIR (gs_pgs.h).
#if defined(PS2X_HAVE_PGS)
                constexpr bool kHaveVulkan = true;
#else
                constexpr bool kHaveVulkan = false;
#endif
                Definition renderer{"video", "renderer", "Renderizador",
                                    "Software desenha na CPU. Vulkan desenha na placa de vídeo (paraLLEl-GS, tão fiel "
                                    "quanto o Software) e permite resolução interna maior.",
                                    Kind::Choice, "software", "PS2X_RENDERER", {"software", "vulkan", "pcsx2"},
                                    {"Software (CPU)", kHaveVulkan ? "Vulkan (GPU)" : "Vulkan (não incluído nesta build)",
                                     "PCSX2 GS (GPU)"},
                                    0, 0, nullptr, false};
                renderer.choiceEnvironment = {"", "vulkan", "pcsx2"};
                renderer.choiceAvailable = {true, kHaveVulkan, true};
                add(renderer);
                Definition scale{"video", "internal_resolution", "Resolução interna",
                                 "Só no Vulkan. 2x e 4x desenham a imagem 2 e 4 vezes maior (detalhe nítido em telas "
                                 "grandes); \"suavizado\" desenha mais amostras por pixel no mesmo tamanho, o que "
                                 "tira o serrilhado. Quanto mais à direita, mais trabalho para a placa de vídeo.",
                                 Kind::Choice, "1", "PS2X_PGS_SCALE", {"1", "1aa", "2", "2aa", "4"},
                                 {"1x (nativa)", "1x suavizado", "2x", "2x suavizado", "4x"}, 0, 0, nullptr, false};
                scale.choiceEnvironment = {"", "1aa", "2", "2aa", "4"};
                scale.choiceAvailable = {true, kHaveVulkan, kHaveVulkan, kHaveVulkan, kHaveVulkan};
                add(scale);
            }
            {
                // LOCAL PATCH: options of the PCSX2 GS renderer (gs_pcsx2.cpp / the bridge's RRV_PCSX2_GS_* settings).
                Definition renderScale{"video", "render_scale", "Render resolution (PCSX2)",
                                       "How many times the PS2's resolution the PCSX2 renderer draws at: 2x is 1280x896, "
                                       "4x is 2560x1792. Higher is sharper and costs only GPU time.",
                                       Kind::Choice, "3", "PS2X_PCSX2_SCALE", {"1", "2", "3", "4", "6", "8", "12"},
                                       {"1x (native, interlaced)", "2x", "3x", "4x", "6x", "8x", "12x (8K)"}, 0, 0, nullptr,
                                       true}; // LOCAL PATCH: live (gs_pcsx2.cpp applyLiveSettings), except to or from 1x
                // LOCAL PATCH: 12x = 8K (7680x5376), PCSX2's highest upscale; the launcher offers it once the Mod Menu's
                // "8K resolution" switch is on.
                renderScale.choiceEnvironment = {"1", "", "3", "4", "6", "8", "12"};
                add(renderScale);
                add({"video", "allow_8k", "8K resolution",
                     "Offers 12x (8K) in the launcher's Resolution list. Needs a very powerful graphics card.", Kind::Bool,
                     "false", nullptr, {}, {}, 0, 0, nullptr, false});
                Definition gpuApi{"video", "pcsx2_renderer", "Graphics API (PCSX2)",
                                  "Which GPU interface the PCSX2 renderer uses. Both give the same picture.",
                                  Kind::Choice, "d3d11", "PS2X_PCSX2_RENDERER", {"d3d11", "vulkan"},
                                  {"Direct3D 11", "Vulkan"}, 0, 0, nullptr, false};
                gpuApi.choiceEnvironment = {"d3d11", "vulkan"};
                add(gpuApi);
                add({"video", "edge_aa", "Edge anti-aliasing (PCSX2)",
                     "The PS2's own edge smoothing (GS AA1) on the polygons that ask for it.", Kind::Bool, "false",
                     "RRV_PCSX2_GS_FULL_HWAA1", {}, {}, 0, 0, nullptr, true}); // LOCAL PATCH: live
                Definition aniso{"video", "anisotropic", "Anisotropic filtering (PCSX2)",
                                 "Sharper textures on surfaces seen at an angle (floors, walls).", Kind::Choice, "2",
                                 "RRV_PCSX2_GS_ANISO", {"off", "2", "4", "8", "16"}, {"Off", "2x", "4x", "8x", "16x"}, 0, 0,
                                 nullptr, true}; // LOCAL PATCH: live
                aniso.choiceEnvironment = {"", "2", "4", "8", "16"};
                add(aniso);
                Definition texFilter{"video", "texture_filter", "Texture filtering (PCSX2)",
                                     "PS2 uses each texture as the game asked; bilinear smooths every texture.",
                                     Kind::Choice, "ps2", "RRV_PCSX2_GS_TEXTURE_FILTER", {"ps2", "bilinear"},
                                     {"As on PS2", "Bilinear (smooth)"}, 0, 0, nullptr, true}; // LOCAL PATCH: live
                texFilter.choiceEnvironment = {"", "bilinear"};
                add(texFilter);
                // LOCAL PATCH (low-end GPUs): PCSX2's blending accuracy. Basic is PCSX2's own default; Minimum skips the
                // extra blending passes on slower graphics cards (the launcher's Low-end PC preset).
                Definition blending{"video", "blending", "Blending accuracy (PCSX2)",
                                    "How exactly see-through effects are blended. Minimum is lighter on slow graphics cards.",
                                    Kind::Choice, "basic", "RRV_PCSX2_GS_BLENDING", {"minimum", "basic", "medium", "high"},
                                    {"Minimum", "Basic", "Medium", "High"}, 0, 0, nullptr, true}; // LOCAL PATCH: live
                blending.choiceEnvironment = {"0", "1", "2", "3"};
                add(blending);
                // LOCAL PATCH: the launcher's Effects tab (PCSX2 options through the bridge).
                Definition dithering{"video", "dithering", "Dithering (PCSX2)",
                                     "The PS2's grainy colour pattern. Off gives smooth, clean colour gradients.",
                                     Kind::Choice, "on", "RRV_PCSX2_GS_DITHERING", {"on", "off"}, {"On", "Off"}, 0, 0,
                                     nullptr, true}; // live (applyLiveSettings)
                dithering.choiceEnvironment = {"2", "0"};
                add(dithering);
                Definition mipmap{"video", "mipmapping", "Mipmapping (PCSX2)",
                                  "Smaller versions of textures far away: less shimmer on distant floors and walls.",
                                  Kind::Choice, "on", "RRV_PCSX2_GS_FULL_HWMIPMAP", {"on", "off"}, {"On", "Off"}, 0, 0,
                                  nullptr, false};
                mipmap.choiceEnvironment = {"1", "0"};
                add(mipmap);
                Definition upfix{"video", "upscale_fixes", "Upscaling fixes (PCSX2)",
                                 "Half-pixel offset and sprite rounding: removes seams and thin lines at high resolutions.",
                                 Kind::Choice, "on", "RRV_PCSX2_GS_FULL_HPO", {"on", "off"}, {"On", "Off"}, 0, 0, nullptr,
                                 false};
                upfix.choiceEnvironment = {"", "0"};
                add(upfix);
                // LOCAL PATCH: done by the window's own shader (casSharpen, ps2_runtime.cpp): PCSX2's CAS runs at its
                // presentation step, which the bridge's picture never goes through, so RRV_PCSX2_GS_CAS did nothing.
                add({"video", "sharpening", "Sharpening",
                     "Contrast adaptive sharpening of the finished picture, 0 = off. Applies at once.", Kind::Number, "20",
                     nullptr, {}, {}, 0, 100, "Off", true});
                Definition fps{"video", "fps_target", "Frame rate target",
                               "60 is the game's own rate. 30 shows a steady, evenly paced 30; 50 shows 5 of every 6 "
                               "frames at normal speed (for 50 Hz screens); unlocked removes every cap and the vsync wait.",
                               Kind::Choice, "60", "PS2X_FPS_TARGET", {"60", "30", "50", "unlocked"},
                               {"60 fps", "30 fps (locked)", "50 fps", "Unlocked"}, 0, 0, nullptr, false};
                fps.choiceEnvironment = {"", "30", "50", "unlocked"};
                add(fps);
            }
            {
                // LOCAL PATCH (input): the camera on keyboard and mouse (the PS2 pad's right stick), and the stick dead zone.
                add({"input", "mouse_look", "Mouse camera",
                     "Moving the mouse turns the camera (the pad's right stick) and the pointer stays inside the game window. "
                     "F10 switches it on and off while playing; it is off while the F1 menu is open. Keyboard: I J K L is the right stick.",
                     Kind::Choice, "on", nullptr, {"on", "off"}, {"On", "Off"}, 0, 0, nullptr, true});
                add({"input", "mouse_sensitivity", "Mouse camera speed",
                     "How fast the mouse turns the camera, in percent (100 = normal).", Kind::Number, "100", nullptr, {}, {}, 10, 400,
                     nullptr, true});
                add({"input", "stick_deadzone", "Stick dead zone",
                     "How far a controller stick has to move before the game sees it, in percent. Raise it if the character or "
                     "camera drifts on its own.", Kind::Number, "15", nullptr, {}, {}, 0, 50, nullptr, true});
                // LOCAL PATCH: DualSense haptics and adaptive triggers (ps2_dualsense.cpp).
                add({"input", "haptics", "DualSense haptics",
                     "Footsteps, shuriken throws, ninja magic and hits felt through a PS5 DualSense or DualSense Edge.",
                     Kind::Bool, "true", nullptr, {}, {}, 0, 0, nullptr, true});
                add({"input", "haptic_strength", "Haptic strength", "How strong the DualSense haptics are, in percent.",
                     Kind::Number, "100", nullptr, {}, {}, 0, 100, nullptr, true});
                add({"input", "trigger_resistance", "L2 trigger resistance",
                     "L2 stiffens part way down and gives way with a click just before the ninja magic fires.",
                     Kind::Bool, "true", nullptr, {}, {}, 0, 0, nullptr, true});
            }
            {
                // LOCAL PATCH (effects): the PCSX2 renderer settings that decide whether Shinobi's screen effects (fog, glow,
                // blur, trails) survive drawing above the PS2's resolution. Shinobi's defaults are set in gs_pcsx2.cpp
                // (RR5-only fixes off, native scaling on); these only take effect when written to the file.
                Definition nativeScaling{"effects", "native_scaling", "Effects at high resolution (PCSX2 native scaling)",
                                         "Blur, glow and fog passes are drawn at the PS2's own size and then scaled, so they "
                                         "keep their look when the game is drawn at 2x-6x. Off = PCSX2's plain upscaling.",
                                         Kind::Choice, "normal", "RRV_PCSX2_GS_FULL_NATIVE_SCALING", {"off", "normal", "aggressive"},
                                         {"Off", "Normal", "Aggressive"}, 0, 0, nullptr, false};
                nativeScaling.choiceEnvironment = {"0", "1", "2"};
                add(nativeScaling);
                add({"effects", "rr5_dof_fix", "Ridge Racer blur fix (bridge)",
                     "A depth-of-field fix written for Ridge Racer V. Not needed by Shinobi; can damage its effects.",
                     Kind::Bool, "false", "RRV_PCSX2_GS_FULL_DOF_NATIVE", {}, {}, 0, 0, nullptr, false});
                Definition largeSt{"effects", "large_st", "Stretched triangles (bridge)",
                                   "Ridge Racer V's fix drops triangles with very large texture coordinates; full-screen effect "
                                   "layers can be such triangles. Keep = draw them all, as PCSX2 does.",
                                   Kind::Choice, "keep", "RRV_PCSX2_GS_FULL_LARGE_ST", {"keep", "mode1", "drop"},
                                   {"Keep all (PCSX2)", "Mode 1", "Drop (Ridge Racer)"}, 0, 0, nullptr, false};
                largeSt.choiceEnvironment = {"0", "1", "2"};
                add(largeSt);
                Definition hpo{"effects", "half_pixel_offset", "Half-pixel offset (PCSX2)",
                               "Lines up effect layers with the scene at raised resolutions (blurry or doubled glow/fog edges).",
                               Kind::Choice, "auto", "RRV_PCSX2_GS_FULL_HPO", {"auto", "0", "1", "2", "3", "4", "5"},
                               {"Bridge default", "Off", "Normal (vertex)", "Special (texture)", "Special (aggressive)",
                                "Align to native", "Align to native + texture"},
                               0, 0, nullptr, false};
                hpo.choiceEnvironment = {"", "0", "1", "2", "3", "4", "5"};
                add(hpo);
                Definition roundSprite{"effects", "round_sprite", "Round sprite (PCSX2)",
                                       "Fixes seams in 2D sprites (HUD, particles) at raised resolutions.", Kind::Choice, "auto",
                                       "RRV_PCSX2_GS_FULL_ROUND_SPRITE", {"auto", "0", "1", "2"},
                                       {"Bridge default", "Off", "Half", "Full"}, 0, 0, nullptr, false};
                roundSprite.choiceEnvironment = {"", "0", "1", "2"};
                add(roundSprite);
                Definition spriteFilter{"effects", "sprite_filter", "Sprite filtering (bridge)",
                                        "Even = whole texels on point-sampled sprites (no seams); PS2 = exactly as the game asks.",
                                        Kind::Choice, "auto", "RRV_PCSX2_GS_SPRITE_FILTER", {"auto", "even", "ps2"},
                                        {"Bridge default", "Even", "As on PS2"}, 0, 0, nullptr, false};
                spriteFilter.choiceEnvironment = {"", "even", "ps2"};
                add(spriteFilter);
                Definition mipmap{"effects", "mipmapping", "Texture mipmapping (PCSX2)",
                                  "The GS's own smaller texture levels for distant surfaces (less shimmer).", Kind::Choice, "auto",
                                  "RRV_PCSX2_GS_FULL_HWMIPMAP", {"auto", "on", "off"}, {"Bridge default", "On", "Off"}, 0, 0,
                                  nullptr, false};
                mipmap.choiceEnvironment = {"", "1", "0"};
                add(mipmap);
                add({"effects", "record_drawing", "Record drawing (effects check)",
                     "Writes every drawing command from boot to captures/gs_stream.bin (several GB) so the effects can be "
                     "replayed and compared offline (scripts/effects_check.bat).",
                     Kind::Bool, "false", "PS2X_PCSX2_STREAM", {}, {}, 0, 0, nullptr, false});
            }
            add({"video", "window", "Janela", "Como o jogo ocupa o monitor.", Kind::Choice, "windowed", nullptr,
                 {"windowed", "fullscreen", "borderless"}, {"Janela", "Tela cheia", "Tela cheia sem borda"}, 0, 0, nullptr,
                 true});
            {
                // LOCAL PATCH: 32:9 and auto (the monitor's own shape) for ultrawide screens. Shinobi widens its camera
                // through AdjustDisplayGameAspect (the PCSX2 widescreen patch's spot, see ps2xWidescreenFactor).
                Definition aspect{"video", "aspect", "Aspect ratio",
                                  "4:3 is the TV shape the game was made for. 16:9, 21:9 and 32:9 widen the camera so you "
                                  "see more to the sides; auto uses your monitor's exact shape. Menus and movies stay 4:3.",
                                  Kind::Choice, "16:9", "PS2X_WIDESCREEN", {"4:3", "16:9", "21:9", "32:9", "auto", "stretch"},
                                  {"4:3", "16:9", "21:9 ultrawide", "32:9 super ultrawide", "Auto (monitor)", "Stretch"}, 0,
                                  0, nullptr, true}; // LOCAL PATCH: live (ps2xWidescreenVsync)
                aspect.choiceEnvironment = {"", "16:9", "21:9", "32:9", "auto", ""};
                add(aspect);
            }
            {
                Definition hud{"video", "hud", "HUD no widescreen",
                               "Em 16:9 e 21:9, nos jogos que suportam: centralizado mantém o HUD de 4:3 no meio da "
                               "tela; nas bordas encosta cada lado do HUD nas laterais; esticado estica HUD, menus e "
                               "vídeos junto com a imagem.",
                               Kind::Choice, "center", "PS2X_WIDESCREEN_HUD", {"center", "edges", "stretch"},
                               {"Centralizado", "Nas bordas", "Esticado"}, 0, 0, nullptr, false};
                hud.choiceEnvironment = {"", "edges", "stretch"};
                add(hud);
            }
            add({"video", "filter", "Filtro da imagem",
                 "Nítido mostra os pixels; suave os mistura ao ampliar; scanlines e CRT imitam a TV.", Kind::Choice,
                 "smooth", nullptr, {"sharp", "smooth", "scanlines", "crt"}, {"Nítido", "Suave", "Scanlines", "CRT"}, 0,
                 0, nullptr, true});
            add({"video", "antialias", "Antisserrilhado (FXAA)", "Suaviza as bordas serrilhadas dos objetos.",
                 Kind::Bool, "false", nullptr, {}, {}, 0, 0, nullptr, true});
            add({"video", "vsync", "VSync", "Sincroniza com o monitor (evita imagem rasgada).", Kind::Bool, "true",
                 nullptr, {}, {}, 0, 0, nullptr, true});
            add({"video", "deinterlace", "Desentrelaçamento", "Junta os dois campos entrelaçados como uma TV faz.",
                 Kind::Bool, "true", "PS2X_DEINTERLACE", {}, {}, 0, 0, nullptr, false});
            // Desempenho
            add({"performance", "gs_threads", "Threads do renderizador",
                 "Threads que desenham a imagem na CPU. Automático usa 3/4 dos núcleos lógicos.", Kind::Number, "0",
                 "PS2X_GS_THREADS", {}, {}, 0, 32, "Automático", false});
            add({"performance", "vif1_thread", "VIF1 em thread própria",
                 "Processa a geometria em paralelo com a lógica do jogo, como no console.", Kind::Bool, "true",
                 "PS2X_VIF1_THREAD", {}, {}, 0, 0, nullptr, false});
            add({"performance", "vu1_threads", "Threads do VU1",
                 "Programas do VU1 em paralelo, mesmo resultado; cerca de +10% em batalha. Automático usa 3 com 8 "
                 "ou mais núcleos lógicos.", Kind::Choice, "auto", "PS2X_VU1_THREADS", {"auto", "0", "2", "3", "4"},
                 {"Automático", "Desligado", "2", "3", "4"}, 0, 0, nullptr, false});
            add({"performance", "vu1_depth", "Execuções do VU1 adiantadas",
                 "Quantas execuções do VU1 podem começar antes das anteriores terminarem.", Kind::Number, "0",
                 "PS2X_VU1_DEPTH", {}, {}, 0, 64, "Automático", false});
            {
                // LOCAL PATCH: PS2X_VU1_FAST (ps2_vu1_compiled.h): out-of-range VU maths clamped like PCSX2's default
                // instead of the exact (slow) path. Off by default: exact results.
                Definition fastVu{"performance", "fast_vu", "Fast VU maths",
                                  "Faster vector-unit maths, like PCSX2's default settings. Rare results can differ from the "
                                  "console's; turn off if something looks wrong.",
                                  Kind::Choice, "off", "PS2X_VU1_FAST", {"off", "on"}, {"Off (exact)", "On (faster)"}, 0, 0,
                                  nullptr, false};
                fastVu.choiceEnvironment = {"", "1"};
                add(fastVu);
            }
            // LOCAL PATCH (mod): the close-up, over-the-shoulder camera (ps2_close_camera.cpp). F3 in game.
            add({"game", "close_camera", "Close-up camera",
                 "A closer, over-the-shoulder camera for more intimate combat. F3 switches it while playing.", Kind::Bool,
                 "false", nullptr, {}, {}, 0, 0, nullptr, true});
            // LOCAL PATCH: the performance overlay (FPS, frame time, 1% low, CPU, GPU, RAM, VRAM) in the top-left corner.
            add({"performance", "stats_overlay", "Performance overlay",
                 "FPS, frame time, 1% lows, CPU and GPU use, RAM and VRAM in the top-left corner while you play.",
                 Kind::Bool, "false", nullptr, {}, {}, 0, 0, nullptr, true});
            add({"performance", "show_fps", "Mostrar FPS", "Contador de quadros por segundo no canto da tela.",
                 Kind::Bool, "false", nullptr, {}, {}, 0, 0, nullptr, true});
            // Áudio
            add({"audio", "volume", "Volume geral", "Volume de saída do jogo.", Kind::Number, "100", nullptr, {}, {}, 0,
                 100, nullptr, true});
            {
                // LOCAL PATCH: PCSX2's SPU2 core (default) or the runtime's own older sound model (iop_spu2.cpp).
                Definition chip{"audio", "sound_chip", "Sound chip emulation",
                                "PCSX2 is the accurate SPU2 (voices, streaming, reverb). Legacy is the older built-in model.",
                                Kind::Choice, "pcsx2", "PS2X_SPU2", {"pcsx2", "legacy"}, {"PCSX2 SPU2", "Legacy"}, 0, 0,
                                nullptr, false};
                chip.choiceEnvironment = {"", "legacy"};
                add(chip);
                // LOCAL PATCH: the streamed music's right channel reaches the sound chip damaged (static, a late copy of the
                // sound) while the left is clean; this plays the left one in both ears. Sound effects stay in stereo.
                Definition repair{"audio", "stereo_repair", "Music: right ear",
                                  "Copy left plays the music's clean left channel in both ears, which avoids the static some "
                                  "scenes put in the right ear. Sound effects stay in stereo. Takes effect at the next start.",
                                  Kind::Choice, "copy_left", "PS2X_SPU2_COPY_LEFT", {"copy_left", "stereo"},
                                  {"Copy left (no static)", "True stereo"}, 0, 0, nullptr, false};
                repair.choiceEnvironment = {"1", ""};
                add(repair);
            }
            // Saves
            // LOCAL PATCH: saves live in their own "saves" folder next to the launcher (../saves/mc0 from the game folder),
            // not inside the game's files, so they belong to the player, survive a re-setup of the game files and are easy to
            // find and back up.
            add({"saves", "memcard_folder", "Memory card 1 folder",
                 "Where the saves of slot 1 are kept (slot 2 uses the sibling folder mc1). Relative to the game folder: "
                 "../saves/mc0 is the saves folder next to the launcher.", Kind::Path,
                 "../saves/mc0", nullptr, {}, {}, 0, 0, nullptr, false});
            // Jogo (ganchos que o override do jogo implementa)
            add({"game", "skip_intro", "Skip start-up logos",
                 "SEGA, Overworks and ADX logos pass in about a second each (a button press during them does the same).",
                 Kind::Bool, "false",
                 "PS2X_SKIP_INTRO", {}, {}, 0, 0, nullptr, false});
            // LOCAL PATCH: the studio intro video before the game's own logos (ps2_intro_video.h).
            add({"game", "studio_intro", "Studio intro (Twilight Orchard)",
                 "Plays intro/studio_intro.mp4 before the game starts. Any key or button skips it.", Kind::Bool, "true",
                 "PS2X_STUDIO_INTRO", {}, {}, 0, 0, nullptr, false});
            // LOCAL PATCH (mod): Hotsuma's scarf colour, "original" (red) or #RRGGBB (ps2_scarf_colour.cpp). Live: the
            // in-game menu can change it while playing.
            add({"game", "scarf_color", "Scarf colour",
                 "Hotsuma's scarf: original (red) or any colour as #RRGGBB.", Kind::Path, "original", nullptr, {}, {}, 0, 0,
                 nullptr, true});
            // Saves: cópias de segurança
            add({"saves", "backups", "Cópias de segurança",
                 "Ao abrir o jogo, copia os memory cards para saves_backup/ e guarda as mais recentes.",
                 Kind::Number, "5", nullptr, {}, {}, 0, 20, "Desligado", false});
            // Controles (teclado): uma lista de teclas por botão
            for (const PadButton &button : padButtons())
            {
                static const std::map<std::string, std::string> kDefaults = {
                    {"up", "UP,W"},       {"down", "DOWN,S"},     {"left", "LEFT,A"},       {"right", "RIGHT,D"},
                    {"cross", "X,SPACE"}, {"circle", "C,ESCAPE"}, {"square", "Z,KP_0"},     {"triangle", "V,KP_1"},
                    {"l1", "Q"},          {"r1", "E"},            {"l2", "LEFT_SHIFT"},     {"r2", "RIGHT_SHIFT"},
                    {"start", "ENTER"},   {"select", "TAB"},      {"l3", ""},               {"r3", ""},
                    {"camera_up", "I"},   {"camera_down", "K"},   {"camera_left", "J"},     {"camera_right", "L"},
                };
                const auto found = kDefaults.find(button.key);
                Definition definition{"controls", button.key, button.label, "", Kind::Keys, "", nullptr, {}, {}};
                definition.defaultValue = found != kDefaults.end() ? found->second.c_str() : "";
                definition.live = true;
                add(definition);
            }
            // Avançado
            add({"advanced", "debug_panel", "Painel de depuração", "CPU, threads, GS, DMA e memória (F2).", Kind::Bool,
                 "false", nullptr, {}, {}, 0, 0, nullptr, true});
            add({"advanced", "frame_dump_dir", "Salvar quadros em",
                 "Pasta para salvar quadros (para comparar com o PCSX2). Vazio desliga.", Kind::Path, "",
                 "PS2X_FRAME_DUMP_DIR", {}, {}, 0, 0, nullptr, false});
            return list;
        }();
        return s_definitions;
    }

    void load(const std::filesystem::path &elfPath)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.loaded)
            return;
        s.loaded = true;
        std::error_code error;
        s.elfDirectory = std::filesystem::absolute(elfPath, error).parent_path();
        const char *choice = std::getenv("PS2X_SETTINGS");
        if (choice && lower(choice) == "off")
            s.disabled = true;
        else if (choice && choice[0] != '\0')
            s.file = choice;
        else
            s.file = s.elfDirectory / "settings.ini";

        std::map<std::string, std::string> fileValues;
        if (!s.disabled)
        {
            std::ifstream in(s.file);
            std::string line;
            std::string section;
            while (std::getline(in, line))
            {
                line = trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#')
                    continue;
                if (line.front() == '[' && line.back() == ']')
                {
                    section = lower(trim(line.substr(1, line.size() - 2)));
                    continue;
                }
                const size_t equals = line.find('=');
                if (equals == std::string::npos || section.empty())
                    continue;
                std::string value = trim(line.substr(equals + 1));
                const size_t comment = value.find(" ;");
                if (comment != std::string::npos)
                    value = trim(value.substr(0, comment));
                fileValues[section + "." + lower(trim(line.substr(0, equals)))] = value;
            }
        }

        for (const Definition &definition : definitions())
        {
            const std::string key = keyOf(definition.section, definition.key);
            const auto inFile = fileValues.find(key);
            std::string value = inFile != fileValues.end() ? inFile->second : definition.defaultValue;
            if (definition.environment)
            {
                if (const char *env = std::getenv(definition.environment))
                {
                    s.fileValue[key] = value;
                    value = fromEnvironmentValue(definition, env);
                    s.fromEnvironment[key] = true;
                }
                else if (inFile != fileValues.end() || std::strcmp(definition.key, "stereo_repair") == 0)
                {
                    // (stereo_repair is on by default: its default has to reach the sound code although the file lacks the key.)
                    const std::string env = environmentValue(definition, value);
                    if (!env.empty())
                        setEnvironment(definition.environment, env);
                }
            }
            s.values[key] = value;
        }
        s.values[keyOf("hacks", "enabled")] = fileValues[keyOf("hacks", "enabled")];
        s.atLoad = s.values;
        loadCheats(s);
        refreshCheats(s);
        std::cerr << "[settings] " << (s.disabled ? std::string("off") : s.file.string()) << ", " << s.cheats.size()
                  << " cheat groups" << std::endl;
    }

    bool save()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled || s.file.empty())
            return false;
        std::ofstream out(s.file, std::ios::trunc);
        if (!out)
            return false;
        out << "; Configurações do jogo, gravadas pelo menu (F1).\n";
        out << "; Uma variável PS2X_* definida no ambiente vale mais que este arquivo.\n";
        // LOCAL PATCH: every section once (definitions of one section are not contiguous, and a repeated [video] header made
        // the last copy win when the file was read back).
        std::vector<std::string> sectionOrder;
        for (const Definition &definition : definitions())
            if (std::find(sectionOrder.begin(), sectionOrder.end(), definition.section) == sectionOrder.end())
                sectionOrder.push_back(definition.section);
        for (const std::string &section : sectionOrder)
        {
            out << "\n[" << section << "]\n";
            for (const Definition &definition : definitions())
            {
                if (section != definition.section)
                    continue;
                const std::string key = keyOf(definition.section, definition.key);
                const auto fromFile = s.fileValue.find(key);
                out << definition.key << " = " << (fromFile != s.fileValue.end() ? fromFile->second : s.values[key]) << "\n";
            }
        }
        out << "\n[hacks]\n; grupos de cheats/*.pnach ligados, separados por |\n";
        out << "enabled = " << s.values[keyOf("hacks", "enabled")] << "\n";
        return static_cast<bool>(out);
    }

    std::filesystem::path filePath()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.disabled ? std::filesystem::path() : s.file;
    }

    std::string get(const char *section, const char *key)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto it = s.values.find(keyOf(section, key));
        if (it != s.values.end())
            return it->second;
        const Definition *definition = find(section, key);
        return definition ? definition->defaultValue : std::string();
    }

    bool getBool(const char *section, const char *key)
    {
        return get(section, key) == "true";
    }

    int getNumber(const char *section, const char *key)
    {
        return std::atoi(get(section, key).c_str());
    }

    void set(const char *section, const char *key, const std::string &value)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        {
            // LOCAL DEBUG: who changed what while the game runs ("[settings]" lines in the log).
            const std::string k = keyOf(section, key);
            const auto previous = s.values.find(k);
            if (s.loaded && previous != s.values.end() && previous->second != value)
                std::fprintf(stderr, "[settings] %s.%s: %s -> %s\n", section, key, previous->second.c_str(), value.c_str());
        }
        s.values[keyOf(section, key)] = value;
        if (std::string(section) == "hacks")
            refreshCheats(s);
        g_version.fetch_add(1u, std::memory_order_relaxed);
    }

    bool overriddenByEnvironment(const char *section, const char *key)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.fromEnvironment.count(keyOf(section, key)) != 0;
    }

    void resetToDefaults()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (const Definition &definition : definitions())
        {
            const std::string key = keyOf(definition.section, definition.key);
            if (!s.fromEnvironment.count(key))
                s.values[key] = definition.defaultValue;
            else
                s.fileValue[key] = definition.defaultValue;
        }
        s.values[keyOf("hacks", "enabled")].clear();
        refreshCheats(s);
        g_version.fetch_add(1u, std::memory_order_relaxed);
    }

    bool restartNeeded()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (const Definition &definition : definitions())
        {
            const std::string key = keyOf(definition.section, definition.key);
            if (!definition.live && s.values[key] != s.atLoad[key])
                return true;
            // LOCAL PATCH: the render scale changes live within the progressive mode (2x..12x); 1x is another render
            // mode (interlaced fields), so going to or from it still needs a restart.
            if (key == keyOf("video", "render_scale") && s.values[key] != s.atLoad[key] &&
                (s.values[key] == "1" || s.atLoad[key] == "1"))
                return true;
        }
        return false;
    }

    std::vector<CheatGroup> cheatGroups()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.cheats;
    }

    void setCheatEnabled(const std::string &name, bool enabled)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::string list;
        for (const CheatGroup &group : s.cheats)
        {
            const bool on = group.name == name ? enabled : group.enabled;
            if (!on)
                continue;
            if (!list.empty())
                list += " | ";
            list += group.name;
        }
        s.values[keyOf("hacks", "enabled")] = list;
        refreshCheats(s);
    }

    std::shared_ptr<const std::vector<FrameWrite>> activeFrameWrites()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.frameWrites;
    }

    uint64_t version()
    {
        return g_version.load(std::memory_order_relaxed);
    }

    const std::vector<PadButton> &padButtons()
    {
        static const std::vector<PadButton> s_buttons = {
            {"up", "Direcional ↑", 0x0010u},   {"down", "Direcional ↓", 0x0040u},
            {"left", "Direcional ←", 0x0080u}, {"right", "Direcional →", 0x0020u},
            {"cross", "X (Xis)", 0x4000u},     {"circle", "O (Bola)", 0x2000u},
            {"square", "Quadrado", 0x8000u},   {"triangle", "Triângulo", 0x1000u},
            {"l1", "L1", 0x0400u},             {"r1", "R1", 0x0800u},
            {"l2", "L2", 0x0100u},             {"r2", "R2", 0x0200u},
            {"start", "Start", 0x0008u},       {"select", "Select", 0x0001u},
            {"l3", "L3", 0x0002u},             {"r3", "R3", 0x0004u},
            // The camera (the pad's right stick) on the keyboard: no button bit, ps2_pad.cpp reads these four itself.
            {"camera_up", "Camera up (right stick)", 0u},       {"camera_down", "Camera down (right stick)", 0u},
            {"camera_left", "Camera left (right stick)", 0u},   {"camera_right", "Camera right (right stick)", 0u},
        };
        return s_buttons;
    }

    int keyCode(const std::string &name)
    {
        for (const KeyName &key : keyNames())
        {
            if (name == key.name)
                return key.code;
        }
        return 0;
    }

    std::string keyName(int code)
    {
        for (const KeyName &key : keyNames())
        {
            if (code == key.code)
                return key.name;
        }
        return std::string();
    }

    void backupMemoryCards(const std::filesystem::path &mcRoot, int keep)
    {
        if (keep <= 0)
            return;
        std::error_code error;
        std::vector<std::filesystem::path> cards;
        for (const std::filesystem::path &folder : {mcRoot, mcRoot.parent_path() / "mc1"})
        {
            if (std::filesystem::is_directory(folder, error) && !std::filesystem::is_empty(folder, error))
                cards.push_back(folder);
        }
        if (cards.empty())
            return;
        std::filesystem::path backups;
        {
            State &s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            (void)s;
            backups = mcRoot.parent_path() / "backups"; // saves/backups next to the memory card folders
        }
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
        for (const std::filesystem::path &card : cards)
        {
            const std::filesystem::path target = backups / stamp / card.filename();
            std::filesystem::create_directories(target, error);
            std::filesystem::copy(card, target,
                                  std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                  error);
            if (error)
                std::cerr << "[settings] backup of " << card.string() << " failed: " << error.message() << std::endl;
        }
        // Keep the newest copies (the names sort by date).
        std::vector<std::filesystem::path> copies;
        for (const auto &entry : std::filesystem::directory_iterator(backups, error))
        {
            if (entry.is_directory())
                copies.push_back(entry.path());
        }
        std::sort(copies.begin(), copies.end());
        while (copies.size() > static_cast<size_t>(keep))
        {
            std::filesystem::remove_all(copies.front(), error);
            copies.erase(copies.begin());
        }
        std::cerr << "[settings] memory cards copied to " << (backups / stamp).string() << std::endl;
    }

    namespace
    {
        std::atomic<bool> g_inputBlocked{false};
        std::atomic<uint64_t> g_gameFrames{0};
    }

    void setInputBlocked(bool blocked)
    {
        g_inputBlocked.store(blocked, std::memory_order_relaxed);
    }

    bool inputBlocked()
    {
        return g_inputBlocked.load(std::memory_order_relaxed);
    }

    void countGameFrame()
    {
        g_gameFrames.fetch_add(1u, std::memory_order_relaxed);
    }

    uint64_t gameFrames()
    {
        return g_gameFrames.load(std::memory_order_relaxed);
    }
}
