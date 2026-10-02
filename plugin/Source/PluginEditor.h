// Temporary editor (Phase 3, approved mockup): the 4-character display with the preset -/+ buttons, ROM status,
// engine status and the debug strip with CPU use per block. Replaced by the skin in Phase 4.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class PhyzoEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PhyzoEditor(PhyzoProcessor&);
    ~PhyzoEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Presses and releases the synth's own front-panel button (0 = -/No, 1 = +/Yes) while held.
    class PanelButton : public juce::Component {
    public:
        PanelButton(PhyzoProcessor& p, int osButton, juce::String label) : proc(p), id(osButton), text(std::move(label)) {}
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
    private:
        PhyzoProcessor& proc;
        int id;
        juce::String text;
        bool down = false;
    };

    void timerCallback() override;
    void paintRoms(juce::Graphics&, int y);
    void paintDebug(juce::Graphics&, int y);

    PhyzoProcessor& proc;
    PanelButton minus, plus;
    juce::ToggleButton debugToggle{"Debug"};
};
