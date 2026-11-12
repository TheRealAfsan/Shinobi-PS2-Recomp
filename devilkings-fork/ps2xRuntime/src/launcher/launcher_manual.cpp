// LOCAL PATCH (Shinobi launcher): the game manual, from the player's own PDF.
//
// The manual is SEGA's, like the game, so the launcher ships none of it: the player chooses their own PDF once, and
// its pages are drawn to PNG files in the game folder (game/manual) with Windows' own PDF renderer (Windows.Data.Pdf),
// which the page then turns like a book (StPageFlip, launcher_assets/page-flip.browser.js).

#include "launcher_manual.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Data.Pdf.h>

#include <cstdio>
#include <string>

namespace fs = std::filesystem;

namespace launcher::manual
{
    namespace
    {
        std::string narrowText(const winrt::hstring &text)
        {
            return winrt::to_string(text);
        }

        std::string pageName(uint32_t index)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "page_%03u.png", index + 1u);
            return name;
        }
    }

    uint32_t cachedPages(const fs::path &folder)
    {
        std::error_code ec;
        uint32_t count = 0u;
        while (fs::exists(folder / pageName(count), ec))
            ++count;
        return count;
    }

    std::string render(const fs::path &pdf, const fs::path &folder, uint32_t height,
                       const std::function<void(uint32_t, uint32_t)> &progress)
    {
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        }
        catch (...)
        {
            // Already initialised on this thread.
        }
        try
        {
            using namespace winrt::Windows::Data::Pdf;
            using namespace winrt::Windows::Storage;
            std::error_code ec;
            fs::create_directories(folder, ec);
            // Old pages go first, so a shorter manual leaves none of the previous one behind.
            for (uint32_t index = 0u; fs::exists(folder / pageName(index), ec); ++index)
                fs::remove(folder / pageName(index), ec);

            const StorageFile file = StorageFile::GetFileFromPathAsync(winrt::hstring(fs::absolute(pdf).wstring())).get();
            const PdfDocument document = PdfDocument::LoadFromFileAsync(file).get();
            const StorageFolder out = StorageFolder::GetFolderFromPathAsync(winrt::hstring(fs::absolute(folder).wstring())).get();
            const uint32_t count = document.PageCount();
            if (count == 0u)
                return "That PDF has no pages.";
            // A scanned manual usually has two printed pages side by side on each PDF page (a spread): those are drawn as
            // their left and right halves, so the book turns the manual's own pages one at a time.
            uint32_t written = 0u;
            auto draw = [&](const PdfPage &page, const winrt::Windows::Foundation::Rect *half) {
                PdfPageRenderOptions options;
                options.DestinationHeight(height); // the width follows the (half) page's own shape
                if (half)
                    options.SourceRect(*half);
                const StorageFile image = out.CreateFileAsync(winrt::hstring(fs::path(pageName(written)).wstring()),
                                                              CreationCollisionOption::ReplaceExisting).get();
                const auto stream = image.OpenAsync(FileAccessMode::ReadWrite).get();
                page.RenderToStreamAsync(stream, options).get(); // PNG by default
                stream.Close();
                ++written;
            };
            for (uint32_t index = 0u; index < count; ++index)
            {
                const PdfPage page = document.GetPage(index);
                const auto size = page.Size();
                if (size.Width > size.Height * 1.15f)
                {
                    const winrt::Windows::Foundation::Rect left{0.0f, 0.0f, size.Width / 2.0f, size.Height};
                    const winrt::Windows::Foundation::Rect right{size.Width / 2.0f, 0.0f, size.Width / 2.0f, size.Height};
                    draw(page, &left);
                    draw(page, &right);
                }
                else
                    draw(page, nullptr);
                page.Close();
                if (progress)
                    progress(index + 1u, count);
            }
            return std::string();
        }
        catch (const winrt::hresult_error &error)
        {
            return "The manual could not be read: " + narrowText(error.message());
        }
        catch (...)
        {
            return "The manual could not be read.";
        }
    }
}
