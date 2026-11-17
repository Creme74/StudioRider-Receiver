// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// StudioRider Receiver — stereo resampler with a ratio that can be nudged at
// any time (clock drift correction), without clicks.
//
// Windowed-sinc (Kaiser, about 100 dB stop-band), polyphase table with linear
// interpolation between phases, double-precision position. Pass-band up to
// 90 % of the lower of the two Nyquist frequencies (19.8 kHz at 44.1 kHz).
// SpeexDSP can't be used here: changing its ratio finely (a few ppm) overflows
// its 32-bit fraction arithmetic.

#pragma once

#include <cstddef>
#include <vector>

namespace studiorider
{

class Resampler
{
public:
    void setup (double inRate, double outRate);   // builds the filter, clears the history
    bool isSetUp() const noexcept                  { return taps > 0; }
    double getInRate() const noexcept              { return inRate; }
    double getOutRate() const noexcept             { return outRate; }

    // ppm > 0: produce slightly more output (the listener's clock runs fast).
    void setCorrection (double ppm) noexcept;

    // Appends the output, interleaved stereo, to "out".
    void process (const float* left, const float* right, std::size_t numFrames, std::vector<float>& out);

private:
    static constexpr int kPhases = 512;

    double inRate = 0, outRate = 0, nominalStep = 1.0, step = 1.0;
    int taps = 0;                    // even
    std::vector<float> table;        // (kPhases + 1) * taps
    std::vector<float> histL, histR; // input history
    double pos = 0;                  // position of the next output, in history samples
};

} // namespace studiorider
