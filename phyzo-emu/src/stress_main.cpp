// phyzo_stress: real-time stress of the plugin engine without ROMs (a small program of our own stands in for the
// OS). An audio thread calls Engine::process every block period, as a host would; a UI thread meanwhile sweeps a
// knob (one move per millisecond), presses buttons and asks for the state (as REAPER does after clicks in a
// plugin window). Reports per block configuration: blocks, worst block time, overruns (blocks over their period).
//
//   build/phyzo_stress [--seconds S] [--no-state]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include "engine.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
const uint16_t kEcho[] = {0x13FC, 0x0005, 0x00F0, 0x071A, 0x41F9, 0x0BE0, 0x0200, 0x0839, 0x0000,
                          0x00F0, 0x0719, 0x67F6, 0x10F9, 0x00F0, 0x071B, 0x60EE};

struct Result { long blocks = 0, overruns = 0; double worstMs = 0, meanMs = 0, budgetMs = 0, p999 = 0, lockUs = 0; long states = 0; };

Result run(const std::string& os, const std::string& wave, double rate, int block, double seconds, bool state) {
    Engine e;
    e.setRoms(os, wave, "os", "wave");
    for (int i = 0; i < 1000 && e.state() != Engine::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    e.prepare(rate, block);
    std::atomic<bool> stop{false};
    std::atomic<long> states{0};
    std::vector<double> times;
    times.reserve(100000);
    std::thread ui([&] {
        int v = 0, dir = 7, t = 0;
        while (!stop) {
            e.setControl(3, v);
            v += dir; if (v >= 1023 || v <= 0) dir = -dir;
            if (t % 50 == 0) e.pressButton(1, (t / 50) % 2 == 0);
            if (state && t % 100 == 0) { auto s = e.getState(); if (!s.empty()) ++states; }
            ++t;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    sched_param sp{};                                    // real-time priority when allowed (as a host's audio thread)
    sp.sched_priority = 80;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    Result r;
    r.budgetMs = 1000.0 * block / rate;
    std::vector<float> l(static_cast<size_t>(block)), rr(static_cast<size_t>(block));
    const auto period = std::chrono::duration<double>(block / rate);
    auto next = Clock::now();
    const auto end = next + std::chrono::duration<double>(seconds);
    double total = 0;
    while (Clock::now() < end) {
        next += std::chrono::duration_cast<Clock::duration>(period);
        const auto t0 = Clock::now();
        e.process(l.data(), rr.data(), block, nullptr, 0);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        total += ms; ++r.blocks; times.push_back(ms);
        r.worstMs = std::max(r.worstMs, ms);
        if (ms > r.budgetMs) ++r.overruns;
        std::this_thread::sleep_until(next);
    }
    stop = true;
    ui.join();
    r.meanMs = total / double(std::max(1L, r.blocks));
    std::sort(times.begin(), times.end());
    if (!times.empty()) r.p999 = times[std::min(times.size() - 1, size_t(double(times.size()) * 0.999))];
    r.lockUs = e.maxLockWaitUs();
    r.states = states;
    return r;
}
}  // namespace

int main(int argc, char** argv) {
    double seconds = 5;
    bool state = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-state")) state = false;
    }
    const fs::path dir = fs::temp_directory_path() / "phyzo_stress";
    fs::create_directories(dir);
    {
        std::vector<uint8_t> os(0x1000, 0);
        for (size_t i = 0; i < sizeof kEcho / 2; ++i) { os[0x80 + 2 * i] = uint8_t(kEcho[i] >> 8); os[0x81 + 2 * i] = uint8_t(kEcho[i]); }
        std::ofstream(dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), long(os.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }
    std::printf("knob sweep 1 move/ms, button every 50 ms%s, %.0f s each\n", state ? ", state request every 100 ms" : "", seconds);
    std::printf("%-8s %-6s %8s %10s %9s %9s %9s %9s %12s %7s\n", "rate", "block", "blocks", "budget ms", "mean ms", "p99.9 ms", "worst ms", "overruns", "lock wait ms", "states");
    for (double rate : {44100.0, 48000.0})
        for (int block : {512, 128}) {
            const Result r = run((dir / "os").string(), (dir / "wave").string(), rate, block, seconds, state);
            std::printf("%-8.0f %-6d %8ld %10.3f %9.3f %9.3f %9.3f %9ld %12.3f %7ld\n", rate, block, r.blocks, r.budgetMs, r.meanMs, r.p999, r.worstMs, r.overruns, r.lockUs / 1000, r.states);
        }
    return 0;
}
