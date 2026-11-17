// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// StudioRider Receiver — decodes the "MXS2" audio messages sent by StudioRider
// (see StreamServer.h for the format): FLAC 16/24-bit frames or runs of Opus
// packets. No JUCE dependency.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace studiorider
{

struct PacketHeader
{
    std::uint8_t codec = 0;          // 2 = FLAC, 3 = Opus
    std::uint8_t channels = 0;
    std::uint8_t bits = 0;           // 16 or 24 for FLAC, 0 for Opus
    std::uint8_t quality = 0;        // 0 = cd, 1 = hires, 2 = eco
    std::uint32_t sampleRate = 0;
    std::uint32_t frames = 0;
    std::uint32_t sequence = 0;
};

// Reads the 20-byte header. Returns false if this is not an MXS2 audio message.
bool parsePacketHeader (const std::uint8_t* data, std::size_t size, PacketHeader& out) noexcept;

class StreamDecoder
{
public:
    StreamDecoder();
    ~StreamDecoder();

    // Decodes one whole message (header included). The audio is appended to
    // "left"/"right" at the stream's own rate (header.sampleRate, 48 kHz for Opus).
    // The decoder follows format changes (quality, codec, sample rate) by itself.
    // Returns false on a decoding error (the message is dropped; the next one
    // is decoded normally).
    bool decode (const std::uint8_t* data, std::size_t size, PacketHeader& header,
                 std::vector<float>& left, std::vector<float>& right);

    void reset();                    // forget the current stream (new connection)
    std::uint64_t getErrors() const noexcept    { return errors; }
    std::string getLastError() const            { return lastError; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    std::uint64_t errors = 0;
    std::string lastError;
};

} // namespace studiorider
