// What a skin needs from the synth. The plugin implements it over the Engine; tests use a recording fake.
// No JUCE and no emulator types here: the skin runtime (plugin/skin) builds and tests on its own.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace skin {

enum class LedMode : uint8_t { Off = 0, On = 1, Flash = 2, Beat = 3 };   // panel messages 90, 91, 92, 9D

struct LedReport {
    LedMode mode = LedMode::Off;
    uint32_t beats = 0;              // number of 9D messages so far for this code
    float beatIntervalMs = 0;        // machine time between the last two 9D messages (0: only one so far)
};

class PanelPort {
public:
    virtual ~PanelPort() = default;
    virtual void sendControl(int cc, int raw) = 0;            // one panel message Bx cc vv, absolute raw 0-1023
    virtual int controlPosition(int cc) = 0;                  // the last value sent for the cc (F-02 defaults when fresh)
    virtual void sendButton(int raw, bool down) = 0;          // 81 raw / 80 raw
    virtual LedReport led(int code) = 0;
    virtual std::array<uint8_t, 4> segments() = 0;            // display segment bytes, leftmost digit first
    // Positions of individual knob elements (stacked knobs share a cc), kept in the plugin state.
    virtual bool knobPosition(const std::string& id, int& raw) = 0;
    virtual void setKnobPosition(const std::string& id, int raw) = 0;
    virtual bool audioClip() { return false; }                // Input Clip LED (F-19); unlit until then
};

struct PluginInfo {
    std::string name = "Phyzo", vendor = "DHammers", version;
};

}  // namespace skin
