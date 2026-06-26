#!/bin/sh
# Wrapper for idf.py that sets up the ESP-IDF environment internally.
# Usage: ./idf.sh [idf.py arguments...]
# Example: ./idf.sh build
#          ./idf.sh -p /dev/cu.usbmodem31323101 flash
#
# This avoids the need to run `source esp-idf/export.sh` in the calling shell,
# which triggers a permission prompt in automated environments.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/esp-idf/export.sh" > /dev/null 2>&1
exec idf.py -C "$SCRIPT_DIR/apps/full" "$@"
