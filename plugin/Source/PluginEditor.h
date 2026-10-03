// The plugin window: shows the chosen skin (a file skin from ~/Documents/Phyzo/skins/, drawn by RmlUi, or the
// compiled-in "Built-in" skin) at the chosen zoom, and the right-click menu (skin, reload, zoom, developer).
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include "PluginProcessor.h"

class RmlSkinComponent;

// The compiled-in skin (Phase 4 approved mockup): title and version, the display drawn from the OS's segment bytes,
// previous/next preset, Status, and the Debug section with CPU per block. No image files.
class BuiltinView : public juce::Component, private juce::Timer {
public:
    static constexpr int kWidth = 520, kHeight = 284;
    explicit BuiltinView(PhyzoProcessor&);
    ~BuiltinView() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void setNotice(const juce::String& text);        // e.g. why this skin is shown instead of the chosen one
    std::function<void(const juce::MouseEvent&)> onRightClick;

private:
    class PanelButton : public juce::Component {     // presses an OS button (0 = -/No, 1 = +/Yes) while held
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
    void paintDisplay(juce::Graphics&, juce::Rectangle<int> area);
    void paintStatus(juce::Graphics&, int y);
    void paintDebug(juce::Graphics&, int y);

    PhyzoProcessor& proc;
    PanelButton minus, plus;
    juce::ToggleButton debugToggle{"Debug"};
    juce::String notice_;
    juce::uint32 noticeUntil_ = 0;
};

class PhyzoEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PhyzoEditor(PhyzoProcessor&);
    ~PhyzoEditor() override;
    void resized() override;

private:
    juce::StringArray skinNames() const;             // discovered file skins
    juce::File skinFolder(const juce::String& name) const;
    void showSkin(const juce::String& name, const juce::String& notice = {});
    void showBuiltIn(const juce::String& notice);
    void skinFailed(const juce::String& name, const juce::String& reason);
    void applyZoom();
    void showMenu(const juce::MouseEvent&);
    void timerCallback() override;                   // missing-ROM message on file skins

    PhyzoProcessor& proc;
    std::unique_ptr<BuiltinView> builtin_;
    std::unique_ptr<RmlSkinComponent> rml_;
    juce::String current_;                           // the skin being shown
    int zoom_ = 100;
    bool debugger_ = false;
};
