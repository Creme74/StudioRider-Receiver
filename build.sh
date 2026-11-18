#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Builds StudioRider Receiver (Audio Unit) for this Mac and installs it for the
# current user (~/Library/Audio/Plug-Ins), then checks it (auval).
set -euo pipefail
cd "$(dirname "$0")"
source scripts/common.sh

check_tools
fetch_dependencies

mkdir -p build
CMAKE_DIR=build/cmake
cmake_build "$CMAKE_DIR" "$(uname -m)" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS"

PLUGINS="$HOME/Library/Audio/Plug-Ins"
AU="$PLUGINS/Components/StudioRider Receiver.component"
VST3="$PLUGINS/VST3/StudioRider Receiver.vst3"
ART="$CMAKE_DIR/StudioRiderReceiver_artefacts/Release"

say "Installing and signing locally"
mkdir -p "$(dirname "$AU")"
rm -rf "$AU"
cp -R "$ART/AU/StudioRider Receiver.component" "$AU"
codesign --force --deep --sign - "$AU" >/dev/null 2>&1
echo "AU:   $AU"
if [[ -d "$VST3" ]]; then   # left by an earlier test build: the VST3 isn't ready yet
    rm -rf "$VST3"
    echo "Removed an earlier test copy of the VST3: $VST3"
fi
if [[ -d "/Library/Audio/Plug-Ins/Components/StudioRider Receiver.component" ]]; then
    echo "Note: StudioRider Receiver is also in /Library/Audio/Plug-Ins (for all users). Your DAW may see two copies."
fi

say "Validating the Audio Unit (auval)"
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
sleep 2
if auval -v aumf Mxrv Mxsr > build/auval.log 2>&1 && grep -q "AU VALIDATION SUCCEEDED" build/auval.log; then
    echo "auval: AU VALIDATION SUCCEEDED"
else
    tail -25 build/auval.log
    die "auval failed (details in build/auval.log)."
fi

say "Done"
echo "Quit and reopen your DAW, then load StudioRider Receiver as the instrument of an"
echo "instrument track (Audio Units: under \"AU MIDI-controlled Effects\")."
