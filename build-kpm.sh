#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------
# build-kpm.sh – build a KPM from kpms/<name>/ (NS_team)
#
# Usage:
#   ./build-kpm.sh [kpm_name] [toolchain_prefix]
#
# Examples:
#   ./build-kpm.sh                       # builds kpms/ns_team
#   ./build-kpm.sh ns_team
#   ./build-kpm.sh ns_team /opt/tc/bin/aarch64-none-elf-
#
# If no toolchain prefix is given, the script auto-detects one:
#   1. ./compiler/<arm toolchain>/bin/aarch64-none-elf-   (see download-toolchain.txt)
#   2. aarch64-none-elf- / aarch64-linux-gnu- from $PATH
# ---------------------------------------------------------

KPM_NAME="${1:-ns_team}"
KP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KPM_DIR="${KP_DIR}/kpms/${KPM_NAME}"

# ---------------------------------------------------------
# Resolve the toolchain prefix
# ---------------------------------------------------------
find_toolchain() {
    # 1) explicitly passed prefix
    if [ -n "${1:-}" ]; then
        echo "$1"
        return
    fi

    # 2) bundled toolchain downloaded into ./compiler
    local bundled
    bundled="$(find "${KP_DIR}/compiler" -maxdepth 4 -type f \
        -name 'aarch64-none-elf-gcc' 2>/dev/null | head -n 1 || true)"
    if [ -n "$bundled" ]; then
        # strip the trailing "gcc" to leave the compiler prefix
        echo "${bundled%gcc}"
        return
    fi

    # 3) system cross compilers
    local c
    for c in aarch64-none-elf- aarch64-linux-gnu-; do
        if command -v "${c}gcc" &>/dev/null; then
            echo "$c"
            return
        fi
    done

    echo ""
}

TARGET_COMPILE="$(find_toolchain "${2:-}")"

# ---------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------

if [ -z "$TARGET_COMPILE" ] || ! command -v "${TARGET_COMPILE}gcc" &>/dev/null; then
    echo "[!] No aarch64 toolchain found."
    echo "    Install one, pass its prefix as the second argument, or run"
    echo "    the commands in download-toolchain.txt to fetch it into ./compiler/."
    exit 1
fi

if [ ! -d "$KPM_DIR" ]; then
    echo "[!] KPM directory '${KPM_DIR}' not found."
    echo "    Available KPMs:"
    ls "${KP_DIR}/kpms/" 2>/dev/null || true
    exit 1
fi

# ---------------------------------------------------------
# Build
# ---------------------------------------------------------

echo "┌─────────────────────────────────────────┐"
echo "│  Building KPM : ${KPM_NAME}"
echo "│  Toolchain    : ${TARGET_COMPILE}gcc"
echo "│  Project root : ${KP_DIR}"
echo "└─────────────────────────────────────────┘"

(
    cd "$KPM_DIR"
    TARGET_COMPILE="$TARGET_COMPILE" KP_DIR="$KP_DIR" make clean
    TARGET_COMPILE="$TARGET_COMPILE" KP_DIR="$KP_DIR" make
)

KPM_FILE="$(find "$KPM_DIR" -maxdepth 1 -name '*.kpm' | head -n 1)"

if [ -z "$KPM_FILE" ]; then
    echo "[!] No .kpm file produced – check build output above."
    exit 1
fi

echo ""
echo "✓ Done! Output: ${KPM_FILE}"
echo ""
echo "To load on a rooted device:"
echo "  adb push ${KPM_FILE} /data/local/tmp/"
echo "  adb push <kpatch_binary> /data/local/tmp/"
echo "  adb shell su -c '/data/local/tmp/kpatch load /data/local/tmp/$(basename "$KPM_FILE")'"
echo "  adb shell su -c 'ls -la /dev/ns_team'"
