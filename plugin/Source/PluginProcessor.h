// Phyzo audio processor: the emulated machine (Engine) as a VST3/AU instrument.
// Data folder: ~/Documents/Phyzo/ with roms/ (the two ROM files, found by checksum) and skins/.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "engine.h"
#include "rom_id.h"

class PhyzoProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    PhyzoProcessor();
    ~PhyzoProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    using AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // ---- for the editor (message thread)
    Engine& engine() { return engine_; }
    const romid::ScanResult& romScan() const { return scan_; }
    juce::File romFolder() const { return dataFolder_.getChildFile("roms"); }
    double hostRate() const { return hostRate_.load(); }
    bool showDebug = true;

    struct Meter {                                   // CPU per block, written by the audio thread
        std::atomic<float> avg{0}, peak{0}, last{0}, lastMs{0}, blockMs{0};
        std::atomic<int> blockSize{0};
        std::atomic<uint64_t> overruns{0};
        std::array<std::atomic<float>, 8> history{}; // one value per second, oldest first after `head`
        std::atomic<uint32_t> head{0};
    } meter;

private:
    void timerCallback() override;
    void scanRoms();

    Engine engine_;
    juce::File dataFolder_;
    romid::ScanResult scan_;
    int timerTicks_ = 0;
    std::atomic<double> hostRate_{0};
    std::vector<Engine::MidiEvent> events_;
    // meter accumulation (audio thread only)
    double winTime_ = 0, winBudget_ = 0, winPeak_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhyzoProcessor)
};
