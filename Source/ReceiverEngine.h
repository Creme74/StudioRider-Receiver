// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// StudioRider Receiver — the engine (no JUCE dependency).
//
//   network thread : WebSocket messages -> decode (FLAC/Opus) -> resample to
//                    the DAW's rate -> ring buffer (lock-free, single writer)
//   audio thread   : render() reads the ring buffer, never blocks, never allocates
//   control thread : connects, reconnects, keep-alives, statistics
//
// Two clocks are involved (the studio's audio interface and this one): the
// resampling ratio is nudged by a few hundred ppm at most so that the buffer
// stays at its target, instead of slowly emptying or overflowing.

#pragma once

#include "Resampler.h"
#include "StreamDecoder.h"
#include "WebSocketClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace studiorider
{

// What the listener asks the studio for. "studio" = the studio's own choice
// (the stream quality set in StudioRider); the studio never sends more than that.
enum class ReceiveQuality : int { studio = 0, hires = 1, cd = 2, eco = 3 };

struct ReceiverStatus
{
    enum class State
    {
        idle,             // not connected
        connecting,       // first attempt
        reconnecting,     // connection lost, trying again
        waiting,          // connected, the studio isn't sending audio (DAW stopped)
        buffering,        // audio arriving, filling the buffer
        playing,
        notProcessed,     // audio arriving, but the DAW isn't running the plugin
        ended,            // the studio stopped sharing
        failed            // gave up (see message)
    };

    State state = State::idle;
    std::string message;          // error or explanation, for the UI

    // Stream being received
    int codec = 0;                // 2 = FLAC, 3 = Opus, 0 = none yet
    int bits = 0;
    std::uint32_t streamRate = 0;
    std::string studioQuality;    // "hires", "cd", "eco" (the studio's stream quality)
    int listeners = 0, maxListeners = 0;

    // Playback
    double dawRate = 0.0;
    double bufferSeconds = 0.0;   // audio waiting to be played
    double targetSeconds = 0.0;
    double kbps = 0.0;
    double driftPpm = 0.0;        // current clock correction
    std::uint64_t dropouts = 0;   // the buffer ran dry while the studio was sending
    std::uint64_t lostPackets = 0;
    std::uint64_t decodeErrors = 0;
};

class ReceiverEngine
{
public:
    using ClientFactory = std::function<std::unique_ptr<WebSocketClient>()>;

    explicit ReceiverEngine (ClientFactory factory = makeWebSocketClient);
    ~ReceiverEngine();

    // Turns a listening link (https://….trycloudflare.com/<token>/) into the
    // WebSocket address, without the "ws?q=" part. Returns false + error text.
    static bool parseLink (const std::string& link, bool tlsAvailable, std::string& base, std::string& error);

    // Message thread (or any thread except the audio thread).
    bool connect (const std::string& link, std::string& error);
    void disconnect();
    bool isConnectWanted() const noexcept       { return wanted.load(); }
    void setQuality (ReceiveQuality q);         // reconnects if needed
    ReceiveQuality getQuality() const noexcept  { return (ReceiveQuality) quality.load(); }
    void setBufferSeconds (double seconds);
    double getBufferSeconds() const noexcept    { return bufferSeconds.load(); }
    ReceiverStatus getStatus() const;
    // Peak levels since the last call (0..1+), for the meters: what is played,
    // or what is received while the DAW isn't running the plugin.
    void takePeaks (float& left, float& right) noexcept;
    bool isDawRunningPlugin() const noexcept;

    // Audio side. prepare() is called when playback starts or the rate changes
    // (not concurrently with render()).
    void prepare (double sampleRate, int maximumBlockSize);
    void render (float* left, float* right, int numFrames) noexcept;

    // Test hooks
    void setReconnectDelaysForTests (int firstMs, int laterMs, int giveUpMs);

    // Diagnostics
    struct Debug
    {
        std::uint64_t renderCalls, writePos, readPos;
        bool readerPlaying;
        double dawRate;
        int resamplerReady;
    };
    Debug getDebug() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    void controlLoop();
    void openClient();               // control thread
    void onOpen (std::uint64_t gen);
    void onBinary (std::uint64_t gen, const std::uint8_t* data, std::size_t size);
    void onText (std::uint64_t gen, const std::string& text);
    void onClose (std::uint64_t gen, int code, int http, const std::string& reason);
    void setState (ReceiverStatus::State s, const std::string& msg = {});

    // Network thread, decodeMutex held
    void writeAudio (const PacketHeader& h, const std::vector<float>& l, const std::vector<float>& r);
    void rebuildResampler (std::uint32_t streamRate);
    void updateDrift (double packetSeconds);
    void resetRingLocked();

    ClientFactory factory;

    // ---- control
    mutable std::mutex controlMutex;
    std::condition_variable controlCv;
    std::thread controlThread;
    bool quitControl = false;
    std::unique_ptr<WebSocketClient> client;
    std::uint64_t generation = 0;     // ignores callbacks from an old connection
    std::atomic<std::uint64_t> liveGeneration { 0 };
    std::string base;                 // ws(s)://host/token/
    bool needOpen = false, needClose = false;
    int closeCode = 0, closeHttp = 0;
    std::string closeReason;
    bool closePending = false;
    bool everOpened = false;
    int failures = 0;
    Clock::time_point nextAttempt {}, lostSince {}, lastKeepAlive {};
    int retryFirstMs = 2000, retryLaterMs = 5000, giveUpMs = 120000;

    std::atomic<bool> wanted { false };
    std::atomic<int> quality { (int) ReceiveQuality::studio };
    std::atomic<double> bufferSeconds { 1.0 };

    // ---- status
    mutable std::mutex statusMutex;
    ReceiverStatus status;            // state, message, info fields
    std::atomic<std::uint64_t> bytesReceived { 0 };
    std::uint64_t bytesAtLastRate = 0;
    Clock::time_point lastRateTime {};
    std::atomic<std::int64_t> lastPacketNs { 0 };

    // ---- decoding (network thread)
    std::mutex decodeMutex;
    StreamDecoder decoder;
    std::vector<float> decL, decR, resampled;
    Resampler resampler;
    double appliedPpm = 0.0, integral = 0.0, emaFill = -1.0;
    Clock::time_point lastRatioChange {};
    std::int64_t expectedSeq = -1;
    std::atomic<std::uint64_t> lostPackets { 0 }, decodeErrors { 0 }, dropouts { 0 };
    std::atomic<double> driftPpm { 0.0 };
    std::atomic<int> streamCodec { 0 }, streamBits { 0 };
    std::atomic<std::uint32_t> streamRate { 0 };
    Clock::time_point previousPacket {};

    // ---- ring buffer (interleaved stereo, at the DAW's rate)
    static constexpr std::size_t kRingFrames = 1u << 20;
    std::vector<float> ring;
    std::atomic<std::uint64_t> writePos { 0 }, readPos { 0 };
    std::atomic<double> dawRate { 0.0 };
    std::atomic<bool> readerPlaying { false };
    std::atomic<bool> underrunPending { false };
    std::atomic<bool> flushRequest { false };  // audio thread: drop what's buffered (new stream)
    int fadeIn = 0;                            // audio thread only
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    std::atomic<float> rxPeakL { 0.0f }, rxPeakR { 0.0f };   // decoded, before the buffer
    std::atomic<std::int64_t> lastRenderNs { 0 };
    std::atomic<std::uint64_t> renderCalls { 0 };
};

} // namespace studiorider
