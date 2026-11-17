// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// Receiver test without JUCE or DAW:
//   1. link parsing
//   2. decoder: FLAC 16/24-bit bit-exact against StudioRider's own encoders, Opus sanity
//   3. end to end: a real StreamServer fed in real time (sine) and a
//      ReceiverEngine pulling audio at another rate, with clock drift,
//      quality changes, "too many listeners" and "Stop sharing".
// Usage: studiorider_receiver_test [--quick]

#include "Encoders.h"
#include "ReceiverEngine.h"
#include "Resampler.h"
#include "StreamDecoder.h"
#include "StreamServer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace studiorider;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) std::printf ("  ok    "); else { std::printf ("  FAIL  "); ++failures; } \
                              std::printf (__VA_ARGS__); std::printf ("\n"); } while (0)

static const char* stateName (ReceiverStatus::State s)
{
    switch (s)
    {
        case ReceiverStatus::State::idle: return "idle";
        case ReceiverStatus::State::connecting: return "connecting";
        case ReceiverStatus::State::reconnecting: return "reconnecting";
        case ReceiverStatus::State::waiting: return "waiting";
        case ReceiverStatus::State::buffering: return "buffering";
        case ReceiverStatus::State::playing: return "playing";
        case ReceiverStatus::State::notProcessed: return "notProcessed";
        case ReceiverStatus::State::ended: return "ended";
        case ReceiverStatus::State::failed: return "failed";
    }
    return "?";
}

//==============================================================================
static void testLinks()
{
    std::printf ("Links\n");
    std::string base, err;
    CHECK (ReceiverEngine::parseLink ("https://a-b.trycloudflare.com/AbC123/", true, base, err)
           && base == "wss://a-b.trycloudflare.com/AbC123/", "https link -> %s", base.c_str());
    CHECK (ReceiverEngine::parseLink ("  a-b.trycloudflare.com/AbC123?q=cd \n", true, base, err)
           && base == "wss://a-b.trycloudflare.com/AbC123/", "no scheme, spaces, query -> %s", base.c_str());
    CHECK (ReceiverEngine::parseLink ("http://127.0.0.1:47800/tok/index.html", false, base, err)
           && base == "ws://127.0.0.1:47800/tok/", "local http link -> %s", base.c_str());
    CHECK (! ReceiverEngine::parseLink ("https://a-b.trycloudflare.com/", true, base, err), "missing code refused (%s)", err.c_str());
    CHECK (! ReceiverEngine::parseLink ("", true, base, err), "empty refused");
    CHECK (! ReceiverEngine::parseLink ("ftp://x/y", true, base, err), "ftp refused");
    CHECK (! ReceiverEngine::parseLink ("https://x.com/tok", false, base, err), "https refused without TLS");
}

//==============================================================================
static std::vector<std::uint8_t> makeMessage (Encoder& enc, Quality q, const EncodedPacket& p, std::uint32_t rate, std::uint32_t seq)
{
    std::vector<std::uint8_t> m (20);
    std::memcpy (m.data(), "MXS2", 4);
    m[4] = enc.codec();
    m[5] = 2;
    m[6] = (std::uint8_t) enc.bitsPerSample();
    m[7] = (std::uint8_t) q;
    auto put = [&] (int at, std::uint32_t v) { for (int i = 0; i < 4; ++i) m[(std::size_t) (at + i)] = (std::uint8_t) (v >> (8 * i)); };
    put (8, rate);
    put (12, p.frames);
    put (16, seq);
    m.insert (m.end(), p.payload.begin(), p.payload.end());
    return m;
}

static void testDecoder()
{
    std::printf ("Decoder\n");
    std::mt19937 rng (42);
    for (int bits : { 16, 24 })
    {
        const Quality q = bits == 16 ? Quality::cd : Quality::hires;
        for (std::uint32_t rate : { 44100u, 96000u })
        {
            std::string err;
            auto enc = makeEncoder (q, rate, false, err);
            const double full = bits == 16 ? 32768.0 : 8388608.0;
            const int n = 4096 * 10 + 1000;
            std::vector<float> L ((std::size_t) n), R ((std::size_t) n);
            std::uniform_int_distribution<int> d ((int) -full, (int) full - 1);
            for (int i = 0; i < n; ++i)
            {
                L[(std::size_t) i] = (float) (d (rng) / full);
                R[(std::size_t) i] = (float) (std::sin (i * 0.01) * 0.5);
                R[(std::size_t) i] = (float) (std::round (R[(std::size_t) i] * full) / full);
            }
            std::vector<EncodedPacket> packets;
            for (int i = 0; i < n; i += 512)
                enc->encode (L.data() + i, R.data() + i, std::min (512, n - i), packets);

            StreamDecoder dec;
            std::vector<float> outL, outR;
            std::uint32_t seq = 0;
            bool ok = true;
            for (auto& p : packets)
            {
                auto msg = makeMessage (*enc, q, p, rate, seq++);
                PacketHeader h;
                ok &= dec.decode (msg.data(), msg.size(), h, outL, outR);
            }
            std::size_t same = 0;
            for (std::size_t i = 0; i < outL.size(); ++i)
                same += (outL[i] == L[i] && outR[i] == R[i]) ? 1 : 0;
            CHECK (ok && outL.size() >= 40960 && same == outL.size(),
                   "FLAC %d-bit %u Hz: %zu packets, %zu/%zu samples bit-exact", bits, rate, packets.size(), same, outL.size());
        }
    }

    // A damaged frame is dropped, the next ones decode.
    {
        std::string err;
        auto enc = makeEncoder (Quality::cd, 48000, false, err);
        std::vector<float> L (4096 * 4, 0.25f), R (4096 * 4, -0.25f);
        std::vector<EncodedPacket> packets;
        enc->encode (L.data(), R.data(), (int) L.size(), packets);
        StreamDecoder dec;
        std::vector<float> outL, outR;
        PacketHeader h;
        auto m0 = makeMessage (*enc, Quality::cd, packets[0], 48000, 0);
        auto m1 = makeMessage (*enc, Quality::cd, packets[1], 48000, 1);
        m1.resize (m1.size() / 2);
        auto m2 = makeMessage (*enc, Quality::cd, packets[2], 48000, 2);
        const bool a = dec.decode (m0.data(), m0.size(), h, outL, outR);
        const bool b = dec.decode (m1.data(), m1.size(), h, outL, outR);
        const bool c = dec.decode (m2.data(), m2.size(), h, outL, outR);
        CHECK (a && ! b && c && outL.size() >= 8192 && outL.back() == 0.25f, "damaged FLAC frame skipped, decoding goes on");
    }

    // Opus: a 1 kHz sine comes back at about the same level and frequency.
    {
        std::string err;
        auto enc = makeEncoder (Quality::eco, 48000, false, err);
        const int n = 48000;
        std::vector<float> L ((std::size_t) n), R ((std::size_t) n);
        for (int i = 0; i < n; ++i)
            L[(std::size_t) i] = R[(std::size_t) i] = (float) (0.5 * std::sin (2 * M_PI * 1000.0 * i / 48000.0));
        std::vector<EncodedPacket> packets;
        enc->encode (L.data(), R.data(), n, packets);
        StreamDecoder dec;
        std::vector<float> outL, outR;
        bool ok = true;
        std::uint32_t seq = 0;
        for (auto& p : packets)
        {
            auto msg = makeMessage (*enc, Quality::eco, p, 48000, seq++);
            PacketHeader h;
            ok &= dec.decode (msg.data(), msg.size(), h, outL, outR);
        }
        double rms = 0;
        int crossings = 0;
        for (std::size_t i = 4800; i < outL.size(); ++i)
        {
            rms += outL[i] * outL[i];
            if (outL[i - 1] < 0 && outL[i] >= 0) ++crossings;
        }
        rms = std::sqrt (rms / (double) (outL.size() - 4800));
        const double f = crossings / ((double) (outL.size() - 4800) / 48000.0);
        CHECK (ok && std::abs (rms - 0.3536) < 0.02 && std::abs (f - 1000) < 3,
               "Opus: %zu packets, rms %.3f (0.354 expected), %.1f Hz", packets.size(), rms, f);
    }
}

//==============================================================================
// Signal-to-noise of a resampled sine, fitting the expected sine (least squares).
static double sineSnrDb (const std::vector<float>& inter, std::size_t from, std::size_t to, double cyclesPerSample)
{
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (std::size_t i = from; i < to; ++i)
    {
        const double s = std::sin (2 * M_PI * cyclesPerSample * (double) i), c = std::cos (2 * M_PI * cyclesPerSample * (double) i);
        const double y = inter[i * 2];
        ss += s * s; cc += c * c; sc += s * c; ys += y * s; yc += y * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double sig = 0, err = 0;
    for (std::size_t i = from; i < to; ++i)
    {
        const double fit = a * std::sin (2 * M_PI * cyclesPerSample * (double) i) + b * std::cos (2 * M_PI * cyclesPerSample * (double) i);
        sig += fit * fit;
        err += (inter[i * 2] - fit) * (inter[i * 2] - fit);
    }
    return 10 * std::log10 (sig / std::max (err, 1e-30));
}

static void testResampler()
{
    std::printf ("Resampler\n");
    struct Case { double in, out, ppm, freq; };
    for (auto c : { Case { 44100, 48000, 0, 1000 }, Case { 48000, 48000, 500, 997 }, Case { 96000, 44100, -300, 15000 },
                    Case { 44100, 192000, 200, 19000 } })
    {
        Resampler r;
        r.setup (c.in, c.out);
        r.setCorrection (c.ppm);
        std::vector<float> l (1000), rr (1000), out;
        std::uint64_t pos = 0;
        for (int b = 0; b < (int) (c.in * 2 / 1000); ++b)
        {
            for (int i = 0; i < 1000; ++i, ++pos)
                l[(std::size_t) i] = rr[(std::size_t) i] = (float) (0.5 * std::sin (2 * M_PI * c.freq * (double) pos / c.in));
            r.process (l.data(), rr.data(), 1000, out);
        }
        const auto frames = out.size() / 2;
        const double snr = sineSnrDb (out, frames / 4, frames, c.freq * (1.0 / (1.0 + c.ppm * 1e-6)) / c.out);
        // Every input sample becomes out/in output samples, minus the half filter still ahead.
        const double expectFrames = (double) pos * c.out / c.in * (1.0 + c.ppm * 1e-6);
        CHECK (snr > 90 && std::abs ((double) frames - expectFrames) < 700,
               "%.1f -> %.1f kHz, %+.0f ppm, %.0f Hz sine: SNR %.1f dB, %zu frames", c.in / 1000, c.out / 1000, c.ppm, c.freq, snr, frames);
    }

    // Above the output's Nyquist: removed (no aliasing).
    {
        Resampler r;
        r.setup (96000, 44100);
        std::vector<float> l (96000), rr (96000), out;
        for (int i = 0; i < 96000; ++i)
            l[(std::size_t) i] = rr[(std::size_t) i] = (float) (0.5 * std::sin (2 * M_PI * 30000.0 * i / 96000.0));
        r.process (l.data(), rr.data(), l.size(), out);
        double peak = 0;
        for (std::size_t i = 2000; i < out.size(); ++i) peak = std::max (peak, (double) std::abs (out[i]));
        CHECK (20 * std::log10 (peak / 0.5 + 1e-12) < -90, "30 kHz into 44.1 kHz: %.1f dB", 20 * std::log10 (peak / 0.5 + 1e-12));
    }

    // Changing the correction on the fly: no jump in the waveform.
    {
        Resampler r;
        r.setup (44100, 48000);
        std::vector<float> l (512), rr (512), out;
        std::uint64_t pos = 0;
        std::mt19937 rng (7);
        for (int b = 0; b < 400; ++b)
        {
            r.setCorrection (std::uniform_real_distribution<double> (-1000, 1000) (rng));
            for (int i = 0; i < 512; ++i, ++pos)
                l[(std::size_t) i] = rr[(std::size_t) i] = (float) (0.5 * std::sin (2 * M_PI * 1000.0 * (double) pos / 44100.0));
            r.process (l.data(), rr.data(), 512, out);
        }
        double maxStep = 0;
        for (std::size_t i = 200; i + 1 < out.size() / 2; ++i)
            maxStep = std::max (maxStep, (double) std::abs (out[(i + 1) * 2] - out[i * 2]));
        const double ideal = 0.5 * 2 * M_PI * 1000.0 / 48000.0;
        CHECK (maxStep < ideal * 1.01, "ratio changes are smooth: largest step %.5f (sine alone: %.5f)", maxStep, ideal);
    }
}

//==============================================================================
// The studio side: a real StreamServer fed in real time, like a DAW would.
struct Studio
{
    StreamServer server;
    std::thread feeder;
    std::atomic<bool> run { true }, paused { false };
    double rate;

    Studio (double sr, Quality cap, int maxListeners = 5) : rate (sr)
    {
        StreamServer::Options o;
        o.firstPort = 47900;
        o.token = "test";
        o.dither = false;
        o.maxQuality = cap;
        o.maxListeners = maxListeners;
        std::string err;
        if (! server.start (o, err))
            std::printf ("server: %s\n", err.c_str());
        server.setSampleRate (sr);
        feeder = std::thread ([this] {
            const int block = 441;
            std::vector<float> l ((std::size_t) block), r ((std::size_t) block);
            const float* ch[2] = { l.data(), r.data() };
            std::uint64_t pos = 0;
            auto next = Clock::now();
            while (run)
            {
                next += std::chrono::microseconds ((long long) (1e6 * block / rate));
                std::this_thread::sleep_until (next);
                if (paused) continue;      // DAW stopped: no processBlock, nothing pushed
                for (int i = 0; i < block; ++i, ++pos)
                {
                    l[(std::size_t) i] = (float) (0.5 * std::sin (2 * M_PI * 1000.0 * (double) pos / rate));
                    r[(std::size_t) i] = (float) (0.25 * std::sin (2 * M_PI * 1500.0 * (double) pos / rate));
                }
                server.pushAudio (ch, 2, block);
            }
        });
    }
    ~Studio() { run = false; feeder.join(); server.stop(); }
    std::string link() const { return "http://127.0.0.1:" + std::to_string (server.getPort()) + "/test/"; }
};

// The listener's DAW: pulls 256 frames at a time, its clock "ppm" off.
struct Daw
{
    ReceiverEngine& engine;
    std::thread thread;
    std::atomic<bool> run { true };
    std::vector<float> lastL;        // the last 2 seconds
    std::mutex mutex;
    double rate;

    Daw (ReceiverEngine& e, double sr, double ppm) : engine (e), rate (sr)
    {
        engine.prepare (sr, 256);
        thread = std::thread ([this, ppm] {
            std::vector<float> l (256), r (256);
            auto next = Clock::now();
            const auto period = std::chrono::nanoseconds ((long long) (1e9 * 256 / (rate * (1.0 + ppm * 1e-6))));
            while (run)
            {
                next += period;
                std::this_thread::sleep_until (next);
                engine.render (l.data(), r.data(), 256);
                std::lock_guard<std::mutex> lock (mutex);
                lastL.insert (lastL.end(), l.begin(), l.end());
                if (lastL.size() > (std::size_t) (rate * 2))
                    lastL.erase (lastL.begin(), lastL.begin() + (std::ptrdiff_t) (lastL.size() - (std::size_t) (rate * 2)));
            }
        });
    }
    ~Daw() { run = false; thread.join(); }

    // Frequency and level of the left channel, in the DAW's own samples.
    void measure (double& freq, double& rms)
    {
        std::lock_guard<std::mutex> lock (mutex);
        int c = 0;
        double s = 0;
        for (std::size_t i = 1; i < lastL.size(); ++i)
        {
            if (lastL[i - 1] < 0 && lastL[i] >= 0) ++c;
            s += lastL[i] * lastL[i];
        }
        freq = c / ((double) lastL.size() / rate);
        rms = std::sqrt (s / (double) std::max<std::size_t> (1, lastL.size()));
    }
};

static ReceiverStatus waitFor (ReceiverEngine& e, ReceiverStatus::State s, double seconds)
{
    const auto end = Clock::now() + std::chrono::milliseconds ((long long) (seconds * 1000));
    ReceiverStatus st;
    while (Clock::now() < end)
    {
        st = e.getStatus();
        if (st.state == s) break;
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    return st;
}

static void sleepS (double s) { std::this_thread::sleep_for (std::chrono::milliseconds ((long long) (s * 1000))); }

static void testEndToEnd (bool quick)
{
    std::printf ("End to end (studio 44.1 kHz, listener DAW 48 kHz, clocks 400 ppm apart)\n");
    Studio studio (44100.0, Quality::hires);
    ReceiverEngine rx (makePlainWebSocketClient);
    rx.setBufferSeconds (0.5);
    Daw daw (rx, 48000.0, 400.0);

    std::string err;
    CHECK (rx.connect (studio.link(), err), "connect %s", studio.link().c_str());
    auto st = waitFor (rx, ReceiverStatus::State::playing, 5);
    CHECK (st.state == ReceiverStatus::State::playing, "playing (state %s, %s)", stateName (st.state), st.message.c_str());
    CHECK (st.codec == 2 && st.bits == 24 && st.streamRate == 44100 && st.studioQuality == "hires",
           "studio's choice: FLAC %d-bit %u Hz, studio quality \"%s\"", st.bits, st.streamRate, st.studioQuality.c_str());

    for (int i = 0; i < (quick ? 4 : 20); ++i)
    {
        sleepS (2);
        st = rx.getStatus();
        if (! quick)
            std::printf ("        t=%2ds buffer %.3f s, correction %+5.0f ppm\n", 2 * (i + 1), st.bufferSeconds, st.driftPpm);
    }
    double f, rms;
    daw.measure (f, rms);
    // 1 kHz sent; the DAW plays it 400 ppm faster, so it lasts 400 ppm fewer of its samples per cycle.
    CHECK (std::abs (f - 1000.0) < 3.0 && std::abs (rms - 0.3536) < 0.01, "1 kHz sine received: %.1f Hz, rms %.4f", f, rms);
    CHECK (st.dropouts == 0 && st.lostPackets == 0 && st.decodeErrors == 0,
           "no dropouts (%llu), lost packets (%llu), decoding errors (%llu)",
           (unsigned long long) st.dropouts, (unsigned long long) st.lostPackets, (unsigned long long) st.decodeErrors);
    CHECK (std::abs (st.bufferSeconds - 0.5) < 0.15, "buffer %.3f s for a 0.5 s target", st.bufferSeconds);
    if (! quick)
        CHECK (st.driftPpm > 250 && st.driftPpm < 600, "clock correction %.0f ppm (400 expected)", st.driftPpm);
    CHECK (st.listeners == 1 && st.maxListeners == 5, "listeners %d / %d", st.listeners, st.maxListeners);
    std::printf ("        %.0f kbps\n", st.kbps);

    // Quality changes (reconnects behind the scenes)
    for (auto q : { ReceiveQuality::cd, ReceiveQuality::eco, ReceiveQuality::hires })
    {
        rx.setQuality (q);
        sleepS (quick ? 3 : 5);
        st = rx.getStatus();
        daw.measure (f, rms);
        const int wantCodec = q == ReceiveQuality::eco ? 3 : 2;
        const int wantBits = q == ReceiveQuality::eco ? 0 : q == ReceiveQuality::cd ? 16 : 24;
        CHECK (st.state == ReceiverStatus::State::playing && st.codec == wantCodec && st.bits == wantBits
                   && std::abs (f - 1000.0) < 3.0,
               "quality %s: codec %d, %d-bit, %u Hz, %s, %.1f Hz, dropouts %llu",
               q == ReceiveQuality::cd ? "cd" : q == ReceiveQuality::eco ? "eco" : "hires",
               st.codec, st.bits, st.streamRate, stateName (st.state), f, (unsigned long long) st.dropouts);
    }

    // The studio stops its DAW for 3 s, then plays again: not a dropout.
    const auto dropoutsBefore = rx.getStatus().dropouts;
    studio.paused = true;
    sleepS (3);
    st = rx.getStatus();
    CHECK (st.state == ReceiverStatus::State::waiting, "studio's DAW stopped: %s", stateName (st.state));
    studio.paused = false;
    st = waitFor (rx, ReceiverStatus::State::playing, 4);
    CHECK (st.state == ReceiverStatus::State::playing && st.dropouts == dropoutsBefore,
           "plays again, not counted as a dropout (%llu)", (unsigned long long) st.dropouts);

    // Too many listeners: the second receiver is refused and keeps trying.
    {
        Studio small (48000.0, Quality::cd, 1);
        ReceiverEngine a (makePlainWebSocketClient), b (makePlainWebSocketClient);
        a.setReconnectDelaysForTests (200, 200, 3000);
        b.setReconnectDelaysForTests (200, 200, 1500);
        Daw da (a, 48000.0, 0), db (b, 48000.0, 0);
        std::string e1;
        // same port is taken by the first studio: use its own link
        a.connect (small.link(), e1);
        waitFor (a, ReceiverStatus::State::playing, 4);
        b.connect (small.link(), e1);
        auto sb = waitFor (b, ReceiverStatus::State::failed, 4);
        CHECK (a.getStatus().state == ReceiverStatus::State::playing && sb.state == ReceiverStatus::State::failed,
               "listener limit: first %s, second %s (\"%s\")", stateName (a.getStatus().state), stateName (sb.state), sb.message.c_str());
    }

    // Wrong code in the link: refused at once.
    {
        ReceiverEngine c (makePlainWebSocketClient);
        Daw dc (c, 48000.0, 0);
        std::string e1;
        c.connect ("http://127.0.0.1:" + std::to_string (studio.server.getPort()) + "/wrongcode/", e1);
        auto sc = waitFor (c, ReceiverStatus::State::failed, 4);
        CHECK (sc.state == ReceiverStatus::State::failed, "wrong link: %s (\"%s\")", stateName (sc.state), sc.message.c_str());
    }

    // The studio clicks "Stop sharing".
    studio.server.stop();
    st = waitFor (rx, ReceiverStatus::State::ended, 4);
    CHECK (st.state == ReceiverStatus::State::ended, "studio stopped sharing: %s (\"%s\")", stateName (st.state), st.message.c_str());

    rx.disconnect();
    CHECK (rx.getStatus().state == ReceiverStatus::State::idle, "disconnect -> idle");
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    const bool quick = argc > 1 && std::strcmp (argv[1], "--quick") == 0;
    testLinks();
    testDecoder();
    testResampler();
    testEndToEnd (quick);
    std::printf (failures == 0 ? "\nAll receiver tests passed.\n" : "\n%d receiver test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}
