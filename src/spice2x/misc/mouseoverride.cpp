#include "mouseoverride.h"

#include <mutex>

#include "hooks/graphics/graphics.h"
#include "util/detour.h"
#include "util/execexe.h"
#include "util/logging.h"

namespace mouseoverride {

    static const char *CURSOR_MODULE = "UnityPlayer.dll";

    static std::mutex STATE_M;
    static bool ACTIVE = false;
    static bool PRESSED = false;
    static bool DRIVEN = false;
    static POINT POSITION {};

    static decltype(GetCursorPos) *GetCursorPos_orig = nullptr;
    static decltype(GetFocus) *GetFocus_orig = nullptr;
    static decltype(GetActiveWindow) *GetActiveWindow_orig = nullptr;
    static bool HOOK_FAILED = false;

    static HWND game_window() {
        return graphics_game_window();
    }

    static bool driven() {
        std::lock_guard<std::mutex> lock(STATE_M);
        return DRIVEN;
    }

    static HWND WINAPI GetFocus_hook() {
        auto window = game_window();
        if (driven() && window != nullptr) {
            return window;
        }
        return GetFocus_orig();
    }

    static HWND WINAPI GetActiveWindow_hook() {
        auto window = game_window();
        if (driven() && window != nullptr) {
            return window;
        }
        return GetActiveWindow_orig();
    }

    static BOOL WINAPI GetCursorPos_hook(LPPOINT lpPoint) {
        POINT position {};
        bool active;
        {
            std::lock_guard<std::mutex> lock(STATE_M);
            active = ACTIVE;
            position = POSITION;
        }

        auto window = game_window();
        if (!active || lpPoint == nullptr || window == nullptr) {
            return GetCursorPos_orig(lpPoint);
        }

        ClientToScreen(window, &position);
        *lpPoint = position;
        return TRUE;
    }

    static HMODULE cursor_module() {
        auto module = GetModuleHandleA(CURSOR_MODULE);
        if (module != nullptr) {
            return module;
        }

        if (execexe::is_initialized()) {
            return execexe::get_module(CURSOR_MODULE, false);
        }

        return nullptr;
    }

    static bool hook() {
        if (GetCursorPos_orig != nullptr) {
            return true;
        }
        if (HOOK_FAILED) {
            return false;
        }

        auto module = cursor_module();
        if (module == nullptr) {
            log_warning("mouseoverride", "no {} in this process", CURSOR_MODULE);
            HOOK_FAILED = true;
            return false;
        }

        GetCursorPos_orig = detour::iat_try("GetCursorPos", GetCursorPos_hook, module);
        if (GetCursorPos_orig == nullptr) {
            log_warning("mouseoverride", "found {} at {} but could not hook GetCursorPos in it",
                    CURSOR_MODULE, fmt::ptr(module));
            HOOK_FAILED = true;
            return false;
        }

        log_info("mouseoverride", "hooked GetCursorPos in {}", CURSOR_MODULE);

        GetFocus_orig = detour::iat_try("GetFocus", GetFocus_hook, module);
        GetActiveWindow_orig = detour::iat_try("GetActiveWindow", GetActiveWindow_hook, module);
        if (GetFocus_orig == nullptr || GetActiveWindow_orig == nullptr) {
            log_warning("mouseoverride", "could not hook GetFocus/GetActiveWindow in {}, "
                    "UI that ignores input without focus will not see clicks", CURSOR_MODULE);
        }

        return true;
    }

    static void post_raw(UINT message, WPARAM wparam, LPARAM lparam) {
        auto window = game_window();
        if (window == nullptr) {
            return;
        }
        PostMessageW(window, message, wparam, lparam);
    }

    static void post(UINT message, WPARAM buttons, POINT position) {
        post_raw(message, buttons, MAKELPARAM(position.x, position.y));
    }

    static void announce_focus() {
        post_raw(WM_ACTIVATEAPP, TRUE, 0);
        post_raw(WM_ACTIVATE, WA_ACTIVE, 0);
        post_raw(WM_SETFOCUS, 0, 0);
    }

    bool available() {
        return hook() && game_window() != nullptr;
    }

    void write(int32_t x, int32_t y, bool pressed) {
        bool was_pressed;
        POINT position { x, y };
        {
            std::lock_guard<std::mutex> lock(STATE_M);
            was_pressed = ACTIVE && PRESSED;
            ACTIVE = true;
            DRIVEN = true;
            PRESSED = pressed;
            POSITION = position;
        }

        if (pressed && !was_pressed) {
            announce_focus();
        }

        post(WM_MOUSEMOVE, pressed ? MK_LBUTTON : 0, position);
        if (pressed && !was_pressed) {
            post(WM_LBUTTONDOWN, MK_LBUTTON, position);
        } else if (!pressed && was_pressed) {
            post(WM_LBUTTONUP, 0, position);
        }
    }

    void write_reset() {
        bool was_pressed;
        POINT position {};
        {
            std::lock_guard<std::mutex> lock(STATE_M);
            was_pressed = ACTIVE && PRESSED;
            position = POSITION;
            ACTIVE = false;
            PRESSED = false;
        }

        if (was_pressed) {
            post(WM_LBUTTONUP, 0, position);
        }
    }

    bool canvas(SIZE *size) {
        auto window = game_window();
        if (window == nullptr) {
            return false;
        }

        RECT client {};
        if (!GetClientRect(window, &client)) {
            return false;
        }

        size->cx = client.right - client.left;
        size->cy = client.bottom - client.top;
        return true;
    }

    bool read(POINT *position, bool *pressed) {
        std::lock_guard<std::mutex> lock(STATE_M);
        if (!ACTIVE) {
            return false;
        }
        *position = POSITION;
        *pressed = PRESSED;
        return true;
    }
}
