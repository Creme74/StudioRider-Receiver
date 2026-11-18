# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Shared by build.sh and package.sh: tool checks and third-party downloads.
# Meant to be sourced from the project's root folder.

JUCE_TAG="9.0.4"
FLAC_TAG="1.5.0"
OPUS_TAG="v1.5.2"
STUDIORIDER_TAG="v1.0"   # tests only: the StudioRider server the tests talk to

MIN_MACOS="13.0"

say() { printf "\n\033[1;38;5;208m==>\033[0m \033[1m%s\033[0m\n" "$*"; }
die() { printf "\n\033[1;31mError:\033[0m %s\n" "$*" >&2; exit 1; }

receiver_version() {
    sed -n 's/^project(StudioRiderReceiver VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt
}

# The version as shown to people: 1.0 rather than 1.0.0 (patch number only when it isn't 0).
receiver_shown_version() {
    receiver_version | sed 's/^\([0-9]*\.[0-9]*\)\.0$/\1/'
}

check_tools() {
    [[ "$(uname)" == "Darwin" ]] || die "This script is meant for macOS."

    say "Checking tools"
    if ! xcode-select -p >/dev/null 2>&1; then
        echo "Apple's developer tools (Command Line Tools) are required."
        xcode-select --install || true
        die "Install them in the window that just opened, then run this script again."
    fi
    echo "Apple tools: $(xcode-select -p)"

    # Homebrew may be installed in a personal folder.
    for p in /opt/homebrew/bin /usr/local/bin "$HOME/.homebrew/bin" "$HOME/homebrew/bin"; do
        [[ -d "$p" ]] && PATH="$PATH:$p"
    done
    export PATH

    if ! command -v cmake >/dev/null 2>&1; then
        if command -v brew >/dev/null 2>&1; then
            say "Installing CMake (Homebrew)"
            brew install cmake
        else
            say "Installing CMake (Python, into .tools/)"
            python3 -m venv .tools/venv
            .tools/venv/bin/pip install -q cmake
            PATH="$PWD/.tools/venv/bin:$PATH"
        fi
    fi
    echo "CMake: $(cmake --version | head -1)"
}

fetch() { # fetch <GitHub repo> <tag> <folder>
    say "Downloading $1 $2 (from GitHub)"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$2" "https://github.com/$1.git" "$3" 2>&1 \
        | grep -v "is not a commit" || true
    [[ -d "$3" ]] || die "Could not download $1."
    rm -rf "$3/.git"
}

fetch_dependencies() {
    [[ -f third_party/flac/CMakeLists.txt ]] || fetch xiph/flac "$FLAC_TAG" third_party/flac
    [[ -f third_party/opus/CMakeLists.txt ]] || fetch xiph/opus "$OPUS_TAG" third_party/opus
    [[ -f third_party/opus/package_version ]] || echo "PACKAGE_VERSION=\"${OPUS_TAG#v}\"" > third_party/opus/package_version
    [[ -f JUCE/CMakeLists.txt ]] || fetch juce-framework/JUCE "$JUCE_TAG" JUCE
}

# Tests only: StudioRider's sources (the server the tests talk to) and their own
# dependencies. Works on macOS and Linux. JUCE isn't needed for StudioRider's
# server: a placeholder keeps its script from downloading it.
fetch_test_server() {
    [[ -f third_party/StudioRider/CMakeLists.txt ]] || fetch Creme74/StudioRider "$STUDIORIDER_TAG" third_party/StudioRider
    mkdir -p third_party/StudioRider/JUCE
    [[ -f third_party/StudioRider/JUCE/CMakeLists.txt ]] || echo "# placeholder: not needed for the tests" > third_party/StudioRider/JUCE/CMakeLists.txt
    (cd third_party/StudioRider && bash -c 'source scripts/common.sh && fetch_dependencies')
}

# cmake_build <build folder> <architectures> [extra cmake arguments…]
cmake_build() {
    local dir="$1" archs="$2"; shift 2
    if [[ -f "$dir/CMakeCache.txt" ]] && ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$PWD" "$dir/CMakeCache.txt"; then
        say "Clearing a build folder made elsewhere"   # e.g. after the folder was renamed
        rm -rf "$dir"
    fi

    say "Configuring (macOS $(sw_vers -productVersion), $archs)"
    cmake -S . -B "$dir" -G "Unix Makefiles" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_OSX_ARCHITECTURES="$archs" \
          -DJUCE_DIR="$PWD/JUCE" "$@" >/dev/null

    say "Building (a few minutes the first time)"
    set +e
    cmake --build "$dir" --config Release -j"$(sysctl -n hw.ncpu)" \
          --target StudioRiderReceiver_AU 2>&1 | tee "$dir.log" | grep --line-buffered -E "Built target|error:"
    local status=${PIPESTATUS[0]}
    set -e
    [[ $status -eq 0 ]] || die "The build failed (full log: $dir.log)."
}
