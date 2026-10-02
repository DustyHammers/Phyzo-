// The emulated machine as a real-time audio engine (no plugin framework here; plugin/ wraps it).
//
// Threads: the host's audio thread calls prepare()/process(); the message thread calls setRoms(), getState(),
// setState(), pressButton() and reads the status. Booting and restoring a saved state run on a worker thread on a
// machine of their own, which then replaces the running one.
//
// Timing: host sample H of the current run corresponds to machine sample anchor + H * 44100 / hostRate. Machine
// output is converted to the host rate without delay (Resampler); MIDI events are delivered to the emulated MIDI
// port at their exact host-sample time plus a constant latency(), which the plugin reports to the host.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "resampler.h"

class Machine;

class Engine {
public:
    enum class State { NoRoms, Booting, Running, Stopped };
    struct MidiEvent { int offset; const uint8_t* data; int size; };   // offset in host samples within the block

    Engine();
    ~Engine();

    // ---- message thread
    void setRoms(const std::string& osPath, const std::string& wavePath, const std::string& osMd5, const std::string& waveMd5);
    std::vector<uint8_t> getState();                 // engine state blob (empty if there is nothing to save)
    void setState(const std::vector<uint8_t>& blob); // restores now, or as soon as the ROMs are known
    // Front-panel button press/release by OS button id (0 = -/No, 1 = +/Yes); lock-free, message thread.
    void pressButton(int osButton, bool down);
    // An analog panel control moved (docs/PANEL_CONTROLS.md): cc 0-25, raw 0-1023. Lock-free, message thread.
    // The running machine receives one Bx cc vv; the position is kept (saved with the machine state) and is what
    // the panel answers to the OS's F4 request when a machine boots.
    void setControl(int cc, int raw);
    int controlPosition(int cc) const { return cc >= 0 && cc < 26 ? positions_[size_t(cc)].load() : 0; }

    // Call regularly on the message thread: restarts the machine after the OS asked for a reboot.
    void service();

    State state() const { return state_.load(); }
    std::string message() const;                     // plain-language status (stop reason, restore notes)
    std::array<char, 4> display() const;             // the 4-character display (best-guess characters)
    uint8_t displayDots() const { return uint8_t(dots_.load()); }

    // ---- audio thread
    void prepare(double hostRate, int maxBlock);
    void process(float* left, float* right, int n, const MidiEvent* events, int numEvents);
    int latencySamples() const { return latency_.load(); }   // host samples, valid after prepare()

    // ---- tests
    std::vector<uint8_t> machineStateForTest();
    uint32_t peekForTest(uint32_t addr, int size);
    std::vector<uint64_t> midiQueueForTest();        // scheduled machine cycles of the MIDI events so far (last 64)
    uint64_t lateMidiForTest() const { return lateMidi_; }   // events that could not be scheduled on time
    uint64_t anchorForTest();
    double cpuHzForTest();

    static constexpr double kBootLimitMs = 8000, kSettleMs = 500;
    static constexpr const char* kBootDisplay = "P 01";

private:
    void startJob(std::vector<uint8_t> state);
    void runJob(std::string osPath, std::string wavePath, std::vector<uint8_t> state, uint64_t generation);
    void stopJob();
    void install(std::unique_ptr<Machine> m, uint64_t generation, uint8_t rawPlus, uint8_t rawMinus);
    void publishDisplay(Machine& m);
    void setMessage(const std::string& s);
    void fail(const std::string& reason);
    void drainButtons(Machine& m);
    void publishPositions(const Machine& m);
    void push(uint32_t entry);

    // ROMs (message thread)
    std::string osPath_, wavePath_, osMd5_, waveMd5_;
    std::vector<uint8_t> pendingState_;              // a saved state waiting for the ROMs

    // worker
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> generation_{0};

    // running machine (audio thread), guarded by mtx_
    std::mutex mtx_;
    std::unique_ptr<Machine> live_;
    uint8_t rawPlus_ = 0, rawMinus_ = 0;             // panel ids of the +/Yes and -/No buttons (from the OS image)
    Resampler resampler_;
    double hostRate_ = 0;
    bool retime_ = true;                             // re-anchor host and machine time on the next block
    uint64_t anchorSample_ = 0;                      // machine sample at host sample 0 of this run
    int64_t hostPos_ = 0;
    int marginIn_ = 48;                              // input frames of slack for MIDI scheduling
    std::vector<float> inL_, inR_;
    std::array<uint64_t, 64> midiTimes_{};           // test record of scheduled MIDI times
    uint64_t midiCount_ = 0, lateMidi_ = 0;

    // status
    std::atomic<State> state_{State::NoRoms};
    std::atomic<bool> rebootRequested_{false};
    std::atomic<int> latency_{0};
    std::atomic<uint32_t> chars_{0x20202020};
    std::atomic<uint32_t> dots_{0};
    mutable std::mutex msgMtx_;
    std::string message_;

    // panel buttons: single-producer ring (UI) -> audio thread
    static constexpr int kButtonRing = 64;
    std::array<std::atomic<uint32_t>, kButtonRing> buttons_{};   // (kind << 24) | payload
    std::array<std::atomic<uint16_t>, 26> positions_{};          // current control positions (for the UI)
    std::mutex controlsMtx_;
    std::array<uint16_t, 26> controls_{};                          // positions for the next cold boot
    std::atomic<uint32_t> btnHead_{0}, btnTail_{0};
};
