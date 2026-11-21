#include "backgroundwindow.h"

#include <mutex>
#include <thread>
#include <unordered_set>

#include <windows.h>

#include "util/detour.h"
#include "util/logging.h"

namespace backgroundwindow {

    bool ENABLED = false;

    static decltype(SetForegroundWindow) *SetForegroundWindow_orig = nullptr;
    static decltype(BringWindowToTop) *BringWindowToTop_orig = nullptr;
    static decltype(ShowWindow) *ShowWindow_orig = nullptr;
    static decltype(SetWindowPos) *SetWindowPos_orig = nullptr;
    static decltype(CreateWindowExW) *CreateWindowExW_orig = nullptr;
    static decltype(CreateWindowExA) *CreateWindowExA_orig = nullptr;
    static decltype(AttachThreadInput) *AttachThreadInput_orig = nullptr;
    static decltype(SystemParametersInfoA) *SystemParametersInfoA_orig = nullptr;
    static decltype(SystemParametersInfoW) *SystemParametersInfoW_orig = nullptr;

    static std::mutex topmost_mutex;
    static std::unordered_set<HWND> topmost_windows;

    static bool kept_on_top(HWND hWnd) {
        std::lock_guard lock(topmost_mutex);
        return topmost_windows.contains(hWnd);
    }

    void allow_topmost(HWND hWnd) {
        std::lock_guard lock(topmost_mutex);
        topmost_windows.insert(hWnd);
    }

    static void keep_behind(HWND hWnd, DWORD style) {
        if (hWnd == nullptr || (style & WS_CHILD)) {
            return;
        }

        if (style & WS_VISIBLE) {
            if (ShowWindow_orig != nullptr) {
                ShowWindow_orig(hWnd, SW_SHOWNOACTIVATE);
            } else {
                ShowWindow(hWnd, SW_SHOWNOACTIVATE);
            }
        }

        if (kept_on_top(hWnd)) {
            return;
        }

        const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (SetWindowPos_orig != nullptr) {
            SetWindowPos_orig(hWnd, HWND_BOTTOM, 0, 0, 0, 0, flags);
        } else {
            SetWindowPos(hWnd, HWND_BOTTOM, 0, 0, 0, 0, flags);
        }
    }

    static bool owns(HWND parent, DWORD style) {
        return parent == nullptr && !(style & WS_CHILD);
    }

    static const DWORD WATCH_INTERVAL_MS = 250;

    static bool ours(HWND window) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);

        return process == GetCurrentProcessId();
    }

    static HWND window_to_raise(HWND ours_window) {
        for (auto next = GetWindow(ours_window, GW_HWNDNEXT);
                next != nullptr;
                next = GetWindow(next, GW_HWNDNEXT)) {
            if (IsWindowVisible(next) && !ours(next) && GetWindowTextLengthW(next) > 0) {
                return next;
            }
        }

        return nullptr;
    }

    static void give_up_foreground() {
        auto foreground = GetForegroundWindow();
        if (foreground == nullptr || !ours(foreground)) {
            return;
        }

        auto next = window_to_raise(foreground);
        if (next != nullptr && SetForegroundWindow_orig != nullptr) {
            SetForegroundWindow_orig(next);
        }

        keep_behind(foreground, 0);
    }

    static void watch() {
        while (true) {
            Sleep(WATCH_INTERVAL_MS);
            give_up_foreground();
        }
    }

    static BOOL WINAPI SetForegroundWindow_hook(HWND) {
        return TRUE;
    }

    static BOOL WINAPI BringWindowToTop_hook(HWND) {
        return TRUE;
    }

    static BOOL WINAPI ShowWindow_hook(HWND hWnd, int nCmdShow) {
        switch (nCmdShow) {
            case SW_SHOW:
            case SW_SHOWNORMAL:
            case SW_SHOWDEFAULT:
            case SW_RESTORE:
                nCmdShow = SW_SHOWNOACTIVATE;
                break;
            default:
                break;
        }

        return ShowWindow_orig(hWnd, nCmdShow);
    }

    static BOOL WINAPI SetWindowPos_hook(HWND hWnd, HWND hWndInsertAfter,
            int X, int Y, int cx, int cy, UINT uFlags) {

        const bool topmost_allowed = hWndInsertAfter == HWND_TOPMOST && kept_on_top(hWnd);
        if (!(uFlags & SWP_NOZORDER) && !topmost_allowed
                && (hWndInsertAfter == HWND_TOP || hWndInsertAfter == HWND_TOPMOST)) {
            hWndInsertAfter = HWND_BOTTOM;
        }

        return SetWindowPos_orig(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags | SWP_NOACTIVATE);
    }

    static DWORD process_of_thread(DWORD thread_id) {
        using GetProcessIdOfThread_t = DWORD (WINAPI *)(HANDLE);
        static const auto get_process_id_of_thread = reinterpret_cast<GetProcessIdOfThread_t>(
                GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcessIdOfThread"));
        if (get_process_id_of_thread == nullptr) {
            return GetCurrentProcessId();
        }
        auto thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, thread_id);
        if (thread == nullptr) {
            return 0;
        }
        const auto process = get_process_id_of_thread(thread);
        CloseHandle(thread);
        return process;
    }

    static BOOL WINAPI AttachThreadInput_hook(DWORD idAttach, DWORD idAttachTo, BOOL fAttach) {
        const auto current = GetCurrentProcessId();
        if (process_of_thread(idAttach) != current || process_of_thread(idAttachTo) != current) {
            log_misc("backgroundwindow", "refused AttachThreadInput({}, {}, {}) across processes",
                    idAttach, idAttachTo, fAttach);
            return TRUE;
        }
        return AttachThreadInput_orig(idAttach, idAttachTo, fAttach);
    }

    static BOOL WINAPI SystemParametersInfoA_hook(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) {
        if (uiAction == SPI_SETFOREGROUNDLOCKTIMEOUT) {
            log_misc("backgroundwindow", "ignored SPI_SETFOREGROUNDLOCKTIMEOUT");
            return TRUE;
        }
        return SystemParametersInfoA_orig(uiAction, uiParam, pvParam, fWinIni);
    }

    static BOOL WINAPI SystemParametersInfoW_hook(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) {
        if (uiAction == SPI_SETFOREGROUNDLOCKTIMEOUT) {
            log_misc("backgroundwindow", "ignored SPI_SETFOREGROUNDLOCKTIMEOUT");
            return TRUE;
        }
        return SystemParametersInfoW_orig(uiAction, uiParam, pvParam, fWinIni);
    }

    static HWND WINAPI CreateWindowExW_hook(DWORD dwExStyle, LPCWSTR lpClassName,
            LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
            HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) {

        const bool top_level = owns(hWndParent, dwStyle);
        const DWORD requested = dwStyle;
        if (top_level) {
            dwStyle &= ~WS_VISIBLE;
        }

        auto window = CreateWindowExW_orig(dwExStyle, lpClassName, lpWindowName, dwStyle,
                X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

        if (top_level) {
            keep_behind(window, requested);
        }

        return window;
    }

    static HWND WINAPI CreateWindowExA_hook(DWORD dwExStyle, LPCSTR lpClassName,
            LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
            HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) {

        const bool top_level = owns(hWndParent, dwStyle);
        const DWORD requested = dwStyle;
        if (top_level) {
            dwStyle &= ~WS_VISIBLE;
        }

        auto window = CreateWindowExA_orig(dwExStyle, lpClassName, lpWindowName, dwStyle,
                X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

        if (top_level) {
            keep_behind(window, requested);
        }

        return window;
    }

    void hide_console() {
        if (!ENABLED) {
            return;
        }

        auto console = GetConsoleWindow();
        if (console != nullptr) {
            ShowWindow(console, SW_HIDE);
        }
    }

    void hook() {
        if (!ENABLED) {
            return;
        }

        detour::trampoline_try("user32.dll", "SetForegroundWindow",
                SetForegroundWindow_hook, &SetForegroundWindow_orig);
        detour::trampoline_try("user32.dll", "BringWindowToTop",
                BringWindowToTop_hook, &BringWindowToTop_orig);
        detour::trampoline_try("user32.dll", "ShowWindow",
                ShowWindow_hook, &ShowWindow_orig);
        detour::trampoline_try("user32.dll", "SetWindowPos",
                SetWindowPos_hook, &SetWindowPos_orig);
        detour::trampoline_try("user32.dll", "CreateWindowExW",
                CreateWindowExW_hook, &CreateWindowExW_orig);
        detour::trampoline_try("user32.dll", "CreateWindowExA",
                CreateWindowExA_hook, &CreateWindowExA_orig);
        detour::trampoline_try("user32.dll", "AttachThreadInput",
                AttachThreadInput_hook, &AttachThreadInput_orig);
        detour::trampoline_try("user32.dll", "SystemParametersInfoA",
                SystemParametersInfoA_hook, &SystemParametersInfoA_orig);
        detour::trampoline_try("user32.dll", "SystemParametersInfoW",
                SystemParametersInfoW_hook, &SystemParametersInfoW_orig);

        std::thread(watch).detach();

        log_info("backgroundwindow", "the game window will not raise or focus itself");
    }
}
