// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
#include "StreamDecoder.h"

#include <FLAC/stream_decoder.h>
#include <opus.h>

#include <algorithm>
#include <cstring>

namespace studiorider
{

namespace
{
    std::uint32_t readLE32 (const std::uint8_t* p) noexcept
    {
        return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24);
    }

    constexpr std::size_t kHeaderSize = 20;
    constexpr std::uint8_t kCodecFlac = 2, kCodecOpus = 3;
}

bool parsePacketHeader (const std::uint8_t* d, std::size_t size, PacketHeader& h) noexcept
{
    if (d == nullptr || size < kHeaderSize || std::memcmp (d, "MXS2", 4) != 0)
        return false;
    h.codec = d[4];
    h.channels = d[5];
    h.bits = d[6];
    h.quality = d[7];
    h.sampleRate = readLE32 (d + 8);
    h.frames = readLE32 (d + 12);
    h.sequence = readLE32 (d + 16);
    return (h.codec == kCodecFlac || h.codec == kCodecOpus)
        && h.channels >= 1 && h.channels <= 8
        && h.sampleRate >= 8000 && h.sampleRate <= 768000;
}

//==============================================================================
// StudioRider sends bare FLAC frames, without the "fLaC" file header. libFLAC
// wants a complete stream: we hand it a made-up header (STREAMINFO block built
// from the message header) followed by the frames, one message at a time.
struct FlacPart
{
    FLAC__StreamDecoder* dec = nullptr;
    std::uint32_t rate = 0;
    int channels = 0, bits = 0;

    std::vector<std::uint8_t> input;
    std::size_t inputPos = 0;
    std::vector<float>* outL = nullptr;
    std::vector<float>* outR = nullptr;
    bool failed = false;

    ~FlacPart() { close(); }

    void close()
    {
        if (dec != nullptr)
        {
            FLAC__stream_decoder_finish (dec);
            FLAC__stream_decoder_delete (dec);
            dec = nullptr;
        }
        rate = 0;
    }

    bool open (std::uint32_t sr, int ch, int bps)
    {
        close();
        dec = FLAC__stream_decoder_new();
        if (dec == nullptr)
            return false;
        FLAC__stream_decoder_set_md5_checking (dec, false);
        if (FLAC__stream_decoder_init_stream (dec, &onRead, nullptr, nullptr, nullptr, nullptr,
                                              &onWrite, nullptr, &onError, this)
            != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        {
            close();
            return false;
        }
        rate = sr;
        channels = ch;
        bits = bps;

        // "fLaC" + STREAMINFO (last metadata block, 34 bytes).
        std::uint8_t h[4 + 4 + 34] = {};
        std::memcpy (h, "fLaC", 4);
        h[4] = 0x80;              // last block, type 0 (STREAMINFO)
        h[7] = 34;                // length
        std::uint8_t* s = h + 8;
        s[0] = 0x00; s[1] = 0x10;  // min block size 16
        s[2] = 0xff; s[3] = 0xff;  // max block size 65535
        // min/max frame size: 0 (unknown)
        s[10] = (std::uint8_t) (sr >> 12);
        s[11] = (std::uint8_t) (sr >> 4);
        s[12] = (std::uint8_t) (((sr & 0x0f) << 4) | (((unsigned) ch - 1) << 1) | (((unsigned) bps - 1) >> 4));
        s[13] = (std::uint8_t) ((((unsigned) bps - 1) & 0x0f) << 4); // total samples: 0 (unknown)
        // MD5: zeros (not checked)
        input.assign (h, h + sizeof (h));
        inputPos = 0;
        failed = false;
        if (! FLAC__stream_decoder_process_until_end_of_metadata (dec) || failed)
        {
            close();
            return false;
        }
        return true;
    }

    bool decodeFrame (const std::uint8_t* data, std::size_t size, std::vector<float>& l, std::vector<float>& r)
    {
        input.assign (data, data + size);
        inputPos = 0;
        outL = &l;
        outR = &r;
        failed = false;
        while (inputPos < input.size())
        {
            const auto before = inputPos;
            if (! FLAC__stream_decoder_process_single (dec))
                failed = true;
            const auto state = FLAC__stream_decoder_get_state (dec);
            if (failed || state == FLAC__STREAM_DECODER_END_OF_STREAM || state == FLAC__STREAM_DECODER_ABORTED)
            {
                failed = true;
                break;
            }
            if (inputPos == before)
                break;
        }
        outL = outR = nullptr;
        return ! failed;
    }

    static FLAC__StreamDecoderReadStatus onRead (const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* client)
    {
        auto* self = static_cast<FlacPart*> (client);
        const auto left = self->input.size() - self->inputPos;
        if (left == 0)
        {
            // Only happens if a frame is cut short: stop here rather than wait.
            *bytes = 0;
            self->failed = true;
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        const auto n = std::min (left, *bytes);
        std::memcpy (buffer, self->input.data() + self->inputPos, n);
        self->inputPos += n;
        *bytes = n;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    static FLAC__StreamDecoderWriteStatus onWrite (const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                                   const FLAC__int32* const buffer[], void* client)
    {
        auto* self = static_cast<FlacPart*> (client);
        if (self->outL == nullptr)
            return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
        const unsigned n = frame->header.blocksize;
        const unsigned ch = frame->header.channels;
        const unsigned bps = frame->header.bits_per_sample != 0 ? frame->header.bits_per_sample : (unsigned) self->bits;
        const float scale = 1.0f / (float) (1u << (bps - 1));
        const auto* l = buffer[0];
        const auto* r = buffer[ch > 1 ? 1 : 0];
        auto& L = *self->outL;
        auto& R = *self->outR;
        const auto base = L.size();
        L.resize (base + n);
        R.resize (base + n);
        for (unsigned i = 0; i < n; ++i)
        {
            L[base + i] = (float) l[i] * scale;
            R[base + i] = (float) r[i] * scale;
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void onError (const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* client)
    {
        static_cast<FlacPart*> (client)->failed = true;
    }
};

//==============================================================================
struct OpusPart
{
    OpusDecoder* dec = nullptr;
    int channels = 0;
    std::vector<float> pcm;

    ~OpusPart() { close(); }

    void close()
    {
        if (dec != nullptr)
            opus_decoder_destroy (dec);
        dec = nullptr;
        channels = 0;
    }

    bool open (int ch)
    {
        close();
        int err = 0;
        dec = opus_decoder_create (48000, ch, &err);
        if (err != OPUS_OK || dec == nullptr)
        {
            dec = nullptr;
            return false;
        }
        channels = ch;
        pcm.resize ((std::size_t) 5760 * (std::size_t) ch); // longest Opus packet: 120 ms
        return true;
    }

    // payload: a run of [u16 size, little-endian][Opus packet]
    bool decodeRun (const std::uint8_t* p, std::size_t size, std::vector<float>& L, std::vector<float>& R)
    {
        bool ok = true;
        for (std::size_t pos = 0; pos + 2 <= size;)
        {
            const std::size_t n = (std::size_t) p[pos] | ((std::size_t) p[pos + 1] << 8);
            pos += 2;
            if (pos + n > size)
                return false;
            const int frames = opus_decode_float (dec, p + pos, (opus_int32) n, pcm.data(), 5760, 0);
            pos += n;
            if (frames < 0)
            {
                ok = false;
                continue;
            }
            const auto base = L.size();
            L.resize (base + (std::size_t) frames);
            R.resize (base + (std::size_t) frames);
            for (int i = 0; i < frames; ++i)
            {
                L[base + (std::size_t) i] = pcm[(std::size_t) (i * channels)];
                R[base + (std::size_t) i] = pcm[(std::size_t) (i * channels + (channels > 1 ? 1 : 0))];
            }
        }
        return ok;
    }
};

struct StreamDecoder::Impl
{
    FlacPart flac;
    OpusPart opus;
};

StreamDecoder::StreamDecoder() : impl (std::make_unique<Impl>()) {}
StreamDecoder::~StreamDecoder() = default;

void StreamDecoder::reset()
{
    impl->flac.close();
    impl->opus.close();
}

bool StreamDecoder::decode (const std::uint8_t* data, std::size_t size, PacketHeader& h,
                            std::vector<float>& left, std::vector<float>& right)
{
    if (! parsePacketHeader (data, size, h))
    {
        ++errors;
        lastError = "Unknown message";
        return false;
    }
    const auto* payload = data + kHeaderSize;
    const auto payloadSize = size - kHeaderSize;

    if (h.codec == kCodecFlac)
    {
        if (h.bits != 16 && h.bits != 24)
        {
            ++errors;
            lastError = "FLAC: unsupported bit depth";
            return false;
        }
        auto& f = impl->flac;
        if (f.dec == nullptr || f.rate != h.sampleRate || f.channels != h.channels || f.bits != h.bits)
        {
            impl->opus.close();
            if (! f.open (h.sampleRate, h.channels, h.bits))
            {
                ++errors;
                lastError = "FLAC: decoder unavailable";
                return false;
            }
        }
        if (! f.decodeFrame (payload, payloadSize, left, right))
        {
            ++errors;
            lastError = "FLAC: damaged frame";
            f.close(); // start again cleanly on the next frame
            return false;
        }
        return true;
    }

    // Opus: always 48 kHz
    h.sampleRate = 48000;
    auto& o = impl->opus;
    if (o.dec == nullptr || o.channels != h.channels)
    {
        impl->flac.close();
        if (! o.open (h.channels))
        {
            ++errors;
            lastError = "Opus: decoder unavailable";
            return false;
        }
    }
    if (! o.decodeRun (payload, payloadSize, left, right))
    {
        ++errors;
        lastError = "Opus: damaged packet";
        return false;
    }
    return true;
}

} // namespace studiorider
