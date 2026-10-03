// Global skin settings (all instances and projects; not saved in projects and not synth parameters):
// the chosen skin and the zoom. Stored in ~/Library/Application Support/Phyzo/Phyzo.settings.
#pragma once
#include <juce_data_structures/juce_data_structures.h>

namespace SkinSettings {

inline const juce::String kBuiltIn = "Built-in";
inline const juce::String kDefaultSkin = "rack";
inline constexpr int kZooms[] = {75, 100, 125, 150, 200};

// Opened for each read or write (rare), so no file object outlives the plugin.
inline juce::PropertiesFile::Options options() {
    juce::PropertiesFile::Options o;
    o.applicationName = "Phyzo";
    o.filenameSuffix = "settings";
    o.folderName = "Phyzo";
    o.osxLibrarySubFolder = "Application Support";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    return o;
}
inline juce::String get(const char* key) { return juce::PropertiesFile(options()).getValue(key); }
inline void set(const char* key, const juce::var& v) {
    juce::PropertiesFile f(options());
    f.setValue(key, v);
    f.saveIfNeeded();
}

// Empty when never chosen (then: rack if present, else Built-in).
inline juce::String skin() { return get("skin"); }
inline void setSkin(const juce::String& name) { set("skin", name); }

inline int zoom() {
    const int z = get("zoom").getIntValue();
    for (int v : kZooms) if (v == z) return z;
    return 100;
}
inline void setZoom(int percent) { set("zoom", percent); }

}  // namespace SkinSettings
