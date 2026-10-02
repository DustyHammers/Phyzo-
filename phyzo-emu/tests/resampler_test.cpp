// Resampler test (no ROM data): accuracy against the ideal signal at several host rates, rejection above the
// output Nyquist when converting down, exact pass-through at 44.1 kHz, and identical output for any block sizes.
#include <cmath>
#include <cstdio>
#include <vector>
#include "resampler.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Converts a sine at `hz` from 44.1 kHz to `outRate` in blocks of `block` and returns the output (left channel).
std::vector<float> convert(int outRate, double hz, int nOut, int block, double amp = 0.5) {
    Resampler r; r.setup(44100, outRate);
    std::vector<float> out(static_cast<size_t>(nOut)), l, rr;
    int64_t in = 0;
    for (int k = 0; k < nOut; k += block) {
        const int n = std::min(block, nOut - k);
        const int64_t need = r.inputNeeded(n);
        l.resize(size_t(need)); rr.resize(size_t(need));
        for (int64_t i = 0; i < need; ++i, ++in) l[size_t(i)] = rr[size_t(i)] = float(amp * std::sin(2 * M_PI * hz * double(in) / 44100.0));
        r.push(l.data(), rr.data(), int(need));
        std::vector<float> ol(static_cast<size_t>(n)), orr(static_cast<size_t>(n));
        r.produce(ol.data(), orr.data(), n);
        for (int i = 0; i < n; ++i) out[size_t(k + i)] = ol[size_t(i)];
    }
    return out;
}

// Error against the ideal sine at the output times (no delay), in dB relative to the signal, after the start-up.
double snrDb(int outRate, double hz) {
    const int n = outRate / 2;
    auto y = convert(outRate, hz, n, 256);
    double sig = 0, err = 0;
    for (int k = outRate / 100; k < n; ++k) {
        const double ideal = 0.5 * std::sin(2 * M_PI * hz * double(k) / outRate);
        sig += ideal * ideal; err += (y[size_t(k)] - ideal) * (y[size_t(k)] - ideal);
    }
    return 10 * std::log10(sig / std::max(err, 1e-30));
}
double level(const std::vector<float>& y, int from) {
    double s = 0; for (size_t k = size_t(from); k < y.size(); ++k) s += double(y[k]) * y[k];
    return std::sqrt(s / double(y.size() - size_t(from)));
}
}  // namespace

int main() {
    for (int rate : {48000, 88200, 96000, 176400, 192000})
        for (double hz : {100.0, 1000.0, 10000.0, 18000.0, 19500.0}) {
            const double snr = snrDb(rate, hz);
            if (snr < 75) std::printf("  %d Hz host, %.0f Hz tone: %.1f dB\n", rate, hz, snr);
            CHECK(snr >= 75);
        }
    // Converting down: tones the output can carry are kept, tones above its Nyquist are removed.
    for (int rate : {32000, 22050}) {
        CHECK(snrDb(rate, 1000.0) >= 75);
        const double nyq = rate / 2.0;
        auto y = convert(rate, nyq * 1.15, rate / 2, 512);
        CHECK(20 * std::log10(level(y, rate / 50) / (0.5 / std::sqrt(2.0)) + 1e-30) < -75);
    }
    // 44.1 kHz host: samples pass through unchanged.
    {
        auto y = convert(44100, 1234.5, 5000, 333);
        bool same = true;
        for (int k = 0; k < 5000; ++k) same &= y[size_t(k)] == float(0.5 * std::sin(2 * M_PI * 1234.5 * k / 44100.0));
        CHECK(same);
    }
    // Block sizes do not change the result.
    {
        auto a = convert(48000, 3000, 20000, 64), b = convert(48000, 3000, 20000, 1), c = convert(48000, 3000, 20000, 4096);
        CHECK(a == b && a == c);
    }
    std::printf("resampler_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
