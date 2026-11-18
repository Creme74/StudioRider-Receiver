# Third-party software

StudioRider Receiver is free software, licensed under the GNU General Public
License, version 3 or (at your option) any later version: see [LICENSE](LICENSE).

It is built with the software below. The build scripts download it from its
authors' GitHub repositories, at the versions pinned in `scripts/common.sh`; none
of it is modified.

## Inside the plugin (statically linked)

| Software | Version | Licence |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.4 | AGPL-3.0-only (or the commercial JUCE licence) |
| VST3 SDK (Steinberg, included in JUCE; VST3 version only, not published yet) | | MIT |
| AudioUnitSDK (Apple, included in JUCE) | | Apache-2.0 |
| [libFLAC](https://github.com/xiph/flac) | 1.5.0 | BSD-3-Clause |
| [libopus](https://github.com/xiph/opus) | 1.5.2 | BSD-3-Clause |

JUCE is used under the AGPLv3. Section 13 of the GPLv3 and of the AGPLv3 allows
combining the two: the JUCE part of the plugin stays under the AGPLv3, the rest
under the GPLv3. JUCE itself contains other libraries (zlib, HarfBuzz, SheenBidi,
libpng, the IJG JPEG library, LunaSVG, PlutoVG, …); they are listed with their
licences in JUCE's `JUCE.spdx.json`.

## Tests only (not in the plugin)

The tests download [StudioRider](https://github.com/Creme74/StudioRider)
(GPL-3.0-or-later) and its own dependencies (civetweb, libFLAC, libopus,
SpeexDSP), to run a real StudioRider server.

## Trademarks

Audio Units is a trademark of Apple Inc. VST is a registered trademark of
Steinberg Media Technologies GmbH. Cloudflare is a trademark of Cloudflare, Inc.
StudioRider Receiver is not affiliated with any of them.
