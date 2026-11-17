// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
#include "Resampler.h"

#include <algorithm>
#include <cmath>

namespace studiorider
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kBeta = 10.0;       // Kaiser window: ~100 dB stop-band
    constexpr double kCutoff = 0.95;     // middle of the transition band, fraction of Nyquist
    constexpr int kBaseTaps = 128;       // transition band ≈ 10 % of Nyquist

    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < sum * 1e-12)
                break;
        }
        return sum;
    }
}

void Resampler::setup (double in, double out)
{
    inRate = in;
    outRate = out;
    nominalStep = step = in / out;

    // Downsampling: lower the cutoff to the output's Nyquist, longer filter.
    const double scale = std::min (1.0, out / in);
    const double fc = kCutoff * scale;
    taps = (int) std::ceil (kBaseTaps / scale);
    taps += taps & 1;
    const int half = taps / 2;

    table.assign ((std::size_t) (kPhases + 1) * (std::size_t) taps, 0.0f);
    const double i0Beta = besselI0 (kBeta);
    for (int p = 0; p <= kPhases; ++p)
    {
        const double frac = (double) p / kPhases;
        double sum = 0;
        std::vector<double> c ((std::size_t) taps);
        for (int k = 0; k < taps; ++k)
        {
            const double t = (double) (k - half + 1) - frac;        // distance from the output point
            const double x = fc * t;
            const double sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (kPi * x) / (kPi * x);
            const double w = t / half;
            const double win = std::abs (w) >= 1.0 ? 0.0 : besselI0 (kBeta * std::sqrt (1.0 - w * w)) / i0Beta;
            c[(std::size_t) k] = fc * sinc * win;
            sum += c[(std::size_t) k];
        }
        for (int k = 0; k < taps; ++k)    // exact unity gain at DC for every phase
            table[(std::size_t) p * (std::size_t) taps + (std::size_t) k] = (float) (c[(std::size_t) k] / sum);
    }

    // Start with half a filter of silence, so the first output is centred on the first input.
    histL.assign ((std::size_t) half, 0.0f);
    histR.assign ((std::size_t) half, 0.0f);
    pos = half - 1;
}

void Resampler::setCorrection (double ppm) noexcept
{
    step = nominalStep / (1.0 + ppm * 1e-6);
}

void Resampler::process (const float* left, const float* right, std::size_t n, std::vector<float>& out)
{
    if (taps == 0)
        return;
    histL.insert (histL.end(), left, left + n);
    histR.insert (histR.end(), right, right + n);

    const int half = taps / 2;
    const auto size = (double) histL.size();
    out.reserve (out.size() + (std::size_t) ((double) n / step) * 2 + 8);

    // Output at "pos" uses input samples floor(pos) - half + 1 … floor(pos) + half.
    while (std::floor (pos) + half < size)
    {
        const auto idx = (long) std::floor (pos);
        const double pf = (pos - (double) idx) * kPhases;
        const int p = std::min ((int) pf, kPhases - 1);
        const float a = (float) (pf - p);
        const float* c0 = table.data() + (std::size_t) p * (std::size_t) taps;
        const float* c1 = c0 + taps;
        const float* xl = histL.data() + (idx - half + 1);
        const float* xr = histR.data() + (idx - half + 1);
        float sl0 = 0, sl1 = 0, sr0 = 0, sr1 = 0;
        for (int k = 0; k < taps; ++k)
        {
            sl0 += xl[k] * c0[k];
            sl1 += xl[k] * c1[k];
            sr0 += xr[k] * c0[k];
            sr1 += xr[k] * c1[k];
        }
        out.push_back (sl0 + (sl1 - sl0) * a);
        out.push_back (sr0 + (sr1 - sr0) * a);
        pos += step;
    }

    // Drop the history no longer needed.
    const long keepFrom = std::min ((long) std::floor (pos) - half + 1, (long) histL.size());
    if (keepFrom > 0)
    {
        histL.erase (histL.begin(), histL.begin() + keepFrom);
        histR.erase (histR.begin(), histR.begin() + keepFrom);
        pos -= (double) keepFrom;
    }
}

} // namespace studiorider
