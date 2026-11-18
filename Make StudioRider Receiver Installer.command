#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Double-click this file (or run it in Terminal) to make the StudioRider Receiver
# installer (build/StudioRider-Receiver-<version>.pkg): the Audio Unit for Apple
# Silicon and Intel Macs.
cd "$(dirname "$0")"
mkdir -p build
./package.sh 2>&1 | tee build/package.log
echo "${PIPESTATUS[0]}" > build/package.status
echo
echo "You can close this window."
