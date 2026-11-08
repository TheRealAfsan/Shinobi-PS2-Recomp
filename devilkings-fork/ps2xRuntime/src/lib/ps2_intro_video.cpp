// LOCAL PATCH: studio intro video decoding (see runtime/ps2_intro_video.h). Windows Media Foundation source readers: one
// for the pictures (converted to 32-bit RGB by Media Foundation's video processor) on a worker thread that keeps a few
// pictures ready, one that decodes the sound track up front. Kept in its own file: windows.h clashes with raylib.
#include "runtime/ps2_intro_video.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

namespace ps2x::intro
{
    std::string configuredFile()
    {
        const char *e = std::getenv("PS2X_STUDIO_INTRO");
        if (e && std::string(e) == "0")
            return std::string();
        std::string path = (e && e[0] && std::string(e) != "1") ? std::string(e) : std::string("intro/studio_intro.mp4");
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            if (e && e[0] && std::string(e) != "1")
                std::fprintf(stderr, "[intro] %s not found, no studio intro\n", path.c_str());
            return std::string();
        }
        return path;
    }

#if defined(_WIN32)
    namespace
    {
        template <class T>
        void release(T *&p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        std::wstring widen(const std::string &text)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            std::wstring out(static_cast<size_t>(std::max(n, 1)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), n);
            out.resize(std::wcslen(out.c_str()));
            return out;
        }

        IMFSourceReader *openReader(const std::wstring &path, bool videoProcessing)
        {
            IMFAttributes *attributes = nullptr;
            if (FAILED(MFCreateAttributes(&attributes, 1)))
                return nullptr;
            if (videoProcessing)
                attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            IMFSourceReader *reader = nullptr;
            const HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attributes, &reader);
            release(attributes);
            return SUCCEEDED(hr) ? reader : nullptr;
        }
    }

    struct Video::Impl
    {
        struct Picture
        {
            double time = 0.0;
            std::vector<uint8_t> rgba;
        };

        IMFSourceReader *reader = nullptr;
        uint32_t width = 0, height = 0;
        int32_t stride = 0;
        std::thread worker;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Picture> ready;
        std::vector<std::vector<uint8_t>> spare;
        bool stop = false;
        bool ended = false;

        void run()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&] { return stop || ready.size() < 4u; });
                    if (stop)
                        break;
                }
                DWORD stream = 0, flags = 0;
                LONGLONG timestamp = 0;
                IMFSample *sample = nullptr;
                const HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, &sample);
                if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
                {
                    release(sample);
                    std::lock_guard<std::mutex> lock(mutex);
                    ended = true;
                    break;
                }
                if (!sample)
                    continue;
                Picture picture;
                picture.time = static_cast<double>(timestamp) / 1e7;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!spare.empty())
                    {
                        picture.rgba.swap(spare.back());
                        spare.pop_back();
                    }
                }
                picture.rgba.resize(static_cast<size_t>(width) * height * 4u);
                IMFMediaBuffer *buffer = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
                {
                    BYTE *scan0 = nullptr;
                    LONG pitch = 0;
                    IMF2DBuffer *buffer2d = nullptr;
                    BYTE *data = nullptr;
                    DWORD maxLength = 0, length = 0;
                    bool locked2d = false;
                    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch)))
                        locked2d = true;
                    else if (SUCCEEDED(buffer->Lock(&data, &maxLength, &length)))
                    {
                        // Media Foundation's RGB32 rows: top-down for a positive default stride, bottom-up for a negative one.
                        pitch = stride != 0 ? stride : static_cast<LONG>(width * 4u);
                        scan0 = pitch < 0 ? data + static_cast<size_t>(-pitch) * (height - 1u) : data;
                    }
                    if (scan0)
                    {
                        for (uint32_t y = 0; y < height; ++y)
                        {
                            const uint8_t *src = scan0 + static_cast<ptrdiff_t>(pitch) * static_cast<ptrdiff_t>(y);
                            uint8_t *dst = picture.rgba.data() + static_cast<size_t>(y) * width * 4u;
                            for (uint32_t x = 0; x < width; ++x, src += 4, dst += 4)
                            {
                                dst[0] = src[2]; // BGRX -> RGBA
                                dst[1] = src[1];
                                dst[2] = src[0];
                                dst[3] = 255;
                            }
                        }
                        if (locked2d)
                            buffer2d->Unlock2D();
                        else
                            buffer->Unlock();
                    }
                    release(buffer2d);
                    release(buffer);
                }
                release(sample);
                std::lock_guard<std::mutex> lock(mutex);
                ready.push_back(std::move(picture));
            }
            CoUninitialize();
        }
    };

    Video::~Video()
    {
        if (m_impl)
        {
            {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                m_impl->stop = true;
            }
            m_impl->cv.notify_all();
            if (m_impl->worker.joinable())
                m_impl->worker.join();
            release(m_impl->reader);
        }
        MFShutdown();
    }

    std::unique_ptr<Video> Video::open(const std::string &path)
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        (void)com; // RPC_E_CHANGED_MODE is fine: COM is already set up on this thread
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        {
            std::fprintf(stderr, "[intro] Media Foundation is not available, no studio intro\n");
            return nullptr;
        }
        std::unique_ptr<Video> video(new Video());
        video->m_impl = std::make_unique<Impl>();
        Impl &impl = *video->m_impl;
        const std::wstring wpath = widen(std::filesystem::absolute(path).string());

        // Pictures: 32-bit RGB through Media Foundation's video processor.
        impl.reader = openReader(wpath, true);
        if (!impl.reader)
        {
            std::fprintf(stderr, "[intro] cannot open %s\n", path.c_str());
            return nullptr;
        }
        impl.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        impl.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        IMFMediaType *type = nullptr;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        HRESULT hr = impl.reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type);
        release(type);
        IMFMediaType *current = nullptr;
        if (FAILED(hr) || FAILED(impl.reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current)))
        {
            std::fprintf(stderr, "[intro] %s: no decodable picture stream\n", path.c_str());
            return nullptr;
        }
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 stride = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)))
            impl.stride = static_cast<int32_t>(stride);
        release(current);
        if (w == 0 || h == 0)
            return nullptr;
        impl.width = video->m_width = w;
        impl.height = video->m_height = h;
        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(impl.reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)))
            video->m_seconds = static_cast<double>(duration.uhVal.QuadPart) / 1e7;
        PropVariantClear(&duration);

        // Sound: the whole track as 16-bit stereo PCM at 48 kHz (a few MB for a short intro).
        if (IMFSourceReader *audio = openReader(wpath, false))
        {
            audio->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            if (SUCCEEDED(audio->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE)))
            {
                IMFMediaType *pcm = nullptr;
                MFCreateMediaType(&pcm);
                pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
                if (SUCCEEDED(audio->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm)))
                {
                    for (;;)
                    {
                        DWORD stream = 0, flags = 0;
                        LONGLONG timestamp = 0;
                        IMFSample *sample = nullptr;
                        if (FAILED(audio->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &stream, &flags,
                                                     &timestamp, &sample)) ||
                            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
                        {
                            release(sample);
                            break;
                        }
                        if (!sample)
                            continue;
                        IMFMediaBuffer *buffer = nullptr;
                        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
                        {
                            BYTE *data = nullptr;
                            DWORD maxLength = 0, length = 0;
                            if (SUCCEEDED(buffer->Lock(&data, &maxLength, &length)))
                            {
                                const int16_t *samples = reinterpret_cast<const int16_t *>(data);
                                video->m_audio.insert(video->m_audio.end(), samples, samples + length / 2u);
                                buffer->Unlock();
                            }
                            release(buffer);
                        }
                        release(sample);
                    }
                }
                release(pcm);
            }
            release(audio);
        }
        std::fprintf(stderr, "[intro] %s: %ux%u, %.1f s, sound %.1f s\n", path.c_str(), w, h, video->m_seconds,
                     static_cast<double>(video->m_audio.size()) / 2.0 / 48000.0);
        impl.worker = std::thread([&impl]() { impl.run(); });
        return video;
    }

    bool Video::pictureAt(double time, std::vector<uint8_t> &rgba, bool &finished)
    {
        Impl &impl = *m_impl;
        bool got = false;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            while (!impl.ready.empty() && impl.ready.front().time <= time)
            {
                // The caller's previous buffer (or a skipped picture's) goes back to the worker to be refilled.
                rgba.swap(impl.ready.front().rgba);
                if (impl.ready.front().rgba.capacity() != 0u && impl.spare.size() < 4u)
                    impl.spare.push_back(std::move(impl.ready.front().rgba));
                impl.ready.pop_front();
                got = true;
            }
            finished = impl.ended && impl.ready.empty();
        }
        if (got)
            impl.cv.notify_all();
        return got;
    }
#else
    struct Video::Impl
    {
    };
    Video::~Video() = default;
    std::unique_ptr<Video> Video::open(const std::string &) { return nullptr; }
    bool Video::pictureAt(double, std::vector<uint8_t> &, bool &finished)
    {
        finished = true;
        return false;
    }
#endif
}
