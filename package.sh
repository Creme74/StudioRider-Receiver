#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Makes the StudioRider Receiver installer: build/StudioRider-Receiver-<version>.pkg
#   - the Audio Unit, universal (Apple Silicon + Intel), macOS 13 or later
#   - licence texts, in /Library/Application Support/StudioRider Receiver
#   - an uninstaller app, in /Applications/StudioRider Receiver
#
# Also writes build/StudioRider-Receiver-<version>-source.tar.gz (the matching
# source code, to hand out with the installer as the GPL asks).
#
# Optional signing (not needed to use the installer, see README):
#   SIGN_APP="Developer ID Application: …"  SIGN_INSTALLER="Developer ID Installer: …"
#   NOTARY_PROFILE=<notarytool keychain profile>   ./package.sh
set -euo pipefail
cd "$(dirname "$0")"
source scripts/common.sh

ARCHS="arm64;x86_64"
PKG_ID="net.studiorider.receiver.pkg"
VERSION="$(receiver_version)"         # 1.0.0: for macOS (package and bundle versions)
SHOWN="$(receiver_shown_version)"     # 1.0: in names and titles
[[ -n "$VERSION" ]] || die "Could not read the version from CMakeLists.txt."

check_tools
for t in pkgbuild productbuild lipo codesign otool python3 ditto osacompile plutil; do
    command -v "$t" >/dev/null 2>&1 || die "$t is missing (it comes with Apple's developer tools)."
done
fetch_dependencies

# --- 1. The plugin, universal --------------------------------------------------
CMAKE_DIR=build/universal
cmake_build "$CMAKE_DIR" "$ARCHS" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS"
AU_SRC="$CMAKE_DIR/StudioRiderReceiver_artefacts/Release/AU/StudioRider Receiver.component"

say "Checking the binary"
minos() { # minos <binary> <arch>: minimum macOS the slice was built for
    otool -arch "$2" -l "$1" | awk '/LC_BUILD_VERSION/ {b=1} /LC_VERSION_MIN_MACOSX/ {v=1}
                                    b && $1=="minos" {print $2; exit} v && $1=="version" {print $2; exit}'
}
bin="$AU_SRC/Contents/MacOS/StudioRider Receiver"
[[ -f "$bin" ]] || die "Missing build output: $bin"
info="$(lipo -info "$bin")"
[[ "$info" == *arm64* && "$info" == *x86_64* ]] || die "Not a universal binary: $info"
echo "StudioRider Receiver.component: arm64 (macOS $(minos "$bin" arm64)+), x86_64 (macOS $(minos "$bin" x86_64)+)"

# --- 2. Files to install ---------------------------------------------------------
say "Preparing the installer contents"
ROOT=build/pkgroot
SUPPORT="$ROOT/Library/Application Support/StudioRider Receiver"
UNINSTALLER="$ROOT/Applications/StudioRider Receiver/Uninstall StudioRider Receiver.app"
AU_DEST="$ROOT/Library/Audio/Plug-Ins/Components/StudioRider Receiver.component"
rm -rf "$ROOT" build/pkgparts
mkdir -p "$(dirname "$AU_DEST")" "$SUPPORT/Licenses" "$(dirname "$UNINSTALLER")" build/pkgparts
ditto --norsrc --noextattr "$AU_SRC" "$AU_DEST"

# The uninstaller: a small AppleScript app (asks for the password with the usual macOS dialog).
osacompile -o "$UNINSTALLER" "packaging/uninstaller/Uninstall StudioRider Receiver.applescript" \
    || die "Could not make the uninstaller (osacompile)."
install -m 755 packaging/uninstaller/uninstall.sh "$UNINSTALLER/Contents/Resources/uninstall.sh"
plutil -replace CFBundleIdentifier -string net.studiorider.receiver.uninstaller "$UNINSTALLER/Contents/Info.plist"
plutil -replace CFBundleShortVersionString -string "$VERSION" "$UNINSTALLER/Contents/Info.plist"
plutil -replace LSMinimumSystemVersion -string "$MIN_MACOS" "$UNINSTALLER/Contents/Info.plist"

L="$SUPPORT/Licenses"
cp LICENSE                         "$L/StudioRider Receiver (GPL-3.0).txt"
cp THIRD_PARTY_NOTICES.md          "$L/THIRD_PARTY_NOTICES.md"
cp third_party/flac/COPYING.Xiph   "$L/libFLAC (BSD-3-Clause).txt"
cp third_party/opus/COPYING        "$L/libopus (BSD-3-Clause).txt"
mkdir -p "$L/JUCE"
cp JUCE/LICENSE.md JUCE/JUCE.spdx.json "$L/JUCE/"
cp JUCE/modules/juce_gui_extra/native/typescript/webview-interop/LICENSE-AGPL.md "$L/JUCE/AGPL-3.0.md"
cp JUCE/modules/juce_audio_plugin_client/AU/AudioUnitSDK/LICENSE.txt "$L/AudioUnitSDK (Apache-2.0).txt"
find "$ROOT" -name .DS_Store -delete
xattr -cr "$ROOT" 2>/dev/null || true

say "Signing"
if [[ -n "${SIGN_APP:-}" ]]; then
    for b in "$AU_DEST" "$UNINSTALLER"; do
        codesign --force --timestamp --options runtime --sign "$SIGN_APP" "$b"
    done
    echo "Signed with: $SIGN_APP"
else
    codesign --force --deep --sign - "$AU_DEST"
    codesign --force --deep --sign - "$UNINSTALLER"
    echo "Signed locally (ad hoc). The installer is not signed: see README, \"Installing\"."
fi

# --- 3. The .pkg -------------------------------------------------------------------
say "Building the installer"
pkgbuild --analyze --root "$ROOT" build/pkgparts/components.plist >/dev/null
# Always install into /Library, even if a copy of the plugin exists somewhere else on the disk.
python3 -I - build/pkgparts/components.plist <<'PY'
import plistlib, sys
path = sys.argv[1]
items = plistlib.load(open(path, "rb"))
def fix(entries):
    for e in entries:
        e["BundleIsRelocatable"] = False
        e["BundleHasStrictIdentifier"] = False
        e["BundleOverwriteAction"] = "upgrade"
        fix(e.get("ChildBundles", []))
fix(items)
plistlib.dump(items, open(path, "wb"))
PY
pkgbuild --root "$ROOT" \
         --component-plist build/pkgparts/components.plist \
         --scripts packaging/scripts \
         --identifier "$PKG_ID" \
         --version "$VERSION" \
         --install-location / \
         build/pkgparts/StudioRiderReceiver.pkg >/dev/null

sed -e "s/@VERSION@/$VERSION/g" -e "s/@SHOWN@/$SHOWN/g" -e "s/@MIN_MACOS@/$MIN_MACOS/g" -e "s/@PKG_ID@/$PKG_ID/g" \
    packaging/Distribution.xml > build/pkgparts/Distribution.xml
mkdir -p build/pkgparts/resources
for f in packaging/resources/*.html; do
    sed -e "s/@VERSION@/$SHOWN/g" "$f" > "build/pkgparts/resources/$(basename "$f")"
done

PKG="build/StudioRider-Receiver-$SHOWN.pkg"
rm -f "$PKG"
if [[ -n "${SIGN_INSTALLER:-}" ]]; then
    productbuild --distribution build/pkgparts/Distribution.xml --resources build/pkgparts/resources \
                 --package-path build/pkgparts --sign "$SIGN_INSTALLER" --timestamp "$PKG" >/dev/null
else
    productbuild --distribution build/pkgparts/Distribution.xml --resources build/pkgparts/resources \
                 --package-path build/pkgparts "$PKG" >/dev/null
fi
if [[ -n "${NOTARY_PROFILE:-}" ]]; then
    say "Notarizing (Apple)"
    xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PKG"
fi

# --- 4. Matching source code -----------------------------------------------------
SRC="build/StudioRider-Receiver-$SHOWN-source.tar.gz"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1 && [[ -z "$(git status --porcelain 2>/dev/null)" ]]; then
    git archive --format=tar.gz --prefix="StudioRider-Receiver-$SHOWN/" -o "$SRC" HEAD
else
    COPYFILE_DISABLE=1 tar -czf "$SRC" -s ",^\.,StudioRider-Receiver-$SHOWN," \
        --exclude ./build --exclude './build-*' --exclude ./JUCE --exclude ./third_party \
        --exclude ./.tools --exclude ./.git --exclude .DS_Store --exclude '*.pkg' .
fi

say "Done"
echo "Installer:   $PKG ($(du -h "$PKG" | cut -f1 | tr -d ' '))"
echo "Source code: $SRC"
echo "Installs StudioRider Receiver $SHOWN (Audio Unit, Apple Silicon + Intel, macOS $MIN_MACOS+)."
pkgutil --check-signature "$PKG" 2>/dev/null | sed -n 2p || true
echo "PACKAGE_OK"
