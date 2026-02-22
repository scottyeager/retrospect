#include "config/Config.h"

#include <toml++/toml.hpp>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>

namespace retrospect {

std::string Config::configFilePath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        return std::string(xdg) + "/retrospect/config.toml";
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.config/retrospect/config.toml";
    }
    return {};
}

Config Config::load() {
    Config cfg;

    std::string path = configFilePath();
    if (path.empty() || !std::filesystem::exists(path)) {
        return cfg;
    }

    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error& err) {
        fprintf(stderr, "Warning: failed to parse %s: %s\n",
                path.c_str(), err.what());
        return cfg;
    }

    // [audio]
    if (auto v = tbl["audio"]["backend"].value<std::string>()) {
        if (*v == "jack" || *v == "alsa" || v->empty()) {
            cfg.audioBackend = *v;
        } else {
            fprintf(stderr, "Warning: invalid audio.backend '%s', using auto\n", v->c_str());
        }
    }

    // [engine]
    if (auto v = tbl["engine"]["max_loops"].value<int64_t>()) {
        if (*v >= 1 && *v <= 64) {
            cfg.maxLoops = static_cast<int>(*v);
        } else {
            fprintf(stderr, "Warning: invalid engine.max_loops %lld, using default %d\n",
                    static_cast<long long>(*v), cfg.maxLoops);
        }
    }
    if (auto v = tbl["engine"]["max_lookback_bars"].value<int64_t>()) {
        if (*v >= 1 && *v <= 64) {
            cfg.maxLookbackBars = static_cast<int>(*v);
        } else {
            fprintf(stderr, "Warning: invalid engine.max_lookback_bars %lld, using default %d\n",
                    static_cast<long long>(*v), cfg.maxLookbackBars);
        }
    }
    if (auto v = tbl["engine"]["min_bpm"].value<double>()) {
        if (*v >= 20.0 && *v <= 300.0) {
            cfg.minBpm = *v;
        } else {
            fprintf(stderr, "Warning: invalid engine.min_bpm %.1f, using default %.1f\n",
                    *v, cfg.minBpm);
        }
    }
    if (auto v = tbl["engine"]["default_quantize"].value<std::string>()) {
        if (*v == "free" || *v == "beat" || *v == "bar") {
            cfg.defaultQuantize = *v;
        } else {
            fprintf(stderr, "Warning: invalid engine.default_quantize '%s', using default '%s'\n",
                    v->c_str(), cfg.defaultQuantize.c_str());
        }
    }
    if (auto v = tbl["engine"]["crossfade_samples"].value<int64_t>()) {
        if (*v >= 0 && *v <= 4096) {
            cfg.crossfadeSamples = static_cast<int>(*v);
        } else {
            fprintf(stderr, "Warning: invalid engine.crossfade_samples %lld, using default %d\n",
                    static_cast<long long>(*v), cfg.crossfadeSamples);
        }
    }
    if (auto v = tbl["engine"]["lookback_bars"].value<int64_t>()) {
        if (*v >= 1 && *v <= cfg.maxLookbackBars) {
            cfg.lookbackBars = static_cast<int>(*v);
        } else {
            fprintf(stderr, "Warning: invalid engine.lookback_bars %lld, using default %d\n",
                    static_cast<long long>(*v), cfg.lookbackBars);
        }
    }
    if (auto v = tbl["engine"]["latency_compensation"].value<bool>()) {
        cfg.latencyCompensation = *v;
    }

    // [input]
    if (auto v = tbl["input"]["live_threshold"].value<double>()) {
        if (*v >= 0.0 && *v <= 1.0) {
            cfg.liveThreshold = static_cast<float>(*v);
        } else {
            fprintf(stderr, "Warning: invalid input.live_threshold %.4f, using default %.4f\n",
                    *v, static_cast<double>(cfg.liveThreshold));
        }
    }
    if (auto v = tbl["input"]["live_window_ms"].value<int64_t>()) {
        if (*v >= 10 && *v <= 10000) {
            cfg.liveWindowMs = static_cast<int>(*v);
        } else {
            fprintf(stderr, "Warning: invalid input.live_window_ms %lld, using default %d\n",
                    static_cast<long long>(*v), cfg.liveWindowMs);
        }
    }

    // [input]
    if (auto v = tbl["input"]["live_threshold"].value<double>()) {
        if (*v >= 0.0 && *v <= 1.0) {
            cfg.liveThreshold = static_cast<float>(*v);
        } else {
            fprintf(stderr, "Warning: invalid input.live_threshold %.4f, using default %.4f\n",
                    *v, static_cast<double>(cfg.liveThreshold));
        }
    }
    if (auto v = tbl["input"]["live_window_ms"].value<int64_t>()) {
        if (*v >= 10 && *v <= 10000) {
            cfg.liveWindowMs = static_cast<int>(*v);
        } else {
            fprintf(stderr, "Warning: invalid input.live_window_ms %lld, using default %d\n",
                    static_cast<long long>(*v), cfg.liveWindowMs);
        }
    }

    // [metronome]
    if (auto v = tbl["metronome"]["bpm"].value<double>()) {
        if (*v >= 20.0 && *v <= 300.0) {
            cfg.bpm = *v;
        } else {
            fprintf(stderr, "Warning: invalid metronome.bpm %.1f, using default %.1f\n",
                    *v, cfg.bpm);
        }
    }
    if (auto v = tbl["metronome"]["beats_per_bar"].value<int64_t>()) {
        if (*v >= 1 && *v <= 16) {
            cfg.beatsPerBar = static_cast<int>(*v);
        } else {
            fprintf(stderr, "Warning: invalid metronome.beats_per_bar %lld, using default %d\n",
                    static_cast<long long>(*v), cfg.beatsPerBar);
        }
    }
    if (auto v = tbl["metronome"]["click_enabled"].value<bool>()) {
        cfg.clickEnabled = *v;
    }
    if (auto v = tbl["metronome"]["click_volume"].value<double>()) {
        if (*v >= 0.0 && *v <= 1.0) {
            cfg.clickVolume = static_cast<float>(*v);
        } else {
            fprintf(stderr, "Warning: invalid metronome.click_volume %.2f, using default %.2f\n",
                    *v, static_cast<double>(cfg.clickVolume));
        }
    }

    // [midi]
    if (auto v = tbl["midi"]["sync_enabled"].value<bool>()) {
        cfg.midiSyncEnabled = *v;
    }
    if (auto v = tbl["midi"]["output_device"].value<std::string>()) {
        cfg.midiOutputDevice = *v;
    }

    // [osc]
    // Accept port as either string or integer
    if (auto node = tbl["osc"]["port"]) {
        if (auto v = node.value<std::string>()) {
            cfg.oscPort = *v;
        } else if (auto v = node.value<int64_t>()) {
            cfg.oscPort = std::to_string(*v);
        }
    }

    // [tui]
    if (auto v = tbl["tui"]["refresh_ms"].value<int64_t>()) {
        if (*v >= 10 && *v <= 1000) {
            cfg.tuiRefreshMs = static_cast<int>(*v);
        } else {
            fprintf(stderr, "Warning: invalid tui.refresh_ms %lld, using default %d\n",
                    static_cast<long long>(*v), cfg.tuiRefreshMs);
        }
    }

    return cfg;
}

bool Config::parseArgs(int argc, char* argv[], int& exitCode) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--jack") {
            audioBackend = "jack";
        } else if (arg == "--alsa") {
            audioBackend = "alsa";
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--connect") {
            if (i + 1 < argc) {
                connectTarget = argv[++i];
            } else {
                fprintf(stderr, "--connect requires HOST:PORT argument\n");
                exitCode = 1;
                return false;
            }
        } else if (arg == "--midi-out") {
            if (i + 1 < argc) {
                midiOutputDevice = argv[++i];
            } else {
                fprintf(stderr, "--midi-out requires a device name argument\n");
                exitCode = 1;
                return false;
            }
        } else if (arg == "--list-midi") {
            listMidi = true;
        } else if (arg == "--help" || arg == "-h") {
            showHelp = true;
            exitCode = 0;
            return false;
        } else if (arg[0] != '-') {
            oscPort = arg;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            exitCode = 1;
            return false;
        }
    }
    return true;
}

} // namespace retrospect
