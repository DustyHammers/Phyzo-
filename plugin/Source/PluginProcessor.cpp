#include "PluginProcessor.h"
#include <chrono>
#include "PluginEditor.h"

namespace {
constexpr uint32_t kStateMagic = 0x505A4850;     // "PHZP" (little-endian)
constexpr uint32_t kStateVersion = 3;          // 2: + skin knob positions; 3: + window size (older still read)
constexpr int kTimerMs = 100, kScanEveryTicks = 20;   // rescan the ROM folder every 2 s while something is missing
}

PhyzoProcessor::PhyzoProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    dataFolder_ = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Phyzo");
    dataFolder_.getChildFile("roms").createDirectory();
    dataFolder_.getChildFile("skins").createDirectory();
    events_.reserve(4096);
    scanRoms();
    startTimer(kTimerMs);
}

PhyzoProcessor::~PhyzoProcessor() { stopTimer(); }

void PhyzoProcessor::scanRoms() {
    scan_ = romid::scanFolder(romFolder().getFullPathName().toStdString());
    if (scan_.complete()) engine_.setRoms(scan_.osPath, scan_.wavePath, romid::kOsImageMd5, romid::kWaveImageMd5);
}

void PhyzoProcessor::timerCallback() {
    engine_.service();
    if (!scan_.complete() && ++timerTicks_ >= kScanEveryTicks) {
        timerTicks_ = 0;
        if (!romFolder().isDirectory()) romFolder().createDirectory();
        scanRoms();
    }
}

void PhyzoProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    hostRate_ = sampleRate;
    meter.worstMs = 0; meter.overruns = 0;
    engine_.prepare(sampleRate, samplesPerBlock);
    setLatencySamples(engine_.latencySamples());
}

bool PhyzoProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainInputChannelSet().isDisabled() && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void PhyzoProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = std::chrono::steady_clock::now();
    const int n = buffer.getNumSamples();
    events_.clear();
    for (const auto meta : midi)
        if (events_.size() < events_.capacity()) events_.push_back({meta.samplePosition, meta.data, meta.numBytes});
    midi.clear();
    if (buffer.getNumChannels() >= 2) engine_.process(buffer.getWritePointer(0), buffer.getWritePointer(1), n, events_.data(), int(events_.size()));
    else buffer.clear();

    // CPU per block: processing time against the block's real-time duration.
    const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double budget = n / std::max(1.0, hostRate_.load());
    const double load = budget > 0 ? used / budget : 0;
    meter.last = float(load); meter.lastMs = float(used * 1000); meter.blockMs = float(budget * 1000); meter.blockSize = n;
    if (load > 1.0) ++meter.overruns;
    if (float(used * 1000) > meter.worstMs.load()) meter.worstMs = float(used * 1000);
    winTime_ += used; winBudget_ += budget; winPeak_ = std::max(winPeak_, load);
    if (winBudget_ >= 1.0) {
        const float avg = float(winTime_ / winBudget_);
        meter.avg = avg; meter.peak = float(winPeak_);
        const uint32_t h = meter.head.load();
        meter.history[h % meter.history.size()] = avg;
        meter.head = h + 1;
        winTime_ = winBudget_ = winPeak_ = 0;
    }
}

juce::AudioProcessorEditor* PhyzoProcessor::createEditor() { return new PhyzoEditor(*this); }

// State: magic, version, UI flags, the engine state (complete machine, gzip-compressed), then the skin's knob
// element positions (count, then id and raw for each), then the window size (scale of the skin's base size).
void PhyzoProcessor::getStateInformation(juce::MemoryBlock& dest) {
    ++meter.stateRequests;
    const std::vector<uint8_t> engineState = engine_.getState();
    juce::MemoryOutputStream out(dest, false);
    out.writeInt(int(kStateMagic));
    out.writeInt(int(kStateVersion));
    out.writeInt(showDebug ? 1 : 0);
    juce::MemoryBlock packed;
    {
        juce::MemoryOutputStream raw(packed, false);
        juce::GZIPCompressorOutputStream gz(raw, 6);
        gz.write(engineState.data(), engineState.size());
    }
    out.writeInt64(juce::int64(engineState.size()));
    out.writeInt64(juce::int64(packed.getSize()));
    out.write(packed.getData(), packed.getSize());
    std::lock_guard<std::mutex> lk(port_.mtx);
    out.writeInt(int(port_.knobs.size()));
    for (const auto& kv : port_.knobs) { out.writeString(juce::String::fromUTF8(kv.first.c_str())); out.writeInt(kv.second); }
    out.writeFloat(editorScale.load());
}

void PhyzoProcessor::setStateInformation(const void* data, int size) {
    juce::MemoryInputStream in(data, size_t(size), false);
    if (uint32_t(in.readInt()) != kStateMagic) return;
    const int version = in.readInt();
    if (version < 1 || version > int(kStateVersion)) return;
    showDebug = in.readInt() != 0;
    const juce::int64 rawSize = in.readInt64(), packedSize = in.readInt64();
    if (rawSize <= 0 || rawSize > (juce::int64(256) << 20) || packedSize <= 0 || packedSize > in.getNumBytesRemaining()) return;
    juce::MemoryBlock packed;
    in.readIntoMemoryBlock(packed, packedSize);
    juce::MemoryInputStream packedIn(packed, false);
    juce::GZIPDecompressorInputStream gz(packedIn);
    std::vector<uint8_t> engineState(static_cast<size_t>(rawSize));
    if (gz.read(engineState.data(), int(rawSize)) != int(rawSize)) return;
    std::map<std::string, int> knobs;
    if (version >= 2) {
        const int n = in.readInt();
        for (int i = 0; i < n && i < 4096 && !in.isExhausted(); ++i) {
            const juce::String id = in.readString();
            knobs[id.toStdString()] = juce::jlimit(0, 1023, in.readInt());
        }
    }
    if (version >= 3 && !in.isExhausted()) editorScale = in.readFloat();
    { std::lock_guard<std::mutex> lk(port_.mtx); port_.knobs = std::move(knobs); }
    engine_.setState(engineState);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PhyzoProcessor(); }
