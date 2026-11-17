// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
#include "ReceiverEngine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace studiorider
{

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr double kMaxPpm = 1000.0;      // ±0.1 %: far below anything audible (1.7 cents)
    // The buffer only moves by 1 ms per second at 1000 ppm, so the loop is slow
    // by nature (about 30 s): plenty for clock drift, which is a few dozen ppm.
    constexpr double kKp = 30000.0;         // ppm per second of buffer error
    constexpr double kKi = 250.0;           // ppm per second² of accumulated error
    constexpr double kTau = 3.0;            // smoothing of the measured buffer (s)

    std::int64_t nowNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds> (Clock::now().time_since_epoch()).count();
    }

    std::string trim (const std::string& s)
    {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace ((unsigned char) s[a])) ++a;
        while (b > a && std::isspace ((unsigned char) s[b - 1])) --b;
        return s.substr (a, b - a);
    }

    std::string lower (std::string s)
    {
        for (auto& c : s) c = (char) std::tolower ((unsigned char) c);
        return s;
    }

    // Tiny readers for the studio's flat JSON messages ({"type":"info",...}).
    const char* findValue (const std::string& json, const char* key)
    {
        const std::string k = std::string ("\"") + key + "\"";
        auto pos = json.find (k);
        if (pos == std::string::npos) return nullptr;
        pos += k.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        return pos < json.size() ? json.c_str() + pos : nullptr;
    }

    bool jsonString (const std::string& json, const char* key, std::string& out)
    {
        const char* v = findValue (json, key);
        if (v == nullptr || *v != '"') return false;
        const char* end = std::strchr (v + 1, '"');
        if (end == nullptr) return false;
        out.assign (v + 1, end);
        return true;
    }

    bool jsonNumber (const std::string& json, const char* key, double& out)
    {
        const char* v = findValue (json, key);
        if (v == nullptr) return false;
        char* end = nullptr;
        out = std::strtod (v, &end);
        return end != v;
    }

    const char* qualityParam (ReceiveQuality q)
    {
        switch (q)
        {
            case ReceiveQuality::hires: return "hires";
            case ReceiveQuality::cd:    return "cd";
            case ReceiveQuality::eco:   return "eco";
            case ReceiveQuality::studio: break;
        }
        return nullptr;
    }
}

//==============================================================================
ReceiverEngine::ReceiverEngine (ClientFactory f)
    : factory (std::move (f)), ring (kRingFrames * 2, 0.0f)
{
    controlThread = std::thread ([this] { controlLoop(); });
}

ReceiverEngine::~ReceiverEngine()
{
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        quitControl = true;
        ++generation;
        liveGeneration.store (generation);
    }
    controlCv.notify_all();
    controlThread.join();
    if (client != nullptr)
        client->close();
    client.reset();
}

bool ReceiverEngine::parseLink (const std::string& linkIn, bool tls, std::string& out, std::string& error)
{
    auto link = trim (linkIn);
    if (link.empty())
    {
        error = "Paste the listening link the studio sent you.";
        return false;
    }

    std::string scheme = "https", rest = link;
    const auto sep = link.find ("://");
    if (sep != std::string::npos)
    {
        scheme = lower (link.substr (0, sep));
        rest = link.substr (sep + 3);
    }

    std::string wsScheme;
    if (scheme == "https" || scheme == "wss")    wsScheme = "wss";
    else if (scheme == "http" || scheme == "ws") wsScheme = "ws";
    else
    {
        error = "This isn't a web link (it should start with https://).";
        return false;
    }

    const auto cut = rest.find_first_of ("?#");
    if (cut != std::string::npos)
        rest = rest.substr (0, cut);
    const auto slash = rest.find ('/');
    const auto authority = rest.substr (0, slash);
    if (authority.empty() || authority.find_first_of (" \t@") != std::string::npos)
    {
        error = "This link isn't complete. Copy it again from the studio's message.";
        return false;
    }

    std::string token;
    if (slash != std::string::npos)
    {
        std::size_t p = slash;
        while (p < rest.size() && rest[p] == '/') ++p;
        const auto end = rest.find ('/', p);
        token = rest.substr (p, end == std::string::npos ? std::string::npos : end - p);
    }
    if (token.empty() || token == "ws" || token == "info")
    {
        error = "This link is missing its code (the part after the address). Copy it again from the studio's message.";
        return false;
    }
    if (wsScheme == "wss" && ! tls)
    {
        error = "Secure (https) links aren't supported on this system yet.";
        return false;
    }

    out = wsScheme + "://" + authority + "/" + token + "/";
    return true;
}

//==============================================================================
bool ReceiverEngine::connect (const std::string& link, std::string& error)
{
    std::string b;
    bool tls = false;
    if (auto probe = factory())
        tls = probe->supportsTls();
    if (! parseLink (link, tls, b, error))
        return false;

    {
        std::lock_guard<std::mutex> lock (controlMutex);
        base = b;
        wanted.store (true);
        needClose = needOpen = true;
        closePending = false;
        everOpened = false;
    }
    flushRequest.store (true);
    setState (ReceiverStatus::State::connecting);
    {
        std::lock_guard<std::mutex> lock (statusMutex);
        status.codec = status.bits = 0;
        status.streamRate = 0;
        status.studioQuality.clear();
        status.listeners = status.maxListeners = 0;
    }
    dropouts.store (0);
    lostPackets.store (0);
    decodeErrors.store (0);
    controlCv.notify_all();
    return true;
}

void ReceiverEngine::disconnect()
{
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        wanted.store (false);
        needClose = true;
        needOpen = false;
        closePending = false;
    }
    flushRequest.store (true);
    setState (ReceiverStatus::State::idle);
    controlCv.notify_all();
}

void ReceiverEngine::setQuality (ReceiveQuality q)
{
    if (quality.exchange ((int) q) == (int) q)
        return;
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        if (! wanted.load() || client == nullptr)
            return;
        needClose = needOpen = true; // reconnect, asking for the new quality
    }
    controlCv.notify_all();
}

void ReceiverEngine::setBufferSeconds (double s)
{
    bufferSeconds.store (std::clamp (s, 0.1, 5.0));
}

void ReceiverEngine::setReconnectDelaysForTests (int firstMs, int laterMs, int giveUp)
{
    std::lock_guard<std::mutex> lock (controlMutex);
    retryFirstMs = firstMs;
    retryLaterMs = laterMs;
    giveUpMs = giveUp;
}

void ReceiverEngine::setState (ReceiverStatus::State s, const std::string& msg)
{
    std::lock_guard<std::mutex> lock (statusMutex);
    status.state = s;
    status.message = msg;
}

ReceiverStatus ReceiverEngine::getStatus() const
{
    ReceiverStatus s;
    {
        std::lock_guard<std::mutex> lock (statusMutex);
        s = status;
    }
    const double rate = dawRate.load();
    s.dawRate = rate;
    s.targetSeconds = bufferSeconds.load();
    s.codec = streamCodec.load();
    s.bits = streamBits.load();
    s.streamRate = streamRate.load();
    const auto fill = writePos.load() - readPos.load();
    s.bufferSeconds = rate > 0 ? (double) fill / rate : 0.0;
    s.driftPpm = driftPpm.load();
    s.dropouts = dropouts.load();
    s.lostPackets = lostPackets.load();
    s.decodeErrors = decodeErrors.load();

    // "waiting" (connected), refined from what the audio is doing.
    if (s.state == ReceiverStatus::State::waiting)
    {
        const auto last = lastPacketNs.load();
        const bool recent = last != 0 && nowNs() - last < 1500000000LL;
        if (recent && ! isDawRunningPlugin())
            s.state = ReceiverStatus::State::notProcessed;
        else if (recent)
            s.state = readerPlaying.load() ? ReceiverStatus::State::playing : ReceiverStatus::State::buffering;
        else if (readerPlaying.load() && fill > 0)
            s.state = ReceiverStatus::State::playing; // still playing what's buffered
    }
    return s;
}

bool ReceiverEngine::isDawRunningPlugin() const noexcept
{
    const auto last = lastRenderNs.load();
    return last != 0 && nowNs() - last < 1000000000LL;
}

void ReceiverEngine::takePeaks (float& l, float& r) noexcept
{
    const float pl = peakL.exchange (0.0f), pr = peakR.exchange (0.0f);
    const float xl = rxPeakL.exchange (0.0f), xr = rxPeakR.exchange (0.0f);
    const bool running = isDawRunningPlugin();
    l = running ? pl : xl;
    r = running ? pr : xr;
}

//==============================================================================
// Control thread
void ReceiverEngine::controlLoop()
{
    std::unique_lock<std::mutex> lk (controlMutex);
    lastRateTime = Clock::now();
    while (true)
    {
        controlCv.wait_for (lk, std::chrono::milliseconds (100),
                            [this] { return quitControl || needOpen || needClose || closePending; });
        if (quitControl)
            break;

        auto now = Clock::now();

        if (needClose || closePending)
        {
            const bool wasPending = closePending && ! needClose;
            const int code = closeCode, http = closeHttp;
            const auto reason = closeReason;
            needClose = closePending = false;
            auto old = std::move (client);
            ++generation;
            liveGeneration.store (generation);
            lk.unlock();
            if (old != nullptr)
                old->close(); // may wait for its callbacks: never with controlMutex held
            old.reset();
            lk.lock();
            now = Clock::now();

            if (wasPending && wanted.load())
            {
                if (code == 4000)
                {
                    wanted.store (false);
                    lk.unlock();
                    setState (ReceiverStatus::State::ended, "The studio stopped sharing.");
                    lk.lock();
                }
                else
                {
                    // No WebSocket: ask the studio's info page why (like the listening page does).
                    // The server closes refused WebSockets without an answer, so this is the only
                    // way to tell a dead link or a full studio from a network hiccup.
                    int infoStatus = http;
                    std::string body;
                    if (http != 404)
                    {
                        std::string infoUrl;
                        if (base.compare (0, 3, "wss") == 0)
                            infoUrl = "https" + base.substr (3) + "info";
                        else
                            infoUrl = "http" + base.substr (2) + "info";
                        lk.unlock();
                        if (auto probe = factory())
                            infoStatus = probe->httpGet (infoUrl, body, 4000);
                        lk.lock();
                        now = Clock::now();
                        if (! wanted.load() || needOpen || needClose)
                            continue; // the user did something meanwhile
                    }

                    double listeners = 0, maxL = 0;
                    const bool full = infoStatus == 200 && jsonNumber (body, "listeners", listeners)
                                      && jsonNumber (body, "maxListeners", maxL) && maxL > 0 && listeners >= maxL;

                    if (infoStatus == 404)
                    {
                        wanted.store (false);
                        lk.unlock();
                        setState (ReceiverStatus::State::failed,
                                  "This link doesn't work anymore: the studio may have stopped sharing. Ask for a new link.");
                        lk.lock();
                        continue;
                    }

                    ++failures;
                    if (lostSince == Clock::time_point {})
                        lostSince = now;
                    if (now - lostSince > std::chrono::milliseconds (giveUpMs))
                    {
                        wanted.store (false);
                        lk.unlock();
                        setState (ReceiverStatus::State::failed,
                                  full ? "The studio already has the maximum number of listeners. Try again later."
                                       : "Can't reach the studio. Check the link and your connection, then try again.");
                        lk.lock();
                    }
                    else
                    {
                        nextAttempt = now + std::chrono::milliseconds (failures < 3 ? retryFirstMs : retryLaterMs);
                        std::string msg;
                        if (full)
                            msg = "The studio already has " + std::to_string ((int) maxL) + " listeners, the maximum. Trying again…";
                        else if (infoStatus >= 500 || (infoStatus == 0 && failures >= 3))
                            msg = "The studio isn't answering. Trying again…";
                        lk.unlock();
                        setState (everOpened ? ReceiverStatus::State::reconnecting : ReceiverStatus::State::connecting, msg);
                        lk.lock();
                    }
                }
            }
        }

        if (needOpen)
        {
            needOpen = false;
            failures = 0;
            lostSince = {};
            nextAttempt = now;
        }

        if (wanted.load() && client == nullptr && now >= nextAttempt)
            openClient();

        if (client != nullptr && now - lastKeepAlive >= std::chrono::seconds (10))
        {
            lastKeepAlive = now;
            client->sendText ("ka");
        }

        if (now - lastRateTime >= std::chrono::seconds (1))
        {
            const auto bytes = bytesReceived.load();
            const double secs = std::chrono::duration<double> (now - lastRateTime).count();
            const double kbps = (double) (bytes - bytesAtLastRate) * 8.0 / 1000.0 / secs;
            bytesAtLastRate = bytes;
            lastRateTime = now;
            std::lock_guard<std::mutex> slock (statusMutex);
            status.kbps = kbps;
        }
    }
}

void ReceiverEngine::openClient()
{
    client = factory();
    const auto gen = ++generation;
    liveGeneration.store (gen);
    lastKeepAlive = Clock::now();

    {
        std::lock_guard<std::mutex> dlock (decodeMutex);
        decoder.reset();
        expectedSeq = -1;
    }

    std::string url = base + "ws";
    if (const char* q = qualityParam ((ReceiveQuality) quality.load()))
        url += std::string ("?q=") + q;

    WebSocketClient::Callbacks cb;
    cb.onOpen   = [this, gen] { onOpen (gen); };
    cb.onBinary = [this, gen] (const std::uint8_t* d, std::size_t n) { onBinary (gen, d, n); };
    cb.onText   = [this, gen] (const std::string& t) { onText (gen, t); };
    cb.onClose  = [this, gen] (int code, int http, const std::string& reason) { onClose (gen, code, http, reason); };
    client->open (url, std::move (cb));
}

void ReceiverEngine::onOpen (std::uint64_t gen)
{
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        if (gen != generation)
            return;
        everOpened = true;
        failures = 0;
        lostSince = {};
    }
    setState (ReceiverStatus::State::waiting);
}

void ReceiverEngine::onClose (std::uint64_t gen, int code, int http, const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        if (gen != generation)
            return;
        closeCode = code;
        closeHttp = http;
        closeReason = reason;
        closePending = true;
    }
    controlCv.notify_all();
}

void ReceiverEngine::onText (std::uint64_t gen, const std::string& text)
{
    if (gen != liveGeneration.load())
        return;
    std::string type;
    jsonString (text, "type", type);
    if (type == "bye")
    {
        onClose (gen, 4000, 0, "stopped");
        return;
    }
    if (type != "info")
        return;

    std::lock_guard<std::mutex> lock (statusMutex);
    std::string q;
    if (jsonString (text, "maxQuality", q)) status.studioQuality = q;
    double v = 0;
    if (jsonNumber (text, "listeners", v))    status.listeners = (int) v;
    if (jsonNumber (text, "maxListeners", v)) status.maxListeners = (int) v;
}

void ReceiverEngine::onBinary (std::uint64_t gen, const std::uint8_t* data, std::size_t size)
{
    if (gen != liveGeneration.load())
        return;
    bytesReceived.fetch_add (size);
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lock (decodeMutex);
    decL.clear();
    decR.clear();
    PacketHeader h;
    if (! decoder.decode (data, size, h, decL, decR))
    {
        decodeErrors.fetch_add (1);
        return;
    }

    if (expectedSeq >= 0 && (std::int64_t) h.sequence != expectedSeq)
        lostPackets.fetch_add (1);
    expectedSeq = (std::int64_t) ((h.sequence + 1u) & 0xffffffffu);

    streamCodec.store (h.codec);
    streamBits.store (h.bits);
    streamRate.store (h.sampleRate);

    // A dry buffer counts as a dropout only if the studio was still sending
    // (not when its DAW was simply stopped for a while).
    if (underrunPending.exchange (false) && previousPacket != Clock::time_point {}
        && now - previousPacket < std::chrono::milliseconds (1500))
        dropouts.fetch_add (1);
    previousPacket = now;
    lastPacketNs.store (nowNs());

    float pl = 0.0f, pr = 0.0f;
    for (std::size_t i = 0; i < decL.size(); ++i)
    {
        pl = std::max (pl, std::abs (decL[i]));
        pr = std::max (pr, std::abs (decR[i]));
    }
    if (pl > rxPeakL.load()) rxPeakL.store (pl);
    if (pr > rxPeakR.load()) rxPeakR.store (pr);

    writeAudio (h, decL, decR);
}

//==============================================================================
// Network thread, decodeMutex held
void ReceiverEngine::rebuildResampler (std::uint32_t inRate)
{
    const double outRate = dawRate.load();
    if (outRate <= 0)
        return;
    resampler.setup ((double) inRate, outRate);
    resampler.setCorrection (appliedPpm); // keep what was learnt of the clock difference
    lastRatioChange = Clock::now();
}

void ReceiverEngine::resetRingLocked()
{
    writePos.store (0);
    readPos.store (0);
    readerPlaying.store (false);
    emaFill = -1.0;
    integral = 0.0;
}

void ReceiverEngine::writeAudio (const PacketHeader& h, const std::vector<float>& l, const std::vector<float>& r)
{
    const double outRate = dawRate.load();
    const auto n = l.size();
    if (outRate <= 0 || n == 0)
        return;

    if (! resampler.isSetUp() || resampler.getInRate() != (double) h.sampleRate || resampler.getOutRate() != outRate)
        rebuildResampler (h.sampleRate);
    if (! resampler.isSetUp())
        return;

    resampled.clear();
    resampler.process (l.data(), r.data(), n, resampled);
    const auto outLen = resampled.size() / 2;

    const auto w = writePos.load (std::memory_order_relaxed);
    const auto rd = readPos.load (std::memory_order_acquire);
    const auto space = kRingFrames - (std::size_t) (w - rd);
    const auto count = std::min<std::size_t> (outLen, space);
    constexpr auto mask = kRingFrames - 1;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto idx = (std::size_t) ((w + i) & mask) * 2;
        ring[idx] = resampled[i * 2];
        ring[idx + 1] = resampled[i * 2 + 1];
    }
    writePos.store (w + count, std::memory_order_release);

    updateDrift ((double) n / h.sampleRate);
}

void ReceiverEngine::updateDrift (double dt)
{
    const double rate = dawRate.load();
    if (! readerPlaying.load() || rate <= 0)
    {
        emaFill = -1.0; // measure again once playback starts (keep what was learnt of the drift)
        return;
    }

    const double fill = (double) (writePos.load() - readPos.load()) / rate;
    // Measured right after a write, like when playback starts (render() starts
    // at exactly the target right after a packet): same reference both ways.
    const double setpoint = bufferSeconds.load();
    if (emaFill < 0)
        emaFill = fill;
    else
        emaFill += std::min (1.0, dt / kTau) * (fill - emaFill);

    const double err = emaFill - setpoint;
    integral = std::clamp (integral + err * dt, -kMaxPpm / kKi, kMaxPpm / kKi);
    const double ppm = std::clamp (-(kKp * err + kKi * integral), -kMaxPpm, kMaxPpm);

    const auto now = Clock::now();
    if (std::abs (ppm - appliedPpm) >= 1.0 && now - lastRatioChange >= std::chrono::milliseconds (200))
    {
        resampler.setCorrection (ppm);
        appliedPpm = ppm;
        driftPpm.store (ppm);
        lastRatioChange = now;
    }
}

//==============================================================================
void ReceiverEngine::prepare (double sampleRate, int)
{
    std::lock_guard<std::mutex> lock (decodeMutex);
    dawRate.store (sampleRate);
    resetRingLocked();
    resampler = Resampler();
    flushRequest.store (false);
    fadeIn = 0;
}

ReceiverEngine::Debug ReceiverEngine::getDebug() const noexcept
{
    return { renderCalls.load(), writePos.load(), readPos.load(), readerPlaying.load(), dawRate.load(),
             resampler.isSetUp() ? 1 : 0 };
}

void ReceiverEngine::render (float* left, float* right, int numFrames) noexcept
{
    renderCalls.fetch_add (1, std::memory_order_relaxed);
    lastRenderNs.store (nowNs(), std::memory_order_relaxed);
    if (numFrames <= 0)
        return;
    constexpr auto mask = kRingFrames - 1;
    const double rate = dawRate.load (std::memory_order_relaxed);
    auto r = readPos.load (std::memory_order_relaxed);
    auto w = writePos.load (std::memory_order_acquire);

    auto silence = [&] (int from)
    {
        for (int i = from; i < numFrames; ++i)
            left[i] = right[i] = 0.0f;
    };

    if (flushRequest.exchange (false) || rate <= 0)
    {
        readPos.store (w, std::memory_order_release);
        readerPlaying.store (false);
        silence (0);
        return;
    }

    const auto target = (std::uint64_t) std::max (1.0, bufferSeconds.load (std::memory_order_relaxed) * rate);
    const int fadeLen = std::max (16, (int) (rate * 0.005)); // 5 ms
    bool playing = readerPlaying.load (std::memory_order_relaxed);
    auto fill = w - r;

    if (! playing)
    {
        if (fill < target)
        {
            silence (0);
            return;
        }
        playing = true;
        fadeIn = fadeLen;
        // Start from the most recent audio, at the target delay.
        r = w - target;
        fill = target;
    }

    int out = 0;
    // Too much in the buffer (a burst after a network hiccup): jump back to the
    // target delay with a short crossfade instead of staying late forever.
    const auto high = target * 2 + (std::uint64_t) (rate * 0.5);
    if (fill > high && fill >= (std::uint64_t) fadeLen)
    {
        const auto jumpTo = w - target;
        const int xf = std::min (numFrames, fadeLen);
        for (int i = 0; i < xf; ++i)
        {
            const float a = (float) (i + 1) / (float) (xf + 1);
            const auto o = (std::size_t) ((r + (std::uint64_t) i) & mask) * 2;
            const auto nIdx = (std::size_t) ((jumpTo + (std::uint64_t) i) & mask) * 2;
            left[i]  = ring[o] * (1.0f - a) + ring[nIdx] * a;
            right[i] = ring[o + 1] * (1.0f - a) + ring[nIdx + 1] * a;
        }
        out = xf;
        r = jumpTo + (std::uint64_t) xf;
        fill = w - r;
    }

    const int want = numFrames - out;
    const int count = (int) std::min<std::uint64_t> ((std::uint64_t) want, fill);
    for (int i = 0; i < count; ++i)
    {
        const auto idx = (std::size_t) ((r + (std::uint64_t) i) & mask) * 2;
        float g = 1.0f;
        if (fadeIn > 0)
        {
            g = 1.0f - (float) fadeIn / (float) fadeLen;
            --fadeIn;
        }
        left[out + i]  = ring[idx] * g;
        right[out + i] = ring[idx + 1] * g;
    }
    r += (std::uint64_t) count;
    out += count;

    if (out < numFrames)
    {
        // Ran dry: fade out what we have, then silence until the buffer refills.
        const int f = std::min (out, 64);
        for (int i = 0; i < f; ++i)
        {
            const float g = (float) i / (float) f;
            left[out - f + i] *= g;
            right[out - f + i] *= g;
        }
        silence (out);
        playing = false;
        underrunPending.store (true);
    }

    readPos.store (r, std::memory_order_release);
    readerPlaying.store (playing, std::memory_order_relaxed);

    float pl = 0.0f, pr = 0.0f;
    for (int i = 0; i < numFrames; ++i)
    {
        pl = std::max (pl, std::abs (left[i]));
        pr = std::max (pr, std::abs (right[i]));
    }
    if (pl > peakL.load (std::memory_order_relaxed)) peakL.store (pl, std::memory_order_relaxed);
    if (pr > peakR.load (std::memory_order_relaxed)) peakR.store (pr, std::memory_order_relaxed);
}

} // namespace studiorider
