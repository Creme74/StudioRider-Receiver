#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Double-click this file (or run it in Terminal) to build StudioRider Receiver (Audio
# Unit) and install it for yourself (~/Library/Audio/Plug-Ins).
cd "$(dirname "$0")"
mkdir -p build
./build.sh 2>&1 | tee build/run.log
echo "${PIPESTATUS[0]}" > build/run.status
echo
echo "You can close this window."
