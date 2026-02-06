#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${FIRMWARE_VERSION:-}" ]]; then
  export FIRMWARE_VERSION="v0.0.0-local"
fi

if [[ -z "${PLATFORMIO_HOME_DIR:-}" ]]; then
  export PLATFORMIO_HOME_DIR="${PWD}/.platformio"
fi

exec ./build.sh build-firmware RAK_3401_repeater
