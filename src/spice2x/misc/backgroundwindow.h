#pragma once

#include <windows.h>

namespace backgroundwindow {

    extern bool ENABLED;

    void hide_console();
    void hook();
    void allow_topmost(HWND hWnd);
}
