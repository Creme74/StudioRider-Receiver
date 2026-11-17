// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// StudioRider Receiver window: same look as the listening page.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ReceiverProcessor.h"

namespace receiver_ui
{
// Colours of the listening page (web/style.css, dark theme).
const juce::Colour card      { 0xff17191e };
const juce::Colour field     { 0xff22252c };
const juce::Colour line      { 0xff2a2d35 };
const juce::Colour text      { 0xffeceef2 };
const juce::Colour muted     { 0xff8b909c };
const juce::Colour accent    { 0xffc56a6b };   // the logo's red (#A10E10), lighter
const juce::Colour accentInk { 0xff1e1212 };   // the logo's dark brown
const juce::Colour okColour        { 0xff4cd18a };
const juce::Colour waitColour      { 0xffe6b84a };
const juce::Colour errColour       { 0xffff5d5d };

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
};

// The big Connect / Disconnect button (the page's Listen / Stop button).
class PlayButton : public juce::Button
{
public:
    PlayButton() : juce::Button ("Connect") {}
    void setConnected (bool c)            { if (c != connected) { connected = c; repaint(); } }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
private:
    bool connected = false;
};

class MenuButton : public juce::Button
{
public:
    MenuButton() : juce::Button ("Menu") { setTooltip ("Menu"); }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

class Meters : public juce::Component
{
public:
    void push (float left, float right);  // peaks since the last call
    void paint (juce::Graphics&) override;
private:
    float levelL = 0, levelR = 0;
};
}

class ReceiverEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit ReceiverEditor (ReceiverProcessor&);
    ~ReceiverEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refresh();
    void toggleConnection();
    void showMenu();
    void showAbout();
    void checkForUpdate();
    void showUpdateResult (bool ok, const juce::String& tag, const juce::String& page, int status);

    ReceiverProcessor& proc;
    receiver_ui::LookAndFeel lookAndFeel;

    juce::Label brand, versionLabel, heading, format, linkCaption, qualityCaption, bufferCaption, note;
    receiver_ui::MenuButton menuButton;
    juce::TextEditor linkField;
    juce::TextButton pasteButton;
    receiver_ui::PlayButton playButton;
    receiver_ui::Meters meters;
    juce::ComboBox qualityBox, bufferBox;
    std::unique_ptr<juce::ComboBoxParameterAttachment> qualityAttachment, bufferAttachment;

    struct Stat { juce::Label name, value; };
    Stat stats[4];

    juce::String pillText;
    juce::Colour pillColour;
    juce::Rectangle<int> pillArea;

    juce::Image logo;
    juce::Rectangle<float> logoArea;
    bool revving = false;
    int tick = 0;
    juce::Random rng;
    int separatorY = 0;
    juce::String lastError;
    juce::String shownStudioQuality = "-";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReceiverEditor)
};
