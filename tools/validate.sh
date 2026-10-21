#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    # 这组测试只用 stub 链接,靠 -Wl,--gc-sections 丢掉没被调用的 demo_*_enter()。
    # ELF 链接器会连它们引用不到的 LVGL/ui_pixel 符号一起丢掉,PE(MinGW)不会,
    # 于是在 Windows 上必然链接失败。CI 跑在 Linux,所以这里只在类 Unix 上执行。
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            echo "SKIP demo runtime tests: PE linker keeps unreferenced sections"
            ;;
        *)
            for demo in audio low_power ble wifi; do
                "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
                    -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
                    "tests/test_demo_${demo}_runtime.c" -Wl,--gc-sections \
                    -o "${test_dir}/test_demo_${demo}_runtime"
                "${test_dir}/test_demo_${demo}_runtime"
            done
            ;;
    esac
    # 足球看板的纯逻辑测试:模型/文案、JSON 解析、导航状态机、配置。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_app_model.c main/app_model.c \
        -o "${test_dir}/test_app_model"
    "${test_dir}/test_app_model"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_app_json.c main/app_json.c main/app_model.c \
        -o "${test_dir}/test_app_json"
    "${test_dir}/test_app_json"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_app_nav.c main/app_nav.c \
        -o "${test_dir}/test_app_nav"
    "${test_dir}/test_app_nav"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_app_config.c main/app_config.c \
        -o "${test_dir}/test_app_config"
    "${test_dir}/test_app_config"
    # 配网交互:屏上键盘模型 + "连过的网络"历史与列表合并。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_app_keyboard.c main/app_keyboard.c \
        -o "${test_dir}/test_app_keyboard"
    "${test_dir}/test_app_keyboard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_app_known.c main/app_known.c \
        -o "${test_dir}/test_app_known"
    "${test_dir}/test_app_known"
    # 单场赛况:进球时间轴 / 阵型 / 技术统计的解析。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_app_detail.c main/app_detail.c main/app_model.c \
        -o "${test_dir}/test_app_detail"
    "${test_dir}/test_app_detail"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/check_fonts.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    # 构建目录要交给 idf.py / cmake 这些原生程序,在 Git Bash 下 "/tmp" 这种
    # POSIX 路径它们不认。允许用 TMPDIR 指一个 Windows 也能用的路径(如 C:/...);
    # 默认仍是 /tmp,Linux/macOS 行为不变。
    validation_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/ai-passport-firmware.XXXXXX")"
    trap 'case "${validation_build_dir}" in */ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
