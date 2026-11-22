#include "d3d9_present_recorder.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <vector>

#include <windows.h>

#include "d3d9_device.h"
#include "d3d9_readback.h"
#include "util/logging.h"

namespace d3d9_present_recorder {

namespace {

constexpr char MAGIC[4] = {'S', 'P', 'R', '2'};
constexpr std::streamoff COUNT_OFFSET = sizeof(MAGIC);

struct Recording {
    int remaining = 0;
    int divide = 1;
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<int64_t> stamps;
    std::vector<uint8_t> row;
    std::ofstream out;
};

std::mutex RECORDING_M;
Recording RECORDING;

std::string part_path(const Recording &recording) {
    return recording.path + ".part";
}

void open_recording(Recording &recording) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const uint32_t frames = 0;
    recording.out.open(part_path(recording), std::ios::binary | std::ios::trunc);
    recording.out.write(MAGIC, sizeof(MAGIC));
    recording.out.write(reinterpret_cast<const char *>(&frames), sizeof(frames));
    recording.out.write(reinterpret_cast<const char *>(&recording.width), sizeof(recording.width));
    recording.out.write(reinterpret_cast<const char *>(&recording.height), sizeof(recording.height));
    recording.out.write(reinterpret_cast<const char *>(&frequency.QuadPart), sizeof(frequency.QuadPart));
}

void finish_recording(Recording &recording) {
    const auto frames = static_cast<uint32_t>(recording.stamps.size());
    recording.out.write(reinterpret_cast<const char *>(recording.stamps.data()),
            static_cast<std::streamsize>(recording.stamps.size() * sizeof(int64_t)));
    recording.out.seekp(COUNT_OFFSET);
    recording.out.write(reinterpret_cast<const char *>(&frames), sizeof(frames));
    recording.out.close();
    MoveFileExA(part_path(recording).c_str(), recording.path.c_str(), MOVEFILE_REPLACE_EXISTING);
    log_info("graphics::d3d9", "recorded {} presented frames to {}", frames, recording.path);
}

bool append_frame(Recording &recording, const d3d9_readback::BackbufferCopy &copy, int64_t stamp) {
    if (copy.desc.Format != D3DFMT_X8R8G8B8 && copy.desc.Format != D3DFMT_A8R8G8B8) {
        log_warning("graphics::d3d9", "present recorder only reads 32-bit back buffers");
        return false;
    }
    const uint32_t width = copy.desc.Width / recording.divide;
    const uint32_t height = copy.desc.Height / recording.divide;
    if (recording.stamps.empty()) {
        recording.width = width;
        recording.height = height;
        recording.row.resize(width);
        open_recording(recording);
    } else if (width != recording.width || height != recording.height) {
        return false;
    }
    D3DLOCKED_RECT locked {};
    if (FAILED(copy.surface->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        return false;
    }
    const auto *base = static_cast<const uint8_t *>(locked.pBits);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = base + static_cast<size_t>(y) * recording.divide * locked.Pitch;
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *pixel = row + static_cast<size_t>(x) * recording.divide * 4;
            recording.row[x] = static_cast<uint8_t>((pixel[2] * 77 + pixel[1] * 150 + pixel[0] * 29) >> 8);
        }
        recording.out.write(reinterpret_cast<const char *>(recording.row.data()),
                static_cast<std::streamsize>(recording.row.size()));
    }
    copy.surface->UnlockRect();
    recording.stamps.push_back(stamp);
    return true;
}

}

void start(int frames, int divide, std::string path) {
    std::lock_guard<std::mutex> lock(RECORDING_M);
    RECORDING = Recording {};
    RECORDING.remaining = frames;
    RECORDING.divide = std::max(1, divide);
    RECORDING.path = std::move(path);
}

void on_present(IDirect3DDevice9 *device, WrappedIDirect3DDevice9 *wrapped_device) {
    std::lock_guard<std::mutex> lock(RECORDING_M);
    if (RECORDING.remaining <= 0) {
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    IDirect3DSwapChain9 *swap_chain = nullptr;
    if (FAILED(wrapped_device->get_screenshot_swap_chain(0, &swap_chain)) || swap_chain == nullptr) {
        return;
    }
    auto copy = d3d9_readback::acquire_backbuffer_copy(device, swap_chain, 0, true);
    swap_chain->Release();
    if (!copy.has_value() || !append_frame(RECORDING, *copy, now.QuadPart)) {
        return;
    }
    if (--RECORDING.remaining == 0) {
        finish_recording(RECORDING);
        RECORDING = Recording {};
    }
}

}
