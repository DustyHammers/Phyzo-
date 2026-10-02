// Addresses for the supported OS version, from our own static analysis
// (HARDWARE_MAP.md / BOOT_CHECKLIST.md in the private data repo). These are
// locations, not OS content: every table is read from the user's image at run time.
#pragma once
#include <cstdint>

namespace profile {

constexpr uint32_t kMainEntry     = 0x4080;   // main(), a C function
constexpr uint32_t kInitialSsp    = 0x0BE30000;
constexpr uint32_t kSegmentFont   = 0x4A00;   // 128-entry ASCII -> segment table
constexpr uint32_t kButtonMap     = 0x6596;   // raw panel id -> OS button id
constexpr uint32_t kButtonMapLen  = 0x22;     // raw ids 0x00-0x21 accepted
constexpr uint32_t kTickCounter   = 0x0BE072F4;   // 1 ms tick (long)
constexpr uint32_t kHeldFlag1526  = 0x0BE01526;   // gates odd-status button events
constexpr uint32_t kTestFlag0800  = 0x0BE00800;
constexpr uint32_t kDmaDoneFlag   = 0x0BE07237;
constexpr uint32_t kDmaBlocksLeft = 0x0BE072AC;
constexpr uint32_t kKnobPending   = 0x0BE03680;   // F4 request: one bit per analog control
// Filter tables (located by routines 0x1F5C0 and 0x1B7EC; FILTER_REPORT.md). Read at run time by test tools.
constexpr uint32_t kResCodeB      = 0x26B58;      // 51 words: damping code B (register 0x48 bits 0-8)
constexpr uint32_t kResCodeA      = 0x26BC0;      // 51 words: input-gain code A (register 0x48 bits 9-17)
constexpr uint32_t kCutoffLP      = 0x22760;      // 1024 words: K >> 4 for low-pass sections
constexpr int      kResEntries    = 51;

struct Checkpoint { uint32_t pc; const char* label; };

// Points in main() (BOOT_CHECKLIST.md section B). Hitting one records its time.
constexpr Checkpoint kCheckpoints[] = {
    {0x4088, "B1 copy .data"},
    {0x40AA, "B3 vector relocation"},
    {0x4118, "B4 serial init call"},
    {0x4126, "B4 serial init returned OK"},
    {0x4132, "B6 timer 1 init"},
    {0x4138, "B7 interrupts on"},
    {0x413C, "B8 panel hello"},
    {0x4140, "B9 control table / F4 request"},
    {0x4152, "B10 ESP2 init"},
    {0x4160, "B11 software init"},
    {0x4192, "B13 voice/sound init"},
    {0x4198, "B14 wave limit / misc"},
    {0x41E0, "B15 ESP2 program load (0xE198)"},
    {0x41F2, "B15 returned"},
    {0x4208, "B16 scheduler init (0x75DF4)"},
    {0x4210, "B16 returned"},
    {0x422A, "B17 load current preset (0x8BAC4)"},
    {0x4232, "B17 returned"},
    {0x4252, "B19 main loop"},
};

// Hard stall loops in main(): `bra *`.
struct ErrorLoop { uint32_t pc; const char* meaning; };
constexpr ErrorLoop kErrorLoops[] = {
    {0x4124, "serial init failed (crystal flag), gate B4"},
    {0x4206, "ESP2 load/handshake failed, display \" ESP\", gate B15"},
    {0x4224, "scheduler init failed, display \" SCH\", gate B16"},
};

}  // namespace profile
