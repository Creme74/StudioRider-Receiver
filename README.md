<p align="center">
  <img src="docs/images/logo.png" width="160" alt="StudioRider logo: a rider on a moped">
</p>

# StudioRider Receiver

A companion plugin for [StudioRider](https://github.com/Creme74/StudioRider).

StudioRider lets someone listen to your DAW in a web browser: the listener needs
nothing but the link. **StudioRider Receiver** is for listeners who would rather
have the stream inside their own DAW, to monitor it next to their own tracks or
to record it. It is optional: nothing changes for StudioRider users.

```
Studio:   DAW ─► StudioRider ─► Cloudflare link ─┬─► web browser (no install)
Listener:                                        └─► DAW ─► StudioRider Receiver
```

<p align="center">
  <img src="docs/images/receiver.png" width="45%" alt="The StudioRider Receiver window: link, Connect/Disconnect, level meters, quality and buffer, stats">
</p>

- macOS 13 Ventura or later. Audio Unit and VST3.
- Works with links from StudioRider 1.0 and later.

## Installing

There is no installer yet: build it from source (see [Building](#building)).
Double-click **Build StudioRider Receiver.command**: it builds the plugin, puts
it in your own `~/Library/Audio/Plug-Ins` folder and checks the Audio Unit.
Then quit and reopen your DAW.

## Using it

1. Create an **instrument track** and load **StudioRider Receiver** as its
   instrument. As an Audio Unit, it is listed under **AU MIDI-controlled
   Effects** (it ignores MIDI).
2. Paste the link the studio sent you (**Paste**), then click **Connect**.
3. To record what you hear, send that track to a bus and record the bus on an
   audio track.

Why an instrument track: some DAWs never run an effect on an audio or aux track
that carries no sound, to save CPU. The receiver's input is always silent (the
sound comes from the network), so there it would never be asked for audio. An
instrument track is always run. If the plugin isn't run, its window says
**Not processed**, and its meters still show the sound that arrives.

**Quality** works as on the listening page: the studio's choice by default, or a
lighter quality; the studio never sends more than its own setting. **Buffer** is
the delay kept against network hiccups (0.5 s by default). The sound comes out at
100 % (no volume control in the plugin: use the track's fader), converted to the
DAW's sample rate. The two machines' clocks never run at exactly the same speed:
the receiver follows the difference (a few dozen ppm, inaudible), so the buffer
neither empties nor fills up over time.

The link is saved with the project, but opening a project never connects by
itself. If the studio stops sharing, the receiver says so; if the connection
drops, it reconnects on its own and gives up after 2 minutes. A StudioRider link
accepts 5 listeners at a time, browsers and receivers together.

The **☰** menu has **About** and **Check for updates**.

## How it works

- `Source/ReceiverEngine.*` (no JUCE): connection and reconnection, decoding and
  resampling on the network thread, a lock-free buffer read by the audio thread,
  clock-drift correction.
- `Source/StreamDecoder.*`: StudioRider's audio messages (`MXS2`): FLAC
  16/24-bit frames (libFLAC) or Opus packets (libopus).
- `Source/Resampler.*`: windowed-sinc resampler (about 120 dB SNR) whose ratio
  can be nudged by the ppm without clicks.
- `Source/WebSocketClient*`: macOS `NSURLSessionWebSocketTask` (wss://, system
  TLS and proxies); a plain `ws://` client elsewhere (used by the tests).
- `Source/ReceiverProcessor.*`, `Source/ReceiverEditor.*`: the JUCE plugin and
  its window (same look as StudioRider's listening page).

## Building

On a Mac with Apple's Command Line Tools (`xcode-select --install`): double-click
**Build StudioRider Receiver.command**, or run `./build.sh`. It downloads JUCE,
libFLAC and libopus at the versions pinned in `scripts/common.sh` (CMake is
installed with Homebrew if missing).

Tests (macOS or Linux): `scripts/run_tests.sh` (on Linux, `xvfb-run
scripts/run_tests.sh`). They download StudioRider's sources and run its real
server: FLAC bit-exactness, Opus, resampling quality, clock drift, quality
changes, full studio, expired link, "Stop sharing", saved state, "Not
processed", and screenshots of the window in `build/screenshots`.

## Licence

GNU General Public License, version 3 or later: see [LICENSE](LICENSE). Third-party
software and licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
