#pragma once
// LOCAL PATCH: studio intro. Plays a video file (game/intro/studio_intro.mp4: the Twilight Orchard opening) before the game
// boots, i.e. before the game's own logos. Decoding uses Windows Media Foundation (H.264/AAC/HEVC as Windows supports them)
// on a worker thread; this header has no Windows or raylib types, ps2_runtime.cpp draws the frames and plays the sound.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::intro
{
    // Which file to play, or empty for none. PS2X_STUDIO_INTRO (settings [game] studio_intro): unset or "1" = intro/
    // studio_intro.mp4 next to the game when it exists, "0" = off, anything else = a path.
    std::string configuredFile();

    class Video
    {
    public:
        ~Video();
        // Opens the file, decodes the whole sound track (16-bit stereo 48 kHz) and starts decoding pictures ahead.
        static std::unique_ptr<Video> open(const std::string &path);

        [[nodiscard]] uint32_t width() const { return m_width; }
        [[nodiscard]] uint32_t height() const { return m_height; }
        [[nodiscard]] double seconds() const { return m_seconds; }
        // Interleaved stereo 16-bit samples at 48000 Hz (empty when the file has no sound or it could not be decoded).
        [[nodiscard]] const std::vector<int16_t> &audio() const { return m_audio; }

        // The newest picture due at `time` seconds, as RGBA8 rows of width() pixels (older due pictures are skipped).
        // False when no new picture is due yet. `finished` turns true after the last picture.
        bool pictureAt(double time, std::vector<uint8_t> &rgba, bool &finished);

    private:
        struct Impl;
        Video() = default;
        std::unique_ptr<Impl> m_impl;
        uint32_t m_width = 0, m_height = 0;
        double m_seconds = 0.0;
        std::vector<int16_t> m_audio;
    };
}
