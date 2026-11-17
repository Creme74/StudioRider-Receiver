// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
#include "ReceiverEditor.h"
#include "ReceiverBinaryData.h"

using namespace receiver_ui;
using State = studiorider::ReceiverStatus::State;

namespace
{
    juce::String T (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    const juce::String githubPage = "https://github.com/Creme74/StudioRider-Receiver";

    constexpr int kWidth = 460, kHeight = 600, kPad = 22;
    // Where the front tyre touches the ground in logo.png, as a fraction of its height.
    constexpr float logoTyreBottom = 0.9815f;
    constexpr float logoHeight = 142.0f;
    constexpr float logoBelowLine = 9.0f;     // the front wheel rolls over the line, like on the page

    // Version as shown to people: "1.0" rather than "1.0.0".
    juce::String shownVersion()
    {
        const juce::String v (JucePlugin_VersionString);
        return v.endsWith (".0") && v.retainCharacters (".").length() == 2 ? v.dropLastCharacters (2) : v;
    }

    int compareVersions (const juce::String& a, const juce::String& b)
    {
        auto pa = juce::StringArray::fromTokens (a, ".", {}), pb = juce::StringArray::fromTokens (b, ".", {});
        for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
        {
            const int x = pa[i].getIntValue(), y = pb[i].getIntValue();
            if (x != y) return x < y ? -1 : 1;
        }
        return 0;
    }

    juce::String khz (double sr)
    {
        const bool whole = juce::roundToInt (sr) % 1000 == 0;
        return juce::String (sr / 1000.0, whole ? 0 : 1) + " kHz";
    }

    juce::String qualityLabel (const std::string& id)
    {
        if (id == "hires") return T ("Hi-Res · FLAC 24-bit");
        if (id == "cd")    return T ("CD · FLAC 16-bit");
        if (id == "eco")   return T ("Data saver · Opus 256 kbps");
        return {};
    }

    int qualityRankOfItem (int itemIndex) // param index -> eco 0 < cd 1 < hires 2
    {
        return itemIndex == 1 ? 2 : itemIndex == 2 ? 1 : 0;
    }

    int qualityRankOfId (const std::string& id)
    {
        return id == "hires" ? 2 : id == "cd" ? 1 : id == "eco" ? 0 : 2;
    }

    void styleLabel (juce::Label& l, float size, juce::Colour c, bool bold = false)
    {
        l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
        l.setColour (juce::Label::textColourId, c);
        l.setBorderSize ({});
        l.setMinimumHorizontalScale (1.0f);
        l.setInterceptsMouseClicks (false, false);
    }
}

//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (juce::ComboBox::backgroundColourId, field);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, line);
    setColour (juce::ComboBox::arrowColourId, text);
    setColour (juce::PopupMenu::backgroundColourId, field);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, line.brighter (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::TextEditor::backgroundColourId, field);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, line);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    setColour (juce::CaretComponent::caretColourId, text);
    setColour (juce::TextButton::buttonColourId, field);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, text);
    setColour (juce::AlertWindow::backgroundColourId, card);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, line);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    g.setColour (field);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (box.hasKeyboardFocus (true) ? accent : line);
    g.drawRoundedRectangle (r, 8.0f, 1.0f);

    // chevron
    const float cx = (float) width - 16.0f, cy = (float) height * 0.5f;
    juce::Path p;
    p.startNewSubPath (cx - 4.5f, cy - 2.0f);
    p.lineTo (cx, cy + 2.5f);
    p.lineTo (cx + 4.5f, cy - 2.0f);
    g.setColour (box.isEnabled() ? text : muted);
    g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 1, box.getWidth() - 34, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox&)  { return juce::FontOptions (14.0f); }
juce::Font LookAndFeel::getPopupMenuFont()                 { return juce::FontOptions (14.0f); }

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (field.brighter (down ? 0.12f : highlighted ? 0.06f : 0.0f));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r, 8.0f, 1.0f);
}

void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (field);
    g.fillRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f), 8.0f);
}

void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& ed)
{
    g.setColour (ed.hasKeyboardFocus (true) ? accent : line);
    g.drawRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f), 8.0f, 1.0f);
}

//==============================================================================
void PlayButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    juce::Colour fg;
    if (! connected)
    {
        g.setColour (accent.brighter (down ? 0.12f : highlighted ? 0.06f : 0.0f));
        g.fillRoundedRectangle (r, 12.0f);
        fg = accentInk;
    }
    else
    {
        if (highlighted || down)
        {
            g.setColour (field.withAlpha (down ? 1.0f : 0.6f));
            g.fillRoundedRectangle (r, 12.0f);
        }
        g.setColour (line);
        g.drawRoundedRectangle (r, 12.0f, 1.0f);
        fg = receiver_ui::text;
    }

    const juce::String label = connected ? "Disconnect" : "Connect";
    const juce::Font font (juce::FontOptions (17.0f, juce::Font::bold));
    const float textW = juce::GlyphArrangement::getStringWidth (font, label);
    const float iconW = 13.0f, gap = 12.0f;
    const float x0 = r.getCentreX() - (iconW + gap + textW) * 0.5f, cy = r.getCentreY();

    g.setColour (fg);
    if (connected)
    {
        g.fillRoundedRectangle (x0, cy - 6.0f, 12.0f, 12.0f, 2.0f);   // stop square
    }
    else
    {
        juce::Path tri;                                               // play triangle
        tri.addTriangle (x0, cy - 8.0f, x0, cy + 8.0f, x0 + iconW, cy);
        g.fillPath (tri);
    }
    g.setFont (font);
    g.drawText (label, juce::Rectangle<float> (x0 + iconW + gap, r.getY(), textW + 4.0f, r.getHeight()),
                juce::Justification::centredLeft, false);
}

void MenuButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (3.0f);
    if (highlighted || down)
    {
        g.setColour (field.brighter (down ? 0.15f : 0.05f));
        g.fillRoundedRectangle (r, 5.0f);
    }
    g.setColour (highlighted ? receiver_ui::text : muted);
    const float w = 14.0f, x = r.getCentreX() - w / 2.0f, y = r.getCentreY();
    for (float dy : { -5.0f, 0.0f, 5.0f })
        g.fillRoundedRectangle (x, y + dy - 1.0f, w, 2.0f, 1.0f);
}

void Meters::push (float l, float r)
{
    // Fast attack, ~300 ms release (called about 30 times a second).
    levelL = juce::jmax (l, levelL * 0.82f);
    levelR = juce::jmax (r, levelR * 0.82f);
    repaint();
}

void Meters::paint (juce::Graphics& g)
{
    const float rowH = (float) getHeight() / 2.0f;
    int row = 0;
    for (float level : { levelL, levelR })
    {
        const float y = rowH * (float) row + rowH * 0.5f;
        g.setColour (muted);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (row == 0 ? "L" : "R", juce::Rectangle<float> (0.0f, y - 7.0f, 14.0f, 14.0f), juce::Justification::centredLeft);
        const auto bar = juce::Rectangle<float> (22.0f, y - 3.0f, (float) getWidth() - 22.0f, 6.0f);
        g.setColour (field);
        g.fillRoundedRectangle (bar, 3.0f);
        // Same scale as the page: linear amplitude.
        const float w = bar.getWidth() * juce::jlimit (0.0f, 1.0f, level);
        if (w > 0.5f)
        {
            g.setColour (level >= 0.98f ? errColour : okColour);
            g.fillRoundedRectangle (bar.withWidth (w), 3.0f);
        }
        ++row;
    }
}

//==============================================================================
ReceiverEditor::ReceiverEditor (ReceiverProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lookAndFeel);

    brand.setText ("StudioRider Receiver", juce::dontSendNotification);
    styleLabel (brand, 14.0f, muted, true);
    addAndMakeVisible (brand);
    versionLabel.setText ("v" + shownVersion(), juce::dontSendNotification);
    styleLabel (versionLabel, 14.0f, muted);
    addAndMakeVisible (versionLabel);

    menuButton.onClick = [this] { showMenu(); };
    addAndMakeVisible (menuButton);

    heading.setText ("Listen live", juce::dontSendNotification);
    styleLabel (heading, 24.0f, text, true);
    addAndMakeVisible (heading);

    styleLabel (format, 13.0f, muted);
    format.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (format);

    linkCaption.setText ("Listening link", juce::dontSendNotification);
    styleLabel (linkCaption, 12.0f, muted);
    addAndMakeVisible (linkCaption);

    linkField.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    linkField.setIndents (10, 0);
    linkField.setJustification (juce::Justification::centredLeft);
    linkField.setTextToShowWhenEmpty ("https://….trycloudflare.com/…/", muted);
    linkField.setSelectAllWhenFocused (true);
    linkField.setText (proc.getLink(), false);
    linkField.onTextChange = [this] { proc.setLink (linkField.getText()); };
    linkField.onReturnKey = [this] { if (! proc.getEngine().isConnectWanted()) toggleConnection(); };
    addAndMakeVisible (linkField);

    pasteButton.setButtonText ("Paste");
    pasteButton.onClick = [this]
    {
        const auto clip = juce::SystemClipboard::getTextFromClipboard().trim();
        if (clip.isNotEmpty())
        {
            linkField.setText (clip, true);
            lastError.clear();
            refresh();
        }
    };
    addAndMakeVisible (pasteButton);

    playButton.onClick = [this] { toggleConnection(); };
    addAndMakeVisible (playButton);
    addAndMakeVisible (meters);

    qualityCaption.setText ("Quality", juce::dontSendNotification);
    styleLabel (qualityCaption, 12.0f, muted);
    addAndMakeVisible (qualityCaption);
    bufferCaption.setText ("Buffer", juce::dontSendNotification);
    styleLabel (bufferCaption, 12.0f, muted);
    addAndMakeVisible (bufferCaption);

    qualityBox.addItemList (proc.getQualityParam().choices, 1);
    qualityAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (proc.getQualityParam(), qualityBox);
    addAndMakeVisible (qualityBox);
    bufferBox.addItemList (proc.getBufferParam().choices, 1);
    bufferAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (proc.getBufferParam(), bufferBox);
    addAndMakeVisible (bufferBox);

    const char* names[] = { "Buffer", "Bitrate", "Network dropouts", "Listeners" };
    for (int i = 0; i < 4; ++i)
    {
        stats[i].name.setText (names[i], juce::dontSendNotification);
        styleLabel (stats[i].name, 11.0f, muted);
        styleLabel (stats[i].value, 14.0f, text);
        addAndMakeVisible (stats[i].name);
        addAndMakeVisible (stats[i].value);
    }

    note.setText ("Load this plugin as the instrument of an instrument track. To record what you hear, "
                  "send that track to a bus and record the bus on an audio track.", juce::dontSendNotification);
    styleLabel (note, 12.0f, muted);
    note.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (note);

    logo = juce::ImageCache::getFromMemory (ReceiverBinaryData::logo_png, ReceiverBinaryData::logo_pngSize);

    setSize (kWidth, kHeight);
    refresh();
    startTimerHz (30);
}

ReceiverEditor::~ReceiverEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void ReceiverEditor::paint (juce::Graphics& g)
{
    g.fillAll (card);

    // State pill, top right (outlined in its own colour, like on the page).
    if (pillText.isNotEmpty())
    {
        g.setColour (pillColour);
        g.drawRoundedRectangle (pillArea.toFloat().reduced (0.5f), (float) pillArea.getHeight() * 0.5f, 1.0f);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (pillText, pillArea, juce::Justification::centred, false);
    }

    g.setColour (line);
    g.drawHorizontalLine (separatorY, (float) kPad, (float) (getWidth() - kPad));

    if (logo.isValid())
    {
        // While connecting, the moped idles: small random shakes.
        auto area = logoArea;
        if (revving)
        {
            g.addTransform (juce::AffineTransform::rotation (juce::degreesToRadians (rng.nextFloat() * 1.2f - 0.6f),
                                                             area.getCentreX(), area.getBottom()));
            area = area.translated (rng.nextFloat() * 3.0f - 1.5f, rng.nextFloat() * 2.0f - 1.0f);
        }
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (logo, area, juce::RectanglePlacement::stretchToFit);
    }
}

void ReceiverEditor::resized()
{
    auto r = getLocalBounds().reduced (kPad, 0);
    r.removeFromTop (18);

    // Brand, version, state pill, menu
    auto top = r.removeFromTop (26);
    menuButton.setBounds (top.removeFromRight (26).translated (6, 0));
    top.removeFromRight (6);
    const int pillW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (juce::FontOptions (12.0f), pillText)) + 22;
    pillArea = top.removeFromRight (pillW).withSizeKeepingCentre (pillW, 22);
    const int brandW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (brand.getFont(), brand.getText())) + 2;
    brand.setBounds (top.removeFromLeft (brandW));
    top.removeFromLeft (6);
    versionLabel.setBounds (top);

    r.removeFromTop (18);
    heading.setBounds (r.removeFromTop (32));
    r.removeFromTop (2);
    format.setBounds (r.removeFromTop (38));
    r.removeFromTop (10);

    linkCaption.setBounds (r.removeFromTop (18));
    r.removeFromTop (6);
    auto linkRow = r.removeFromTop (34);
    pasteButton.setBounds (linkRow.removeFromRight (64));
    linkRow.removeFromRight (8);
    linkField.setBounds (linkRow);
    r.removeFromTop (14);

    playButton.setBounds (r.removeFromTop (56));
    r.removeFromTop (12);
    meters.setBounds (r.removeFromTop (34));
    r.removeFromTop (12);

    auto captions = r.removeFromTop (18);
    auto boxes = r.removeFromTop (32);
    const int bufferW = 150, gap = 14;
    bufferCaption.setBounds (captions.removeFromRight (bufferW));
    captions.removeFromRight (gap);
    qualityCaption.setBounds (captions);
    bufferBox.setBounds (boxes.removeFromRight (bufferW));
    boxes.removeFromRight (gap);
    qualityBox.setBounds (boxes);

    // Bottom: note under the line, the logo's front wheel on the line.
    auto bottom = getLocalBounds().reduced (kPad, 0).withTrimmedBottom (18);
    note.setBounds (bottom.removeFromBottom (36));
    bottom.removeFromBottom (12);
    separatorY = bottom.getBottom();

    const float logoW = logo.isValid() ? logoHeight * (float) logo.getWidth() / (float) logo.getHeight() : 0.0f;
    logoArea = { (float) bottom.getRight() - logoW,
                 (float) separatorY + logoBelowLine - logoHeight * logoTyreBottom,
                 logoW, logoHeight };

    // Stats: 2 x 2, just above the line, left of the logo.
    bottom.removeFromBottom (14);
    auto statsArea = bottom.removeFromBottom (88).withTrimmedRight ((int) logoW + 10);
    const int colW = statsArea.getWidth() / 2;
    for (int i = 0; i < 4; ++i)
    {
        auto cell = juce::Rectangle<int> (statsArea.getX() + (i % 2) * colW, statsArea.getY() + (i / 2) * 46, colW - 8, 42);
        stats[i].name.setBounds (cell.removeFromTop (17));
        stats[i].value.setBounds (cell.removeFromTop (21));
    }
}

//==============================================================================
void ReceiverEditor::timerCallback()
{
    float l = 0, rr = 0;
    proc.getEngine().takePeaks (l, rr);
    meters.push (l, rr);

    const auto s = proc.getEngine().getStatus().state;
    const bool shouldRev = s == State::connecting || s == State::reconnecting || s == State::buffering;
    if (shouldRev || revving)
        repaint (logoArea.expanded (6.0f).getSmallestIntegerContainer());
    revving = shouldRev;

    if (++tick % 6 == 0)   // the texts: 5 times a second
        refresh();
}

void ReceiverEditor::toggleConnection()
{
    lastError.clear();
    if (proc.getEngine().isConnectWanted())
    {
        proc.disconnect();
    }
    else
    {
        juce::String error;
        if (! proc.connect (linkField.getText(), error))
            lastError = error;
    }
    refresh();
}

void ReceiverEditor::refresh()
{
    const auto st = proc.getEngine().getStatus();
    const bool wanted = proc.getEngine().isConnectWanted();
    playButton.setConnected (wanted);

    // State pill
    juce::String pill;
    juce::Colour pc = muted;
    switch (st.state)
    {
        case State::idle:         pill = "Not connected";      pc = muted; break;
        case State::connecting:   pill = T ("Connecting…");    pc = waitColour; break;
        case State::reconnecting: pill = T ("Reconnecting…");  pc = waitColour; break;
        case State::waiting:      pill = "Waiting for audio";  pc = waitColour; break;
        case State::buffering:    pill = T ("Buffering…");     pc = waitColour; break;
        case State::playing:      pill = "Playing";            pc = okColour;   break;
        case State::notProcessed: pill = "Not processed";      pc = waitColour; break;
        case State::ended:        pill = "Ended";              pc = errColour;  break;
        case State::failed:       pill = "Offline";            pc = errColour;  break;
    }
    if (pill != pillText || pc != pillColour)
    {
        pillText = pill;
        pillColour = pc;
        resized();
        repaint();
    }

    // Line under "Listen live": the format, or what is going on.
    juce::String f;
    juce::Colour fc = muted;
    const auto msg = juce::String::fromUTF8 (st.message.c_str());
    if (lastError.isNotEmpty())
    {
        f = lastError;
        fc = errColour;
    }
    else if (st.state == State::ended || st.state == State::failed)
    {
        f = msg;
        fc = errColour;
    }
    else if (st.state == State::connecting || st.state == State::reconnecting)
    {
        f = msg.isNotEmpty() ? msg : (st.state == State::connecting ? T ("Connecting to the studio…")
                                                                     : T ("Connection lost. Trying again…"));
        fc = msg.isNotEmpty() || st.state == State::reconnecting ? waitColour : muted;
    }
    else if (st.state == State::notProcessed)
    {
        f = T ("Your DAW isn’t running this plugin, so no sound can come out.\n"
               "Load it as the instrument of an instrument track.");
        fc = waitColour;
    }
    else if (st.state == State::waiting)
    {
        f = T ("Connected. The studio isn’t playing anything right now.");
    }
    else if (st.state == State::buffering || st.state == State::playing)
    {
        if (st.codec == 3)
            f = T ("Opus 256 kbps · 48 kHz · stereo · compressed");
        else
            f = "FLAC " + juce::String (st.bits) + T ("-bit · ") + khz (st.streamRate) + T (" · stereo · lossless")
                + (st.bits == 16 ? ", CD quality" : "");
        if (st.dawRate <= 0)
            f += "\nWaiting for your DAW to start its audio engine.";
        else if (std::abs ((double) st.streamRate - st.dawRate) > 0.5)
            f += "\nConverted to " + khz (st.dawRate) + " for your DAW";
        else
            f += "\nPlayed at " + khz (st.dawRate) + ", like your DAW";
    }
    else
    {
        f = proc.getLink().isEmpty() ? T ("Paste the link the studio sent you.")
                                     : T ("Click Connect to listen.");
    }
    format.setText (f, juce::dontSendNotification);
    format.setColour (juce::Label::textColourId, fc);

    // Quality menu: name the studio's choice, and grey out what it doesn't send.
    if (st.studioQuality != shownStudioQuality.toStdString())
    {
        shownStudioQuality = juce::String (st.studioQuality);
        const auto label = qualityLabel (st.studioQuality);
        qualityBox.changeItemText (1, label.isNotEmpty() ? label + T (" (studio’s choice)") : T ("Studio’s choice"));
        const int cap = qualityRankOfId (st.studioQuality);
        for (int i = 1; i < 4; ++i)
            qualityBox.setItemEnabled (i + 1, st.studioQuality.empty() || qualityRankOfItem (i) <= cap);
        // The box doesn't redraw the selected item's new text by itself.
        // (JUCE sees no selection once the selected item's text has changed: use the parameter.)
        qualityBox.setText (qualityBox.getItemText (proc.getQualityParam().getIndex()), juce::dontSendNotification);
    }

    // Stats
    const bool live = st.state == State::playing || st.state == State::buffering || st.state == State::waiting
                      || st.state == State::notProcessed;
    stats[0].value.setText (live ? juce::String (st.bufferSeconds, 2) + " s" : T ("—"), juce::dontSendNotification);
    juce::String rate = T ("—");
    if (live && st.kbps > 0.5)
        rate = st.kbps < 1000 ? juce::String (juce::roundToInt (st.kbps)) + " kbps"
                              : juce::String (st.kbps / 1000.0, 2) + " Mbps";
    stats[1].value.setText (rate, juce::dontSendNotification);
    stats[2].value.setText (juce::String ((juce::int64) st.dropouts), juce::dontSendNotification);
    stats[3].value.setText (live && st.maxListeners > 0 ? juce::String (st.listeners) + " / " + juce::String (st.maxListeners)
                                                        : T ("—"), juce::dontSendNotification);
}

//==============================================================================
void ReceiverEditor::showMenu()
{
    juce::PopupMenu m;
    m.setLookAndFeel (&lookAndFeel);
    m.addItem (T ("About StudioRider Receiver…"), [this] { showAbout(); });
    m.addItem (T ("Check for updates…"), [this] { checkForUpdate(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton)
                                               .withParentComponent (this));
}

void ReceiverEditor::showAbout()
{
    const auto message = "Version " + shownVersion() + "\n\n"
        + T ("Plays a StudioRider stream inside your DAW, so you can monitor it or record it on a track. "
             "Lossless FLAC or Opus, as sent by the studio.\n\n"
             "Free software under the GNU General Public License, version 3 or later. "
             "It comes with no warranty.\n"
             "Built with JUCE, libFLAC and libopus.\n\n")
        + githubPage.fromFirstOccurrenceOf ("https://", false, false);

    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::NoIcon)
                       .withTitle ("StudioRider Receiver")
                       .withMessage (message)
                       .withButton ("GitHub page")
                       .withButton ("OK")
                       .withAssociatedComponent (this);
    juce::AlertWindow::showAsync (options, [] (int button)
    {
        if (button == 1)
            juce::URL (githubPage).launchInDefaultBrowser();
    });
}

void ReceiverEditor::checkForUpdate()
{
    const auto api = [] {
        const auto custom = juce::SystemStats::getEnvironmentVariable ("STUDIORIDER_UPDATE_API", {}); // tests
        return custom.isNotEmpty() ? custom
                                   : juce::String ("https://api.github.com/repos/Creme74/StudioRider-Receiver/releases/latest");
    }();

    juce::Component::SafePointer<ReceiverEditor> safe (this);
    juce::Thread::launch ([safe, api]
    {
        int status = 0;
        juce::String tag, page;
        const auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                              .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: StudioRider")
                              .withConnectionTimeoutMs (10000)
                              .withNumRedirectsToFollow (5)
                              .withStatusCode (&status);
        if (auto in = juce::URL (api).createInputStream (opts))
        {
            const auto json = juce::JSON::parse (in->readEntireStreamAsString());
            if (status == 200)
            {
                tag = json["tag_name"].toString().trim();
                page = json["html_url"].toString();
            }
        }
        const bool ok = tag.isNotEmpty();
        juce::MessageManager::callAsync ([safe, ok, tag, page, status]
        {
            if (safe != nullptr)
                safe->showUpdateResult (ok, tag, page, status);
        });
    });
}

void ReceiverEditor::showUpdateResult (bool okResult, const juce::String& tag, const juce::String& page, int status)
{
    const auto current = shownVersion();
    auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::NoIcon)
                                            .withAssociatedComponent (this);
    if (! okResult)
    {
        juce::AlertWindow::showAsync (options.withTitle ("Check for updates")
                                             .withMessage (status == 404 ? juce::String ("No version has been published on GitHub yet.")
                                                         : status > 0 ? "GitHub answered with error " + juce::String (status) + ". Try again later."
                                                                      : juce::String ("Could not reach GitHub. Check your Internet connection."))
                                             .withButton ("OK"), nullptr);
        return;
    }

    const auto latest = tag.trimCharactersAtStart ("vV");
    if (compareVersions (latest, current) > 0)
    {
        const auto url = page.startsWith (githubPage) ? page : githubPage + "/releases/latest";
        juce::AlertWindow::showAsync (options.withTitle ("StudioRider Receiver " + latest + " is available")
                                             .withMessage ("You have version " + current + ". Download the new version from GitHub, "
                                                           "then quit your DAW and install it.")
                                             .withButton ("Download page")
                                             .withButton ("Later"),
                                      [url] (int button) { if (button == 1) juce::URL (url).launchInDefaultBrowser(); });
    }
    else
    {
        juce::AlertWindow::showAsync (options.withTitle ("Check for updates")
                                             .withMessage ("StudioRider Receiver is up to date (version " + current + ").")
                                             .withButton ("OK"), nullptr);
    }
}
