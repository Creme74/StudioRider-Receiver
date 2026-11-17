// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// StudioRider Receiver — plays a StudioRider stream inside a DAW, so that it
// can be monitored or recorded on a track.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ReceiverEngine.h"

class ReceiverProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    ReceiverProcessor();
    ~ReceiverProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    void processBlockBypassed (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
    {
        bypassedCalls.fetch_add (1, std::memory_order_relaxed);
        AudioProcessor::processBlockBypassed (b, m);
    }
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                     { return true; }

    const juce::String getName() const override        { return "StudioRider Receiver"; }
    bool acceptsMidi() const override                   { return true; }   // AU "aumf" (see CMakeLists.txt)
    bool producesMidi() const override                  { return false; }
    bool isMidiEffect() const override                  { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Message thread
    bool connect (const juce::String& link, juce::String& error);
    void disconnect();
    juce::String getLink() const                        { return link; }
    void setLink (const juce::String& l)                { link = l.trim(); }

    studiorider::ReceiverEngine& getEngine()            { return engine; }

    // Plugin parameters, so the DAW saves them with the project.
    // Quality: 0 = studio's choice, 1 = Hi-Res, 2 = CD, 3 = Opus (= studiorider::ReceiveQuality).
    juce::AudioParameterChoice& getQualityParam()       { return *qualityParam; }
    juce::AudioParameterChoice& getBufferParam()        { return *bufferParam; }
    static double bufferSecondsForIndex (int index);

private:
    void timerCallback() override;      // applies parameter changes (message thread)

    studiorider::ReceiverEngine engine;
    juce::AudioParameterChoice* qualityParam = nullptr;
    juce::AudioParameterChoice* bufferParam = nullptr;
    juce::String link;
    juce::AudioBuffer<float> scratch;   // for mono outputs

    // Diagnostics (test builds with STUDIORIDER_RECEIVER_DEBUG_LOG=1):
    // ~/Library/Logs/StudioRider/receiver.log, one line per second.
    void writeDebugLine();
    std::atomic<std::uint64_t> processCalls { 0 }, prepareCalls { 0 }, bypassedCalls { 0 };
    std::atomic<int> lastBlock { 0 }, lastIns { 0 }, lastOuts { 0 }, lastBufChannels { 0 };
    std::atomic<bool> lastNonRealtime { false };
    std::atomic<double> preparedRate { 0.0 };
    int debugTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReceiverProcessor)
};
