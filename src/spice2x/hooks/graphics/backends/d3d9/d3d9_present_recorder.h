#pragma once

#include <string>

#include <d3d9.h>

class WrappedIDirect3DDevice9;

namespace d3d9_present_recorder {

    void start(int frames, int divide, std::string path);
    void on_present(IDirect3DDevice9 *device, WrappedIDirect3DDevice9 *wrapped_device);
}
