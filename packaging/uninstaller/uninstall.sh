#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Run as root by "Uninstall StudioRider Receiver.app". Argument: the user's home folder.
# Removes everything the installer put on the Mac, plus copies of the plugin in the
# user's own Plug-Ins folder (built with build.sh). StudioRider itself is left alone.
set -u
user_home="${1:-}"

rm -rf "/Library/Audio/Plug-Ins/Components/StudioRider Receiver.component" \
       "/Library/Audio/Plug-Ins/VST3/StudioRider Receiver.vst3" \
       "/Library/Application Support/StudioRider Receiver"

if [[ "$user_home" == /* && -d "$user_home/Library" ]]; then
    rm -rf "$user_home/Library/Audio/Plug-Ins/Components/StudioRider Receiver.component" \
           "$user_home/Library/Audio/Plug-Ins/VST3/StudioRider Receiver.vst3" \
           "$user_home/Library/Logs/StudioRider/receiver.log"
    rmdir "$user_home/Library/Logs/StudioRider" 2>/dev/null || true
fi

/usr/sbin/pkgutil --forget net.studiorider.receiver.pkg >/dev/null 2>&1 || true
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true

# Last: the uninstaller itself.
rm -rf "/Applications/StudioRider Receiver"
exit 0
