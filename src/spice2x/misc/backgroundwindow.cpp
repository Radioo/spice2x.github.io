#include "backgroundwindow.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string_view>
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
    static const DWORD GESTURE_WINDOW_MS = 1000;

    static std::atomic<bool> user_chose_window = false;
    static std::atomic<bool> gesture_toward_game = false;
    static std::atomic<DWORD> gesture_at = 0;
    static std::atomic<HWND> last_other_foreground = nullptr;

    static bool ours(HWND window) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);

        return process == GetCurrentProcessId();
    }

    static bool shell_switcher(HWND root) {
        static const std::array<std::wstring_view, 5> classes {
            L"Shell_TrayWnd",
            L"Shell_SecondaryTrayWnd",
            L"TaskListThumbnailWnd",
            L"XamlExplorerHostIslandWindow",
            L"MultitaskingViewFrame",
        };

        wchar_t name[64] {};
        const auto length = GetClassNameW(root, name, static_cast<int>(std::size(name)));
        const std::wstring_view class_name(name, length > 0 ? length : 0);
        return std::find(classes.begin(), classes.end(), class_name) != classes.end();
    }

    static void record_gesture(bool toward_game) {
        gesture_at = GetTickCount();
        gesture_toward_game = toward_game;
    }

    static bool recent_gesture() {
        return gesture_toward_game && GetTickCount() - gesture_at <= GESTURE_WINDOW_MS;
    }

    static LRESULT CALLBACK mouse_hook(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
                || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN)) {
            const auto *event = reinterpret_cast<const MSLLHOOKSTRUCT *>(data);
            const auto root = GetAncestor(WindowFromPoint(event->pt), GA_ROOT);
            record_gesture(root != nullptr && (ours(root) || shell_switcher(root)));
        }

        return CallNextHookEx(nullptr, code, message, data);
    }

    static LRESULT CALLBACK keyboard_hook(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION) {
            const auto *event = reinterpret_cast<const KBDLLHOOKSTRUCT *>(data);
            const bool alt = event->vkCode == VK_LMENU || event->vkCode == VK_RMENU || event->vkCode == VK_MENU;
            const bool released = message == WM_KEYUP || message == WM_SYSKEYUP;
            if (alt && released) {
                record_gesture(true);
            } else if (!alt && !released) {
                record_gesture(false);
            }
        }

        return CallNextHookEx(nullptr, code, message, data);
    }

    static HWND window_to_raise(HWND ours_window) {
        const HWND previous = last_other_foreground;
        if (previous != nullptr && IsWindow(previous) && IsWindowVisible(previous)) {
            return previous;
        }

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
        if (foreground == nullptr || !ours(foreground) || user_chose_window) {
            return;
        }

        auto next = window_to_raise(foreground);
        if (next != nullptr && SetForegroundWindow_orig != nullptr) {
            SetForegroundWindow_orig(next);
        }

        keep_behind(foreground, 0);
    }

    static void CALLBACK foreground_changed(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
        if (window == nullptr) {
            return;
        }

        if (!ours(window)) {
            user_chose_window = false;
            last_other_foreground = window;
            return;
        }

        if (!user_chose_window && recent_gesture()) {
            user_chose_window = true;
            log_misc("backgroundwindow", "the game window was brought to the front by the user");
        }

        give_up_foreground();
    }

    static void watch() {
        SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, GetModuleHandleW(nullptr), 0);
        SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, GetModuleHandleW(nullptr), 0);
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                foreground_changed, 0, 0, WINEVENT_OUTOFCONTEXT);

        while (true) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, WATCH_INTERVAL_MS, QS_ALLINPUT);

            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

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
        if (!(uFlags & SWP_NOZORDER) && !topmost_allowed && !user_chose_window
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
