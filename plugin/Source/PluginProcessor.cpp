#include "PluginProcessor.h"

PhyzoProcessor::PhyzoProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

void PhyzoProcessor::prepareToPlay(double, int) {}

bool PhyzoProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainInputChannelSet().isDisabled() && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void PhyzoProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    midi.clear();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PhyzoProcessor(); }
