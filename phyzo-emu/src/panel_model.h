// Model of the front-panel controller on serial channel A.
// OS -> panel: F0 33 hello, F2, F3, F4 (report analog controls), 9x display/LED messages.
// Panel -> OS: F0 33 reply, 80/81 id button edges, 9x keys, Bx knobs.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct PanelEvent {
    uint64_t cycle;
    bool fromOs;                 // true: OS -> panel
    std::vector<uint8_t> bytes;
    std::string decoded;
};

struct DisplayChange {
    uint64_t cycle;
    std::string text;            // 4 characters, best-guess decode
    std::array<uint8_t, 4> raw;  // segment bytes, leftmost first
    uint8_t dots;
};

class PanelModel {
public:
    // fontTable: 128 bytes (ASCII -> segments) read from the user's OS image.
    void setFont(const uint8_t* fontTable);
    std::function<void(const std::vector<uint8_t>&, uint64_t)> sendToOs;
    std::function<double(uint64_t)> cyclesToMs;
    uint64_t cyclesPerMs = 16000;

    // configuration
    bool replyHello = true;
    double helloDelayMs = 2.0;
    int replyF2 = -1;            // >= 0: answer F2 with "F2 xx"
    int answerF4Value = -1;      // >= 0: answer F4 with all 26 controls at this 10-bit value

    void onOsByte(uint8_t b, uint64_t cycle);
    void onResetPin(bool asserted, uint64_t cycle);
    void inject(const std::vector<uint8_t>& bytes, uint64_t cycle, const std::string& note);

    std::string text() const { return text_; }
    std::vector<PanelEvent> transcript;
    std::vector<DisplayChange> displayHistory;
    std::map<uint8_t, int> ledState;     // LED code -> 0 off, 1 on, 2 flashing
    uint64_t helloCount = 0, f2Count = 0, f3Count = 0, f4Count = 0, resetPulses = 0;
    uint64_t unknownMessages = 0;

private:
    void complete();
    std::string decodeSeg(uint8_t s) const;
    int expectedData(uint8_t status) const;

    std::map<uint8_t, char> segToChar_;
    std::array<uint8_t, 4> raw_{};
    uint8_t dots_ = 0;
    std::string text_ = "    ";
    uint8_t status_ = 0;
    std::vector<uint8_t> msg_;
    uint64_t msgStart_ = 0;
    bool resetAsserted_ = false;
};
