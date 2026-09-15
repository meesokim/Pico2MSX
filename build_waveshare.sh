#!/bin/bash
# ==============================================================================
# Build script for Waveshare RP2350-PiZero (MSX1 / MSX2 / MSX2+)
# Target: Waveshare RP2350-PiZero (DVI HDMI, PIO-USB Host, PSRAM, MSX Bus GPIO)
# ==============================================================================

set -e

BOARD="WAVESHARE"
BOARD_NAME="Waveshare RP2350-PiZero (HDMI / PIO-USB / PSRAM / MSX-Bus)"

# Resolve PICO_SDK_PATH if not set
if [ -z "${PICO_SDK_PATH:-}" ]; then
    if [ -d "/home/msx/pico-sdk" ]; then
        export PICO_SDK_PATH="/home/msx/pico-sdk"
    elif [ -d "$(pwd)/pico-sdk" ]; then
        export PICO_SDK_PATH="$(pwd)/pico-sdk"
    fi
fi

# Parse Options:
#   --msx1              -> MSX1 (DEFAULT_MSX_VERSION=0)
#   --msx2              -> MSX2 (DEFAULT_MSX_VERSION=1)
#   --msx2p / --msx2plus -> MSX2+ (DEFAULT_MSX_VERSION=2)
#   DEFAULT_MSX_VERSION=<0|1|2>
MSX_VERSION_ARG="-DDEFAULT_MSX_VERSION=0"
OUT_SUFFIX="waveshare_msx1"

for arg in "$@"; do
    case "$arg" in
        --msx1)
            MSX_VERSION_ARG="-DDEFAULT_MSX_VERSION=0"
            OUT_SUFFIX="waveshare_msx1"
            ;;
        --msx2)
            MSX_VERSION_ARG="-DDEFAULT_MSX_VERSION=1"
            OUT_SUFFIX="waveshare_msx2"
            ;;
        --msx2p|--msx2plus)
            MSX_VERSION_ARG="-DDEFAULT_MSX_VERSION=2"
            OUT_SUFFIX="waveshare_msx2p"
            ;;
        DEFAULT_MSX_VERSION=*)
            VAL="${arg#*=}"
            if [[ "$VAL" =~ ^[0-2]$ ]]; then
                MSX_VERSION_ARG="-DDEFAULT_MSX_VERSION=${VAL}"
                case "$VAL" in
                    0) OUT_SUFFIX="waveshare_msx1";;
                    1) OUT_SUFFIX="waveshare_msx2";;
                    2) OUT_SUFFIX="waveshare_msx2p";;
                esac
            else
                echo "Invalid DEFAULT_MSX_VERSION: $VAL (must be 0, 1, or 2)"
                exit 2
            fi
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "============================================================"
echo "  PicoMSX Build for $BOARD_NAME"
echo "============================================================"
echo "MSX Target Model : ${MSX_VERSION_ARG}"
echo "PICO_SDK_PATH    : ${PICO_SDK_PATH:-<not set>}"
echo "Build Directory  : ${BUILD_DIR}"
echo "============================================================"
echo ""

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[1/3] Configuring CMake..."
cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR" \
    -DPICO_SDK_PATH="${PICO_SDK_PATH}" \
    -DBOARD_TYPE=WAVESHARE \
    -DPICO_PLATFORM=rp2350-arm-s \
    -DCMAKE_BUILD_TYPE=Release \
    ${MSX_VERSION_ARG}

echo ""
echo "[2/3] Compiling..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo ""
echo "[3/3] Packaging UF2 firmware..."
# Ensure UF2 is properly generated/converted for RP2350 ARM Secure mode
if [ -f "picomsx.elf" ]; then
    if command -v picotool &> /dev/null; then
        picotool uf2 convert picomsx.elf picomsx.uf2 --family rp2350-arm-s 2>/dev/null || true
    fi
fi

if [ -f "picomsx.uf2" ]; then
    cp -f picomsx.uf2 "picomsx_${OUT_SUFFIX}.uf2"
    echo "============================================================"
    echo "  ✓ Build successful!"
    echo "============================================================"
    echo "Output UF2 : ${BUILD_DIR}/picomsx_${OUT_SUFFIX}.uf2"
    ls -lh "${BUILD_DIR}/picomsx_${OUT_SUFFIX}.uf2"
    echo ""
    echo "Flashing instructions:"
    echo "  1. Hold BOOTSEL button and connect Waveshare RP2350-PiZero via USB."
    echo "  2. Drag and drop 'picomsx_${OUT_SUFFIX}.uf2' onto the RPI-RP2 drive."
    echo "============================================================"
else
    echo "Build failed: picomsx.uf2 not created."
    exit 1
fi
