#!/usr/bin/env bash
set -euo pipefail

version="1.7.12"
destination="${1:-/tmp/actionlint-${version}}"
system_name="$(uname -s)"
machine_name="$(uname -m)"

case "${system_name}/${machine_name}" in
    Linux/x86_64)
        platform="linux_amd64"
        checksum="8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8"
        binary="actionlint"
        archive_ext="tar.gz"
        ;;
    Linux/aarch64|Linux/arm64)
        platform="linux_arm64"
        checksum="325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6"
        binary="actionlint"
        archive_ext="tar.gz"
        ;;
    Darwin/x86_64)
        platform="darwin_amd64"
        checksum="5b44c3bc2255115c9b69e30efc0fecdf498fdb63c5d58e17084fd5f16324c644"
        binary="actionlint"
        archive_ext="tar.gz"
        ;;
    Darwin/arm64)
        platform="darwin_arm64"
        checksum="aba9ced2dee8d27fecca3dc7feb1a7f9a52caefa1eb46f3271ea66b6e0e6953f"
        binary="actionlint"
        archive_ext="tar.gz"
        ;;
    # Git Bash / MSYS2 报的是 MINGW64_NT-10.0-xxxxx 这种带构建号的名字,
    # 所以这里用通配而不是精确匹配。Windows 版发的是 zip,可执行文件带 .exe。
    MINGW*_NT*/x86_64|MSYS*_NT*/x86_64|CYGWIN*_NT*/x86_64)
        platform="windows_amd64"
        checksum="6e7241b51e6817ea6a047693d8e6fed13b31819c9a0dd6c5a726e1592d22f6e9"
        binary="actionlint.exe"
        archive_ext="zip"
        ;;
    *)
        echo "Unsupported actionlint platform: ${system_name}/${machine_name}" >&2
        exit 1
        ;;
esac

if [[ -x "${destination}/${binary}" ]]; then
    printf '%s\n' "${destination}/${binary}"
    exit 0
fi

archive_name="actionlint_${version}_${platform}.${archive_ext}"
archive_path="${destination}/${archive_name}"
download_url="https://github.com/rhysd/actionlint/releases/download/v${version}/${archive_name}"

mkdir -p "${destination}"
if ! curl --fail --location --silent --show-error --retry 3 --retry-all-errors \
    "${download_url}" --output "${archive_path}"; then
    if ! command -v gh >/dev/null 2>&1; then
        echo "actionlint download failed and GitHub CLI is unavailable" >&2
        exit 1
    fi
    rm -f "${archive_path}"
    gh release download "v${version}" --repo rhysd/actionlint \
        --pattern "${archive_name}" --dir "${destination}"
fi
if command -v sha256sum >/dev/null 2>&1; then
    printf '%s  %s\n' "${checksum}" "${archive_path}" | sha256sum --check --status
elif command -v shasum >/dev/null 2>&1; then
    [[ "$(shasum -a 256 "${archive_path}" | awk '{print $1}')" == "${checksum}" ]]
else
    echo "No SHA-256 verification tool is available" >&2
    exit 1
fi

if [[ "${archive_ext}" == "zip" ]]; then
    unzip -o -q "${archive_path}" "${binary}" -d "${destination}"
else
    tar -xzf "${archive_path}" -C "${destination}" "${binary}"
fi
chmod +x "${destination}/${binary}"
printf '%s\n' "${destination}/${binary}"
