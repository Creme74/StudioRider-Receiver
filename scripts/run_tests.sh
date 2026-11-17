#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Builds and runs the tests (macOS or Linux). They run a real StudioRider server,
# downloaded at the version pinned in scripts/common.sh.
#   scripts/run_tests.sh [--quick]
# The plugin test needs a display (on Linux: xvfb-run scripts/run_tests.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/common.sh
fetch_dependencies
fetch_test_server
cmake -S . -B build/tests -DCMAKE_BUILD_TYPE=Release -DSTUDIORIDER_RECEIVER_BUILD_TESTS=ON >/dev/null
cmake --build build/tests -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)" \
      --target studiorider_receiver_test studiorider_receiver_plugin_test
build/tests/studiorider_receiver_test "$@"
mkdir -p build/screenshots
build/tests/studiorider_receiver_plugin_test build/screenshots
