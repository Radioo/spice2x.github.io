#include "capture.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include "api/capture_pump.h"
#include "api/stream_format.h"
#include "api/stream_server.h"
#include "external/rapidjson/document.h"
#include "hooks/graphics/graphics.h"
#include "hooks/graphics/jpeg_encoder.h"
#include "util/crypt.h"
#include "util/time.h"

using namespace std::placeholders;
using namespace rapidjson;

namespace api::modules {

    std::optional<uint32_t> CAPTURE_QUALITY;
    std::optional<uint32_t> CAPTURE_DIVIDE;

    static thread_local std::vector<uint8_t> CAPTURE_BUFFER;

    struct CachedFrame {
        std::vector<uint8_t> jpeg;
        uint64_t timestamp = 0;
        int width = 0;
        int height = 0;
    };

    static std::mutex FRAME_CACHE_M;
    static std::unordered_map<int, CachedFrame> FRAME_CACHE;

    static void add_jpeg_response(
            int screen,
            uint64_t timestamp,
            int width,
            int height,
            const std::vector<uint8_t> &jpeg,
            Response &res) {

        auto encoded = crypt::base64_encode(jpeg.data(), jpeg.size());

        Value data;
        data.SetString(encoded.c_str(), encoded.length(), res.doc()->GetAllocator());
        res.add_data(timestamp);
        res.add_data(width);
        res.add_data(height);
        res.add_data(data);

        std::lock_guard<std::mutex> lock(FRAME_CACHE_M);
        FRAME_CACHE[screen] = {jpeg, timestamp, width, height};
    }

    static bool try_cached_response(int screen, Response &res) {
        std::lock_guard<std::mutex> lock(FRAME_CACHE_M);
        const auto pos = FRAME_CACHE.find(screen);
        if (pos == FRAME_CACHE.end() || pos->second.jpeg.empty()) {
            return false;
        }

        const auto &cached = pos->second;
        auto encoded = crypt::base64_encode(cached.jpeg.data(), cached.jpeg.size());

        Value data;
        data.SetString(encoded.c_str(), encoded.length(), res.doc()->GetAllocator());
        res.add_data(cached.timestamp);
        res.add_data(cached.width);
        res.add_data(cached.height);
        res.add_data(data);
        return true;
    }

    static BOOL CALLBACK find_largest_process_window(HWND hwnd, LPARAM param) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        const bool overlay = (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || overlay) {
            return TRUE;
        }

        RECT rect {};
        GetClientRect(hwnd, &rect);
        auto best = reinterpret_cast<std::pair<HWND, LONG> *>(param);
        const LONG area = rect.right * rect.bottom;
        if (area > best->second) {
            *best = { hwnd, area };
        }
        return TRUE;
    }

    static bool capture_gdi_window(std::shared_ptr<uint8_t[]> &out, int divide, int *width, int *height) {
        std::pair<HWND, LONG> best { nullptr, 0 };
        EnumWindows(find_largest_process_window, reinterpret_cast<LPARAM>(&best));
        if (best.first == nullptr) {
            return false;
        }

        RECT rect {};
        GetClientRect(best.first, &rect);
        const int source_width = rect.right;
        const int source_height = rect.bottom;

        HDC window_dc = GetDC(best.first);
        HDC memory_dc = CreateCompatibleDC(window_dc);
        HBITMAP bitmap = CreateCompatibleBitmap(window_dc, source_width, source_height);
        HGDIOBJ previous = SelectObject(memory_dc, bitmap);
        const bool copied_window = BitBlt(memory_dc, 0, 0, source_width, source_height, window_dc, 0, 0, SRCCOPY) != FALSE;
        SelectObject(memory_dc, previous);

        BITMAPINFOHEADER info {};
        info.biSize = sizeof(info);
        info.biWidth = source_width;
        info.biHeight = -source_height;
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        std::vector<uint8_t> bgrx(static_cast<size_t>(source_width) * source_height * 4);
        const bool copied = copied_window && GetDIBits(memory_dc, bitmap, 0, source_height, bgrx.data(),
                reinterpret_cast<BITMAPINFO *>(&info), DIB_RGB_COLORS) == source_height;

        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(best.first, window_dc);
        if (!copied) {
            return false;
        }

        const int step = divide > 1 ? divide : 1;
        const int target_width = (source_width + step - 1) / step;
        const int target_height = (source_height + step - 1) / step;
        out = std::shared_ptr<uint8_t[]>(new uint8_t[static_cast<size_t>(target_width) * target_height * 3]);
        for (int y = 0; y < target_height; y++) {
            for (int x = 0; x < target_width; x++) {
                const uint8_t *source = &bgrx[(static_cast<size_t>(y * step) * source_width + x * step) * 4];
                uint8_t *target = &out[(static_cast<size_t>(y) * target_width + x) * 3];
                target[0] = source[2];
                target[1] = source[1];
                target[2] = source[0];
            }
        }

        *width = target_width;
        *height = target_height;
        return true;
    }

    Capture::Capture() : Module("capture") {
        functions["get_screens"] = std::bind(&Capture::get_screens, this, _1, _2);
        functions["get_jpg"] = std::bind(&Capture::get_jpg, this, _1, _2);
        functions["get_streams"] = std::bind(&Capture::get_streams, this, _1, _2);
    }

    /**
     * get_screens()
     */
    void Capture::get_screens(Request &req, Response &res) {

        // aquire screens
        std::vector<int> screens;
        graphics_screens_get(screens);

        // add screens to response
        for (auto &screen : screens) {
            res.add_data(screen);
        }
    }

    /**
     * get_jpg([screen=0, quality=70, downscale=0, divide=1])
     * screen: uint specifying the window
     * quality: uint in range [0, 100]
     * reduce: uint for dividing image size
     */
    void Capture::get_jpg(Request &req, Response &res) {

        // settings
        int screen = 0;
        int quality = 70;
        int divide = 1;
        if (req.params.Size() > 0 && req.params[0].IsUint()) {
            screen = req.params[0].GetUint();
        }

        if (CAPTURE_QUALITY.has_value()) {
            quality = CAPTURE_QUALITY.value();
        } else if (req.params.Size() > 1 && req.params[1].IsUint()) {
            quality = req.params[1].GetUint();
        }

        if (CAPTURE_DIVIDE.has_value()) {
            divide = CAPTURE_DIVIDE.value();
        } else if (req.params.Size() > 2 && req.params[2].IsUint()) {
            divide = req.params[2].GetUint();
        }

        // receive JPEG data
        uint64_t timestamp = 0;
        int width = 0;
        int height = 0;

        std::shared_ptr<uint8_t[]> pixels;
        bool success = capture_pump::capture_direct(
                screen, pixels, divide, &timestamp, &width, &height);

        if (!success && screen == 0 && !graphics_capture_last_size(screen, nullptr, nullptr)) {
            success = capture_gdi_window(pixels, divide, &width, &height);
            timestamp = static_cast<uint64_t>(get_performance_milliseconds());
        }

        if (success) {
            CAPTURE_BUFFER.clear();
            success = jpeg_encoder::encode(
                    CAPTURE_BUFFER, pixels.get(), width, height, quality);
        }

        if (success) {
            add_jpeg_response(screen, timestamp, width, height, CAPTURE_BUFFER, res);
            CAPTURE_BUFFER.clear();
            return;
        }

        // fall back to the last successful frame while the game is busy loading
        CAPTURE_BUFFER.clear();
        try_cached_response(screen, res);
    }

    /**
     * get_streams()
     */
    void Capture::get_streams(Request &req, Response &res) {

        auto &alloc = res.doc()->GetAllocator();

        // nothing is listening without -apistream, so there is no stream to describe
        const unsigned short port = stream_server_port();
        if (port == 0) {
            return;
        }

        Value formats(kArrayType);
        for (const auto &[name, path] : stream_formats()) {
            Value entry(kObjectType);
            entry.AddMember("name", Value(name.c_str(), alloc), alloc);
            entry.AddMember("path", Value(path.c_str(), alloc), alloc);
            formats.PushBack(entry, alloc);
        }

        std::vector<int> screen_numbers;
        graphics_screens_get(screen_numbers);

        // measuring a screen nobody has captured yet waits for the game to present, which can
        // take as long as the whole request is allowed, so only one screen is measured per
        // call and the rest are reported null until a later one settles them. which screen
        // gets the attempt rotates, otherwise one that never presents would take every
        // request and the screens behind it would stay unmeasured forever
        int probe_screen = -1;
        {
            std::vector<int> unmeasured;
            for (const auto screen : screen_numbers) {
                if (screen < static_cast<int>(GRAPHICS_CAPTURE_SCREEN_NO)
                        && !graphics_capture_last_size(screen, nullptr, nullptr)
                        && !capture_pump::screen_claimed(screen)) {
                    unmeasured.push_back(screen);
                }
            }

            if (!unmeasured.empty()) {
                static std::atomic<unsigned> probe_cursor { 0 };
                probe_screen = unmeasured[probe_cursor.fetch_add(1) % unmeasured.size()];
            }
        }

        Value screens(kArrayType);
        for (const auto screen : screen_numbers) {
            if (screen >= static_cast<int>(GRAPHICS_CAPTURE_SCREEN_NO)) {
                continue;
            }

            int width = 0;
            int height = 0;
            bool known = graphics_capture_last_size(screen, &width, &height);

            // a probe holds the screen for as long as it waits, so a second caller arriving
            // during one would queue behind it and then take a wait of its own; let it report
            // the screen as unmeasured instead and pick the size up once the first is done
            static std::atomic<bool> probe_running { false };
            if (!known && screen == probe_screen && !probe_running.exchange(true)) {
                std::shared_ptr<uint8_t[]> pixels;
                known = capture_pump::capture_direct(
                        screen, pixels, 1, nullptr, &width, &height);
                probe_running = false;
            }

            // a screen of unknown size cannot be described, and a client told about it could
            // not size its decoder anyway; leaving it out until it has been measured beats
            // handing over an entry that has to be treated as absent
            if (!known) {
                continue;
            }

            Value entry(kObjectType);
            entry.AddMember("screen", screen, alloc);
            entry.AddMember("width", width, alloc);
            entry.AddMember("height", height, alloc);

            // a screen carries one viewer at a time, so this is what decides whether a client
            // can connect at all; still racy by the time it does, only more honest than not
            entry.AddMember("busy", capture_pump::screen_claimed(screen), alloc);

            screens.PushBack(entry, alloc);
        }

        Value info(kObjectType);
        info.AddMember("port", port, alloc);
        info.AddMember("formats", formats, alloc);
        info.AddMember("screens", screens, alloc);

        res.add_data(info);
    }
}
