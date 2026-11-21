#include "cardconnect.h"

#include <filesystem>
#include <format>

#include "acioemu/handle.h"
#include "bi2x_hook.h"
#include "printer.h"
#include "hooks/graphics/graphics.h"
#include "util/execexe.h"
#include "util/fileutils.h"
#include "util/libutils.h"
#include "util/logging.h"
#include "util/unity_player.h"
#include "util/utils.h"

namespace games::cardconnect {
    std::string CARDCONNECT_INJECT_ARGS = "";

    static const std::wstring reader_port = L"\\\\.\\COM2";

    static void (*execexe_ReplacePath)(const char *, const char *) = nullptr;

    static void execexe_ReplacePath_hook(const char *target, const char *pattern) {
        if (pattern != nullptr) {
            const auto host_dir = libutils::module_file_name(nullptr).parent_path();
            std::error_code error;
            if (std::filesystem::equivalent(pattern, host_dir, error)) {
                for (const auto &entry : std::filesystem::directory_iterator(host_dir, error)) {
                    if (!entry.is_directory()) {
                        continue;
                    }
                    const auto path = ws2s(entry.path().wstring());
                    log_info("cardconnect", "keeping {} in place under the application path replacement", path);
                    execexe_ReplacePath(path.c_str(), path.c_str());
                }
            }
        }
        execexe_ReplacePath(target, pattern);
    }

    void CardConnectGame::attach() {
        Game::attach();

        fileutils::dir_create_recursive_log("cardconnect", "dev/raw/log");
        if (!fileutils::bin_write("dev/raw/log/output_log.txt", nullptr, 0)) {
            log_warning("cardconnect", "failed to create Unity output log");
        }

        auto *reader = new acioemu::ACIOHandle(reader_port.c_str(), 1);
        devicehook_init_trampoline();
        devicehook_add(reader);

        execexe::init();
        execexe::init_port_hook(reader_port, reader);
        if (!execexe::trampoline_try("execexe.dll", MAKEINTRESOURCE(36),
                                     execexe_ReplacePath_hook, &execexe_ReplacePath)) {
            log_warning("cardconnect", "failed to hook EXECEXE::ReplacePath");
        }

#if SPICE64
        libutils::load_library("libaio.dll");
        libutils::load_library("libaio-iob.dll");
        libutils::load_library("libaio-iob2_video.dll");
        bi2x_hook_init();
        printer_attach();
#endif

        if (GRAPHICS_SHOW_CURSOR) {
            unity_utils::force_show_cursor(true);
        }

        unity_utils::set_args(
                std::format("{} {}{}",
                            GetCommandLineA(),
                            CARDCONNECT_INJECT_ARGS,
                            unity_utils::get_unity_player_args()));
    }

    void CardConnectGame::detach() {
        Game::detach();
        devicehook_dispose();
    }
}
