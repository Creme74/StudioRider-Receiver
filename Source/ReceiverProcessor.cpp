// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
#include "ReceiverProcessor.h"
#include "ReceiverEditor.h"

#include <limits>

namespace
{
    const double kBufferChoices[] = { 0.25, 0.5, 1.0, 2.0 };
    constexpr int kDefaultBuffer = 1;   // 0.5 s, like the listening page
}

ReceiverProcessor::ReceiverProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (qualityParam = new juce::AudioParameterChoice (
        juce::ParameterID { "quality", 1 },
        "Quality",
        juce::StringArray { juce::String::fromUTF8 ("Studio’s choice"),
                            juce::String::fromUTF8 ("Hi-Res · FLAC 24-bit"),
                            juce::String::fromUTF8 ("CD · FLAC 16-bit"),
                            juce::String::fromUTF8 ("Data saver · Opus 256 kbps") },
        0));
    addParameter (bufferParam = new juce::AudioParameterChoice (
        juce::ParameterID { "buffer", 1 },
        "Buffer",
        juce::StringArray { "0.25 s", "0.5 s", "1 s", "2 s" },
        kDefaultBuffer));

    engine.setBufferSeconds (bufferSecondsForIndex (kDefaultBuffer));
    // Parameter changes can come from the host on any thread (automation,
    // project loading): they reach the engine from the message thread.
    startTimerHz (10);
}

ReceiverProcessor::~ReceiverProcessor()
{
    stopTimer();
    engine.disconnect();
}

double ReceiverProcessor::bufferSecondsForIndex (int i)
{
    return kBufferChoices[juce::jlimit (0, (int) std::size (kBufferChoices) - 1, i)];
}

#ifndef STUDIORIDER_RECEIVER_DEBUG_LOG
 #define STUDIORIDER_RECEIVER_DEBUG_LOG 0
#endif

void ReceiverProcessor::writeDebugLine()
{
    static const auto file = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                 .getChildFile ("Library/Logs/StudioRider/receiver.log");
    static juce::uint64 lastProcess = 0, lastRender = 0;
    file.getParentDirectory().createDirectory();
    if (file.getSize() > 2 * 1024 * 1024)
        file.deleteFile();

    const auto st = engine.getStatus();
    const auto d = engine.getDebug();
    const auto pc = processCalls.load();
    const char* states[] = { "idle", "connecting", "reconnecting", "waiting", "buffering", "playing", "notProcessed", "ended", "failed" };
    juce::String line;
    line << juce::Time::getCurrentTime().toString (false, true, true, true)
         << " build=" << __DATE__ << " " << __TIME__
         << " wrapper=" << juce::AudioProcessor::getWrapperTypeDescription (wrapperType)
         << " inst=" << juce::String::toHexString ((juce::pointer_sized_int) this)
         << " state=" << states[(int) st.state] << " msg=\"" << juce::String::fromUTF8 (st.message.c_str()) << "\""
         << " prepare#=" << (juce::int64) prepareCalls.load() << " prepRate=" << preparedRate.load()
         << " process#=" << (juce::int64) pc << " (+" << (juce::int64) (pc - lastProcess) << "/s)"
         << " bypassed#=" << (juce::int64) bypassedCalls.load()
         << " render#=" << (juce::int64) d.renderCalls << " (+" << (juce::int64) (d.renderCalls - lastRender) << "/s)"
         << " block=" << lastBlock.load() << " bufCh=" << lastBufChannels.load()
         << " ins=" << lastIns.load() << " outs=" << lastOuts.load()
         << " layout=" << getMainBusNumInputChannels() << "->" << getMainBusNumOutputChannels()
         << " nonRealtime=" << (lastNonRealtime.load() ? 1 : 0) << " suspended=" << (isSuspended() ? 1 : 0)
         << " bypassed=" << (getBypassParameter() != nullptr && getBypassParameter()->getValue() > 0.5f ? 1 : 0)
         << " dawRate=" << d.dawRate << " w=" << (juce::int64) d.writePos << " r=" << (juce::int64) d.readPos
         << " fill=" << juce::String (st.bufferSeconds, 3) << "s playing=" << (d.readerPlaying ? 1 : 0)
         << " resampler=" << d.resamplerReady << " codec=" << st.codec << "/" << st.bits << "/" << (int) st.streamRate
         << " kbps=" << juce::roundToInt (st.kbps) << " tail=" << getTailLengthSeconds() << "\n";
    lastProcess = pc;
    lastRender = d.renderCalls;
    file.appendText (line, false, false, "\n");
}

void ReceiverProcessor::timerCallback()
{
    if (STUDIORIDER_RECEIVER_DEBUG_LOG && ++debugTicks % 10 == 0)
        writeDebugLine();

    const auto q = (studiorider::ReceiveQuality) qualityParam->getIndex();
    if (engine.getQuality() != q)
        engine.setQuality (q);
    const auto b = bufferSecondsForIndex (bufferParam->getIndex());
    if (std::abs (engine.getBufferSeconds() - b) > 1e-6)
        engine.setBufferSeconds (b);
}

double ReceiverProcessor::getTailLengthSeconds() const
{
    // The plugin makes sound while its input is silent. Hosts stop calling an
    // effect once its input has been silent for longer than its tail (CPU
    // saving, in AU and VST3 hosts alike): an endless tail keeps it running.
    return std::numeric_limits<double>::infinity();
}

void ReceiverProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepareCalls.fetch_add (1);
    preparedRate.store (sampleRate);
    engine.prepare (sampleRate, samplesPerBlock);
    scratch.setSize (2, juce::jmax (1, samplesPerBlock));
}

bool ReceiverProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    // Same layouts as StudioRider itself: as many inputs as outputs. Offering a
    // "no input" layout too (0 -> 2) makes the plugin look like a generator,
    // and a DAW may then never run it as an effect.
    return layouts.getMainInputChannelSet() == out;
}

void ReceiverProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int outs = getTotalNumOutputChannels();
    processCalls.fetch_add (1, std::memory_order_relaxed);
    lastBlock.store (n, std::memory_order_relaxed);
    lastOuts.store (outs, std::memory_order_relaxed);
    lastIns.store (getTotalNumInputChannels(), std::memory_order_relaxed);
    lastBufChannels.store (buffer.getNumChannels(), std::memory_order_relaxed);
    lastNonRealtime.store (isNonRealtime(), std::memory_order_relaxed);

    // The track's own input is replaced by the stream.
    if (outs >= 2)
    {
        engine.render (buffer.getWritePointer (0), buffer.getWritePointer (1), n);
        for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, 0, n);
        return;
    }

    // Mono track: both channels mixed (blocks bigger than announced are split).
    for (int pos = 0; pos < n;)
    {
        const int len = juce::jmin (n - pos, scratch.getNumSamples());
        engine.render (scratch.getWritePointer (0), scratch.getWritePointer (1), len);
        if (buffer.getNumChannels() > 0)
        {
            auto* out = buffer.getWritePointer (0, pos);
            const auto* l = scratch.getReadPointer (0);
            const auto* r = scratch.getReadPointer (1);
            for (int i = 0; i < len; ++i)
                out[i] = 0.5f * (l[i] + r[i]);
        }
        pos += len;
    }
}

//==============================================================================
bool ReceiverProcessor::connect (const juce::String& l, juce::String& error)
{
    setLink (l);
    std::string err;
    if (! engine.connect (link.toStdString(), err))
    {
        error = juce::String::fromUTF8 (err.c_str());
        return false;
    }
    return true;
}

void ReceiverProcessor::disconnect()
{
    engine.disconnect();
}

//==============================================================================
void ReceiverProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Only the link and the settings: opening a project never connects by itself.
    juce::ValueTree state ("StudioRiderReceiver");
    state.setProperty ("link", link, nullptr);
    state.setProperty ("quality", qualityParam->getIndex(), nullptr);
    state.setProperty ("buffer", bufferParam->getIndex(), nullptr);
    juce::MemoryOutputStream out (destData, false);
    state.writeToStream (out);
}

void ReceiverProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (! state.hasType ("StudioRiderReceiver"))
        return;
    link = state.getProperty ("link").toString();
    *qualityParam = juce::jlimit (0, 3, (int) state.getProperty ("quality", 0));
    *bufferParam = juce::jlimit (0, 3, (int) state.getProperty ("buffer", kDefaultBuffer));
}

//==============================================================================
juce::AudioProcessorEditor* ReceiverProcessor::createEditor()
{
    return new ReceiverEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReceiverProcessor();
}
