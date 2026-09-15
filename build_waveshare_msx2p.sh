#!/bin/bash
# ==============================================================================
# Shortcut: Build PicoMSX for Waveshare RP2350-PiZero (MSX2+)
# ==============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/build_waveshare.sh" --msx2p "$@"
