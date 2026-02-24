#pragma once

#include <string>
#include <vector>

namespace retrospect {

struct Config {
    // [audio]
    std::string audioBackend;             // "" = auto, "jack", "alsa"
    std::string outputMode = "stereo";    // "stereo" or "multichannel"
    std::vector<int> mainOutputs;         // 1-based channel indices (empty = {1,2})
    std::vector<int> metronomeOutputs;    // 1-based channel indices (empty = same as main)

    // [engine]
    int maxLoops = 8;
    int maxLookbackBars = 8;
    double minBpm = 60.0;
    std::string defaultQuantize = "bar";  // "free", "beat", "bar"
    int crossfadeSamples = 256;
    int lookbackBars = 1;
    bool latencyCompensation = true;      // Auto-compensate round-trip latency

    // [input]
    float liveThreshold = 0.0f;          // 0 = disabled (all channels pass)
    int liveWindowMs = 500;              // Activity detection window in ms

    // [metronome]
    double bpm = 120.0;
    int beatsPerBar = 4;
    bool clickEnabled = true;
    float clickVolume = 0.5f;

    // [midi]
    bool midiSyncEnabled = false;
    std::string midiOutputDevice;         // "" = virtual device

    // [osc]
    std::string oscPort = "7770";

    // [tui]
    int tuiRefreshMs = 33;

    // CLI-only fields
    bool headless = false;
    std::string connectTarget;
    bool listMidi = false;
    bool showHelp = false;

    /// Load config from the TOML file (if it exists).
    /// Missing file or missing fields silently use defaults.
    static Config load();

    /// Returns the path to the config file.
    static std::string configFilePath();

    /// Parse CLI arguments, mutating this config in-place.
    /// Returns true if the program should continue, false if it should exit.
    /// Sets exitCode to the exit code when returning false.
    bool parseArgs(int argc, char* argv[], int& exitCode);
};

} // namespace retrospect
