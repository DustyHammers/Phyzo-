// A file skin (RmlUi document from ~/Documents/Phyzo/skins/<name>/) drawn with OpenGL.
//
// Everything RmlUi and Lua do for this window runs on the window's OpenGL thread: the skin view is created there
// when the GL context starts, input is queued to it, and it is destroyed there when the context closes. Results
// (skin loaded or failed) come back to the message thread through onLoaded.
#pragma once
#include <juce_opengl/juce_opengl.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "PluginProcessor.h"

class RmlGLRenderer;
namespace skin { class SkinView; }

class RmlSkinComponent : public juce::Component, private juce::OpenGLRenderer, private juce::Timer {
public:
    explicit RmlSkinComponent(PhyzoProcessor& p);
    ~RmlSkinComponent() override;

    // Loads <folder>/<name>.rml (asynchronously). onLoaded(ok, error) follows on the message thread.
    void loadSkin(const juce::File& folder, const juce::String& name);
    void reload();
    void setZoom(float zoom) { zoom_ = zoom; }
    void setDebugger(bool on);
    bool debuggerOn() const { return debugger_; }
    // A short message across the top of the skin (e.g. why another skin is shown); empty hides it.
    void showMessage(const juce::String& text, int seconds = 10);
    // A message that stays until cleared (missing ROMs); shown above the other one.
    void setPersistentMessage(const juce::String& text);

    std::function<void(bool ok, const juce::String& error)> onLoaded;
    std::function<void(const juce::MouseEvent&)> onRightClick;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void timerCallback() override;
    void post(std::function<void()> f);
    void startLoad();
    int toPx(float v) const;
    static int mods(const juce::ModifierKeys& m);
    static void writeLog(void* self, const std::string& line);

    PhyzoProcessor& proc_;
    juce::File logFile_;
    juce::OpenGLContext gl_;
    std::unique_ptr<RmlGLRenderer> renderer_;
    std::unique_ptr<skin::SkinView> view_;
    std::mutex queueMtx_;
    std::vector<std::function<void()>> queue_;
    std::atomic<int> width_{1}, height_{1};
    std::atomic<float> zoom_{1.0f}, scale_{1.0f};
    std::string folder_, name_;            // the skin to show (GL thread)
    int viewW_ = 0, viewH_ = 0;            // last viewport given to the view (GL thread)
    float viewDp_ = 0;
    bool debugger_ = false;
    juce::String message_, persistent_;
    juce::uint32 messageUntil_ = 0;
    juce::String glError_;
};
