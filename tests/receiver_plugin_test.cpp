// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// StudioRider Receiver plugin test (JUCE, no DAW): a real StreamServer plays a
// sine, the plugin is driven like a DAW would (prepareToPlay + processBlock in
// real time), its state is saved and restored, and its window is captured.
// Usage: studiorider_receiver_plugin_test [folder for the screenshots]

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "ReceiverEditor.h"
#include "ReceiverProcessor.h"
#include "StreamServer.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

static int fails = 0;
static void check (bool c, const juce::String& m)
{
    std::cout << (c ? "  ok    " : "  FAIL  ") << m << "\n";
    if (! c) ++fails;
}

static void pump (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); }

// Runs on the message thread and waits (the scenario runs on its own thread).
template <typename Fn>
static void onUi (Fn&& fn)
{
    juce::WaitableEvent done;
    juce::MessageManager::callAsync ([&] { fn(); done.signal(); });
    done.wait();
}

static int scenario (int argc, char** argv);

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    int result = 1;
    std::thread t ([&] { result = scenario (argc, argv); juce::MessageManager::getInstance()->stopDispatchLoop(); });
    juce::MessageManager::getInstance()->runDispatchLoop();
    t.join();
    return result;
}

static int scenario (int argc, char** argv)
{
    const juce::File shots (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());

    // --- The studio: StudioRider's server, fed with a sine in real time.
    studiorider::StreamServer server;
    studiorider::StreamServer::Options o;
    o.firstPort = 47960;
    o.token = "k3Fq9Lw2pZ7xA1mT0vRbYc";
    o.maxQuality = studiorider::Quality::cd;
    std::string err;
    server.start (o, err);
    server.setSampleRate (44100.0);
    std::atomic<bool> run { true }, dawPaused { false };
    std::thread studio ([&]
    {
        std::vector<float> l (441), r (441);
        const float* ch[2] = { l.data(), r.data() };
        std::uint64_t pos = 0;
        auto next = std::chrono::steady_clock::now();
        while (run)
        {
            next += std::chrono::milliseconds (10);
            std::this_thread::sleep_until (next);
            for (int i = 0; i < 441; ++i, ++pos)
            {
                l[(size_t) i] = (float) (0.7 * std::sin (2 * M_PI * 220.0 * (double) pos / 44100.0) * (0.6 + 0.4 * std::sin ((double) pos / 30000.0)));
                r[(size_t) i] = (float) (0.65 * std::sin (2 * M_PI * 330.0 * (double) pos / 44100.0) * (0.6 + 0.4 * std::cos ((double) pos / 26000.0)));
            }
            server.pushAudio (ch, 2, 441);
        }
    });
    const juce::String link = "http://127.0.0.1:" + juce::String (server.getPort()) + "/" + juce::String (o.token) + "/";

    std::cout << "Plugin\n";
    std::unique_ptr<ReceiverProcessor> proc (static_cast<ReceiverProcessor*> (createPluginFilter()));
    check (proc->getName() == "StudioRider Receiver", "name: " + proc->getName());
    check (proc->getParameters().size() == 2, "two parameters (quality, buffer)");
    check (proc->getQualityParam().getIndex() == 0 && proc->getBufferParam().getCurrentChoiceName() == "0.5 s",
           "defaults: studio's choice, 0.5 s");

    // State: link and settings come back, nothing connects by itself.
    {
        proc->setLink ("  https://x.trycloudflare.com/abc/ ");
        proc->getQualityParam() = 2;
        proc->getBufferParam() = 2;
        juce::MemoryBlock state;
        proc->getStateInformation (state);
        std::unique_ptr<ReceiverProcessor> b (static_cast<ReceiverProcessor*> (createPluginFilter()));
        b->setStateInformation (state.getData(), (int) state.getSize());
        check (b->getLink() == "https://x.trycloudflare.com/abc/" && b->getQualityParam().getIndex() == 2
                   && b->getBufferParam().getIndex() == 2 && ! b->getEngine().isConnectWanted(),
               "project reopened: link, CD, 1 s restored, not connected");
        proc->getQualityParam() = 0;
        proc->getBufferParam() = 1;
    }

    // The DAW: 48 kHz, 256-sample blocks, in real time.
    proc->setPlayConfigDetails (2, 2, 48000.0, 256);
    proc->prepareToPlay (48000.0, 256);
    std::atomic<double> rms { 0 };
    std::thread daw ([&]
    {
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        auto next = std::chrono::steady_clock::now();
        double acc = 0;
        int blocks = 0;
        while (run)
        {
            next += std::chrono::microseconds (5333);
            std::this_thread::sleep_until (next);
            if (dawPaused) continue;   // like a DAW that stops running the plugin
            buf.clear();
            proc->processBlock (buf, midi);
            acc += buf.getRMSLevel (0, 0, 256);
            if (++blocks == 50) { rms = acc / 50; acc = 0; blocks = 0; }
        }
    });

    proc->setLink (link);
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    onUi ([&] { editor.reset (proc->createEditor()); editor->setVisible (true); });
    pump (400);
    auto snap = [&] (const juce::String& name)
    {
        onUi ([&] {
        const auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        auto f = shots.getChildFile (name);
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::PNGImageFormat().writeImageToStream (img, out);
        std::cout << "        screenshot: " << f.getFullPathName() << "\n";
        });
    };
    check (editor->getWidth() == 460 && editor->getHeight() == 600, "window 460 x 600");
    snap ("receiver-plugin-idle.png");

    juce::String e;
    bool connected = false;
    onUi ([&] { connected = proc->connect (link, e); });
    check (connected, "connect");
    pump (4000);
    auto st = proc->getEngine().getStatus();
    check (st.state == studiorider::ReceiverStatus::State::playing && st.bits == 16 && st.streamRate == 44100,
           "playing FLAC " + juce::String (st.bits) + "-bit " + juce::String (st.streamRate) + " Hz");
    check (rms.load() > 0.05, "sound at the plugin's output (rms " + juce::String (rms.load(), 3) + ")");
    snap ("receiver-plugin-playing.png");

    // The DAW stops running the plugin (an audio or aux track with no signal):
    // said in the window, and the meters show what is received.
    dawPaused = true;
    pump (2500);
    st = proc->getEngine().getStatus();
    check (st.state == studiorider::ReceiverStatus::State::notProcessed, "DAW not running the plugin: told");
    check (! proc->getEngine().isDawRunningPlugin(), "the engine sees that the DAW stopped (meters switch to the received sound)");
    snap ("receiver-plugin-not-processed.png");
    dawPaused = false;
    pump (1500);
    check (proc->getEngine().getStatus().state == studiorider::ReceiverStatus::State::playing, "plays again when the DAW runs it");

    // An expired link: refused, explained.
    onUi ([&] { proc->disconnect(); proc->connect ("http://127.0.0.1:" + juce::String (server.getPort()) + "/expired/", e); });
    pump (2500);
    st = proc->getEngine().getStatus();
    check (st.state == studiorider::ReceiverStatus::State::failed, "expired link: " + juce::String (st.message));
    snap ("receiver-plugin-expired.png");

    onUi ([&] { editor.reset(); });
    run = false;
    daw.join();
    studio.join();
    onUi ([&] { proc.reset(); });
    server.stop();
    std::cout << (fails == 0 ? "\nAll plugin tests passed.\n" : "\nSome plugin tests FAILED.\n");
    return fails == 0 ? 0 : 1;
}
