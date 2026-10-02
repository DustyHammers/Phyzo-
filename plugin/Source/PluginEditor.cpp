#include "PluginEditor.h"

namespace {
const juce::Colour kBg(0xff1e1f22), kPanel(0xff2a2c30), kLine(0xff3a3d42), kText(0xffd8d8d8), kDim(0xff8a8f98);
const juce::Colour kLedBg(0xff0a140c), kLed(0xff5dff6a), kLedOff(0xff1d3a22), kOk(0xff5dd16a), kBad(0xffe0605a);
constexpr int kW = 520, kH = 330, kDebugY = 262;

juce::Font uiFont(float size, bool bold = false) { return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)); }
juce::Font monoFont(float size) {
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), size, juce::Font::bold));
}
juce::String rateText(double hz) {
    const auto rounded = juce::roundToInt(hz);
    return juce::String(hz / 1000.0, rounded % 1000 != 0 ? 1 : 0) + " kHz";
}
}  // namespace

// ------------------------------------------------------------------ panel buttons

void PhyzoEditor::PanelButton::paint(juce::Graphics& g) {
    const bool on = isEnabled();
    auto r = getLocalBounds().toFloat().reduced(1);
    g.setColour(down ? kLine : kPanel);
    g.fillRoundedRectangle(r, 6);
    g.setColour(on ? kDim : kLine);
    g.drawRoundedRectangle(r, 6, 1);
    g.setColour(on ? kText : kDim.withAlpha(0.5f));
    g.setFont(uiFont(16, true));
    g.drawText(text, getLocalBounds(), juce::Justification::centred);
}

void PhyzoEditor::PanelButton::mouseDown(const juce::MouseEvent&) {
    if (!isEnabled()) return;
    down = true; repaint();
    proc.engine().pressButton(id, true);
}

void PhyzoEditor::PanelButton::mouseUp(const juce::MouseEvent&) {
    if (!down) return;
    down = false; repaint();
    proc.engine().pressButton(id, false);
}

// ------------------------------------------------------------------ editor

PhyzoEditor::PhyzoEditor(PhyzoProcessor& p)
    : AudioProcessorEditor(&p), proc(p),
      minus(p, 0, juce::String::fromUTF8("\xe2\x97\x80  \xe2\x88\x92")),    // "◀  −"
      plus(p, 1, juce::String::fromUTF8("+  \xe2\x96\xb6")) {               // "+  ▶"
    addAndMakeVisible(minus);
    addAndMakeVisible(plus);
    debugToggle.setToggleState(proc.showDebug, juce::dontSendNotification);
    debugToggle.setColour(juce::ToggleButton::textColourId, kText);
    debugToggle.onClick = [this] { proc.showDebug = debugToggle.getToggleState(); repaint(); };
    addAndMakeVisible(debugToggle);
    setSize(kW, kH);
    timerCallback();
    startTimerHz(15);
}

PhyzoEditor::~PhyzoEditor() { stopTimer(); }

void PhyzoEditor::resized() {
    minus.setBounds(40, 58, 84, 40);
    plus.setBounds(kW - 40 - 84, 58, 84, 40);
    debugToggle.setBounds(14, kDebugY + 6, 90, 22);
}

void PhyzoEditor::timerCallback() {
    const bool running = proc.engine().state() == Engine::State::Running;
    minus.setEnabled(running);
    plus.setEnabled(running);
    repaint();
}

void PhyzoEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    // header
    g.setColour(kText);
    g.setFont(uiFont(16, true));
    g.drawText("PHYZO", 16, 0, 200, 34, juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(uiFont(13));
    g.drawText("v" JucePlugin_VersionString, kW - 216, 0, 200, 34, juce::Justification::centredRight);
    g.setColour(kLine);
    g.drawHorizontalLine(34, 0, float(kW));

    // the synth's 4-character display
    const auto st = proc.engine().state();
    const juce::Rectangle<int> disp((kW - 240) / 2, 50, 240, 56);
    g.setColour(kLedBg);
    g.fillRoundedRectangle(disp.toFloat(), 4);
    g.setColour(kLine);
    g.drawRoundedRectangle(disp.toFloat(), 4, 1);
    std::array<char, 4> chars = proc.engine().display();
    if (st == Engine::State::NoRoms) chars = {'-', '-', '-', '-'};
    g.setFont(monoFont(34));
    const int cellW = 44, x0 = disp.getCentreX() - 2 * cellW;
    for (int i = 0; i < 4; ++i) {
        g.setColour(st == Engine::State::NoRoms ? kLedOff : kLed);
        g.drawText(juce::String::charToString(juce::juce_wchar(uint8_t(chars[size_t(i)]))), x0 + i * cellW, disp.getY(), cellW,
                   disp.getHeight(), juce::Justification::centred);
    }
    const uint8_t dots = proc.engine().displayDots();      // decoded by the panel model: 8 = colon, 4 = point
    g.setColour(kLed);
    if (dots & 8) { g.fillEllipse(float(x0 + 2 * cellW - 3), float(disp.getY() + 20), 5, 5); g.fillEllipse(float(x0 + 2 * cellW - 3), float(disp.getY() + 33), 5, 5); }
    if (dots & 4) g.fillEllipse(float(x0 + 3 * cellW - 3), float(disp.getBottom() - 14), 5, 5);
    g.setColour(kDim);
    g.setFont(uiFont(12));
    g.drawText("previous / next preset", disp.getX(), disp.getBottom() + 4, disp.getWidth(), 16, juce::Justification::centred);

    paintRoms(g, 136);
    if (proc.showDebug) paintDebug(g, kDebugY);
    g.setColour(kLine);
    g.drawHorizontalLine(kDebugY, 0, float(kW));
}

void PhyzoEditor::paintRoms(juce::Graphics& g, int y) {
    const romid::ScanResult& s = proc.romScan();
    const juce::String folder = proc.romFolder().getFullPathName() + "/";
    auto row = [&](int yy, bool ok, const juce::String& what) {
        g.setColour(ok ? kOk : kBad);
        g.fillEllipse(100, float(yy + 5), 8, 8);
        g.setColour(kText);
        g.setFont(uiFont(13));
        g.drawText(what, 116, yy, 130, 18, juce::Justification::centredLeft);
        g.setColour(ok ? kText : kBad);
        g.drawText(ok ? "found" : "MISSING", 250, yy, 120, 18, juce::Justification::centredLeft);
    };
    g.setColour(kDim);
    g.setFont(uiFont(13, true));
    g.drawText("ROMs", 20, y, 70, 18, juce::Justification::centredLeft);
    row(y, s.hasOs(), "OS image");
    row(y + 18, s.hasWave(), "Wave image");
    g.setFont(uiFont(12));
    int yy = y + 40;
    if (s.complete()) {
        g.setColour(kDim);
        g.drawText(folder, 100, yy, kW - 110, 16, juce::Justification::centredLeft);
        yy += 28;
        // engine status
        g.setFont(uiFont(13, true));
        g.drawText("Status", 20, yy, 70, 18, juce::Justification::centredLeft);
        g.setFont(uiFont(13));
        g.setColour(kText);
        juce::String status;
        switch (proc.engine().state()) {
        case Engine::State::NoRoms: status = "waiting for the ROMs"; break;
        case Engine::State::Booting: status = juce::String::fromUTF8("booting\xe2\x80\xa6"); break;
        case Engine::State::Running:
            status = "running " + juce::String::fromUTF8("\xc2\xb7") + " 44.1 kHz " + juce::String::fromUTF8("\xe2\x86\x92") +
                     " host " + rateText(proc.hostRate());
            break;
        case Engine::State::Stopped: status = "stopped"; break;
        }
        g.drawText(status, 100, yy, kW - 110, 18, juce::Justification::centredLeft);
        const juce::String msg(proc.engine().message());
        if (msg.isNotEmpty()) {
            g.setColour(proc.engine().state() == Engine::State::Stopped ? kBad : kDim);
            g.setFont(uiFont(12));
            g.drawFittedText(msg, 100, yy + 18, kW - 110, 32, juce::Justification::topLeft, 2);
        }
        return;
    }
    g.setColour(kText);
    juce::String what;
    if (!s.hasOs() && !s.hasWave()) what = "the OS image and the native wave image (4 MB)";
    else if (!s.hasOs()) what = "the OS image";
    else what = "the native wave image (4 MB)";
    g.drawText("Put " + what + " into:", 20, yy, kW - 30, 16, juce::Justification::centredLeft);
    g.setColour(kLed);
    g.drawText("   " + folder, 20, yy + 16, kW - 30, 16, juce::Justification::centredLeft);
    g.setColour(kText);
    g.drawText("Any file name works; it is recognised by checksum.", 20, yy + 34, kW - 30, 16, juce::Justification::centredLeft);
    juce::String wrong;
    for (const auto& r : s.rejected) if (!r.md5.empty()) wrong << (wrong.isEmpty() ? "" : ", ") << juce::String(r.name);
    int ly = yy + 50;
    if (wrong.isNotEmpty()) {
        g.setColour(kBad);
        g.drawText("Found but wrong checksum: " + wrong, 20, ly, kW - 30, 16, juce::Justification::centredLeft);
        ly += 16;
    }
    g.setColour(kDim);
    const bool both = !s.hasOs() && !s.hasWave();
    g.drawText(juce::String::fromUTF8(both ? "Waiting for the files\xe2\x80\xa6" : "Waiting for the file\xe2\x80\xa6"), 20, ly, kW - 30, 16,
               juce::Justification::centredLeft);
}

void PhyzoEditor::paintDebug(juce::Graphics& g, int y) {
    const auto& m = proc.meter;
    g.setFont(uiFont(12));
    g.setColour(kText);
    const float avg = m.avg.load() * 100, peak = m.peak.load() * 100;
    g.drawText("CPU per block", 20, y + 32, 100, 16, juce::Justification::centredLeft);
    g.drawText("avg " + juce::String(avg, 1) + " %", 130, y + 32, 90, 16, juce::Justification::centredLeft);
    g.setColour(peak > 100 ? kBad : kText);
    g.drawText("peak " + juce::String(peak, 1) + " %", 220, y + 32, 100, 16, juce::Justification::centredLeft);
    // history: one bar per second, newest on the right; full height = 100 %
    const uint32_t head = m.head.load();
    const int n = int(m.history.size()), bx = 330, bw = 10, bh = 16;
    for (int i = 0; i < n; ++i) {
        const float v = juce::jlimit(0.0f, 1.0f, m.history[size_t((head + uint32_t(i)) % uint32_t(n))].load());
        g.setColour(kLine);
        g.fillRect(bx + i * (bw + 2), y + 32, bw, bh);
        g.setColour(v > 0.8f ? kBad : kOk);
        const int h = int(std::round(v * bh));
        g.fillRect(bx + i * (bw + 2), y + 32 + bh - h, bw, h);
    }
    g.setColour(kDim);
    const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
    g.drawText("block " + juce::String(m.blockSize.load()) + " smp" + dot + juce::String(m.blockMs.load(), 1) + " ms" + dot + "last " +
                   juce::String(m.lastMs.load(), 2) + " ms" + dot + "overruns " + juce::String(juce::int64(m.overruns.load())),
               20, y + 50, kW - 30, 16, juce::Justification::centredLeft);
}
