#include "config/Config.h"
#include "core/LoopEngine.h"
#include "core/Metronome.h"
#include "tui/Tui.h"
#include "client/LocalEngineClient.h"
#include "client/OscEngineClient.h"
#include "server/OscServer.h"
#include "JackAudioIO.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <cstdio>
#include <memory>

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

/// Bridges JUCE audio I/O to the LoopEngine
class AudioCallback : public juce::AudioIODeviceCallback {
public:
    explicit AudioCallback(retrospect::LoopEngine& engine) : engine_(engine) {}

    void audioDeviceIOCallbackWithContext(
            const float* const* inputChannelData,
            int numInputChannels,
            float* const* outputChannelData,
            int numOutputChannels,
            int numSamples,
            const juce::AudioIODeviceCallbackContext&) override {

        // Pass all input channels to the engine for per-channel ring
        // buffering and live-activity detection.
        float* output = (numOutputChannels > 0 && outputChannelData[0])
            ? outputChannelData[0] : nullptr;

        engine_.processBlock(inputChannelData, numInputChannels, output, numSamples);

        // Clear any remaining output channels
        for (int ch = 1; ch < numOutputChannels; ++ch) {
            if (outputChannelData[ch]) {
                std::memset(outputChannelData[ch], 0,
                            sizeof(float) * static_cast<size_t>(numSamples));
            }
        }
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        fprintf(stderr, "Audio device starting: %s\n",
                device->getName().toRawUTF8());
        fprintf(stderr, "  Sample rate: %.0f Hz\n", device->getCurrentSampleRate());
        fprintf(stderr, "  Buffer size: %d samples\n",
                device->getCurrentBufferSizeSamples());
    }

    void audioDeviceStopped() override {
        fprintf(stderr, "Audio device stopped\n");
    }

private:
    retrospect::LoopEngine& engine_;
};

enum class RunMode {
    Tui,         // audio + OscServer + LocalEngineClient + TUI (default)
    Headless,    // audio + OscServer, no TUI
    TuiOnly      // OscEngineClient + TUI, no audio
};

/// Open a MIDI output device by name (case-insensitive substring match).
/// Returns nullptr if no matching device is found.
static std::unique_ptr<juce::MidiOutput> openMidiOutput(const juce::String& name) {
    auto devices = juce::MidiOutput::getAvailableDevices();
    for (const auto& d : devices) {
        if (d.name.containsIgnoreCase(name)) {
            auto output = juce::MidiOutput::openDevice(d.identifier);
            if (output) {
                fprintf(stderr, "Opened MIDI output: %s\n", d.name.toRawUTF8());
                return output;
            }
        }
    }
    return nullptr;
}

/// Convert quantize string ("free", "beat", "bar") to Quantize enum
static retrospect::Quantize quantizeFromString(const std::string& s) {
    if (s == "free") return retrospect::Quantize::Free;
    if (s == "beat") return retrospect::Quantize::Beat;
    return retrospect::Quantize::Bar;
}

static void printUsage(const retrospect::Config& cfg) {
    fprintf(stdout, "Usage: retrospect [OPTIONS] [PORT]\n");
    fprintf(stdout, "Options:\n");
    fprintf(stdout, "  --jack                Use JACK audio backend\n");
    fprintf(stdout, "  --alsa                Use ALSA audio backend\n");
    fprintf(stdout, "  --headless            Run without TUI (server only)\n");
    fprintf(stdout, "  --connect HOST:PORT   Connect TUI to a remote server\n");
    fprintf(stdout, "  --midi-out NAME       Use specific MIDI output device (substring match)\n");
    fprintf(stdout, "  --list-midi           List available MIDI output devices\n");
    fprintf(stdout, "\nA virtual MIDI output device named 'Retrospect' is created automatically.\n");
    fprintf(stdout, "  --help                Show this help message\n");
    fprintf(stdout, "\nPORT: OSC server port (default: %s, used in TUI and headless modes)\n", cfg.oscPort.c_str());
    fprintf(stdout, "\nConfig file: %s\n", retrospect::Config::configFilePath().c_str());
    fprintf(stdout, "\nExamples:\n");
    fprintf(stdout, "  retrospect                           TUI + server on port %s (default)\n", cfg.oscPort.c_str());
    fprintf(stdout, "  retrospect 9000                      TUI + server on port 9000\n");
    fprintf(stdout, "  retrospect --headless                Headless server on port %s\n", cfg.oscPort.c_str());
    fprintf(stdout, "  retrospect --headless 9000           Headless server on port 9000\n");
    fprintf(stdout, "  retrospect --connect localhost:7770  TUI-only, connect to remote\n");
    fprintf(stdout, "  retrospect --jack                    TUI + server using JACK\n");
    fprintf(stdout, "  retrospect --midi-out \"USB MIDI\"     Enable MIDI sync output\n");
}

/// Configure engine with shared settings from config.
static void applyEngineConfig(retrospect::LoopEngine& engine, const retrospect::Config& cfg) {
    engine.metronome().setBpm(cfg.bpm);
    engine.metronome().setBeatsPerBar(cfg.beatsPerBar);
    engine.midiSync().setBpm(cfg.bpm);
    engine.setMetronomeClickEnabled(cfg.clickEnabled);
    engine.setMetronomeClickVolume(cfg.clickVolume);
    engine.setCrossfadeSamples(cfg.crossfadeSamples);
    engine.setLookbackBars(cfg.lookbackBars);
    engine.setDefaultQuantize(quantizeFromString(cfg.defaultQuantize));
    engine.setDefaultScrambleParams({cfg.scrambleWindowDuration,
                                     cfg.scrambleFadeDuration,
                                     cfg.scrambleAllowRepeat});
}

/// Set up MIDI output (JUCE virtual or named device) and wire to engine.
static std::unique_ptr<juce::MidiOutput> setupMidiOutput(
        retrospect::LoopEngine& engine, const retrospect::Config& cfg) {
    std::unique_ptr<juce::MidiOutput> midiOutput;
    if (!cfg.midiOutputDevice.empty()) {
        midiOutput = openMidiOutput(juce::String(cfg.midiOutputDevice));
        if (!midiOutput) {
            fprintf(stderr, "Warning: MIDI output device '%s' not found\n", cfg.midiOutputDevice.c_str());
            fprintf(stderr, "Available MIDI outputs:\n");
            auto devices = juce::MidiOutput::getAvailableDevices();
            for (const auto& d : devices) {
                fprintf(stderr, "  %s\n", d.name.toRawUTF8());
            }
        }
    }
    if (!midiOutput) {
        midiOutput = juce::MidiOutput::createNewDevice("Retrospect");
        if (midiOutput) {
            fprintf(stderr, "Created virtual MIDI output: Retrospect\n");
        } else {
            fprintf(stderr, "Warning: could not create virtual MIDI output\n");
        }
    }
    if (midiOutput) {
        juce::MidiOutput* rawPtr = midiOutput.get();
        engine.midiSync().setSendCallback([rawPtr](uint8_t statusByte) {
            rawPtr->sendMessageNow(juce::MidiMessage(statusByte));
        });
    }
    return midiOutput;
}

/// Count system capture ports via a temporary JACK client.
static int countJackCapturePorts() {
    jack_client_t* jc = jack_client_open("RetrospectProbe", JackNoStartServer, nullptr);
    if (!jc) return 2;  // sensible fallback
    int count = 0;
    const char** ports = jack_get_ports(jc, "system:capture",
                                         JACK_DEFAULT_AUDIO_TYPE,
                                         JackPortIsOutput);
    if (ports) {
        while (ports[count]) ++count;
        jack_free(ports);
    }
    jack_client_close(jc);
    return count > 0 ? count : 2;
}

// ---------------------------------------------------------------------------
// JACK audio path — bypasses JUCE for audio I/O
// ---------------------------------------------------------------------------

/// Probe JACK sample rate and buffer size via a temporary client.
struct JackProbeInfo { double sampleRate; int bufferSize; };
static JackProbeInfo probeJackParams() {
    JackProbeInfo info{48000.0, 1024};
    jack_client_t* jc = jack_client_open("RetrospectProbe", JackNoStartServer, nullptr);
    if (jc) {
        info.sampleRate = static_cast<double>(jack_get_sample_rate(jc));
        info.bufferSize = static_cast<int>(jack_get_buffer_size(jc));
        jack_client_close(jc);
    }
    return info;
}

static int runWithJack(const retrospect::Config& cfg, RunMode mode) {
    // JUCE still needed for MIDI
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Determine input channel count
    int numInputChannels = cfg.inputChannels;
    if (numInputChannels <= 0) {
        numInputChannels = countJackCapturePorts();
    }

    // Probe JACK parameters before creating the engine
    auto jackParams = probeJackParams();
    double sampleRate = jackParams.sampleRate;
    int bufferSize = jackParams.bufferSize;

    // Create engine with correct sample rate from the start
    retrospect::LoopEngine engine(cfg.maxLoops, cfg.maxLookbackBars, sampleRate, cfg.minBpm,
                                   numInputChannels, cfg.liveThreshold, cfg.liveWindowMs);
    applyEngineConfig(engine, cfg);

    // Set up MIDI output
    auto midiOutput = setupMidiOutput(engine, cfg);

    // Create and init JackAudioIO
    retrospect::JackAudioIO jackAudioIO(engine, numInputChannels, cfg.jackAutoConnect);
    if (!jackAudioIO.init()) {
        fprintf(stderr, "Failed to initialize JACK audio\n");
        return 1;
    }

    sampleRate = jackAudioIO.sampleRate();
    bufferSize = jackAudioIO.bufferSize();

    fprintf(stderr, "Using JACK audio (direct)\n");
    fprintf(stderr, "  Sample rate: %.0f Hz\n", sampleRate);
    fprintf(stderr, "  Buffer size: %d samples\n", bufferSize);
    fprintf(stderr, "  Input channels: %d\n", numInputChannels);

    // Set up transport
    jackAudioIO.setBpm(cfg.bpm);
    jackAudioIO.setBeatsPerBar(cfg.beatsPerBar);
    jackAudioIO.rewindTransport();
    jackAudioIO.startTransport();

    engine.setBpmChangedCallback([&jackAudioIO](double bpm) {
        jackAudioIO.setBpm(bpm);
    });
    engine.setTransportPositionCallback([&jackAudioIO](int64_t totalSamples) {
        jackAudioIO.updateMetronomePosition(totalSamples);
    });

    if (cfg.midiSyncEnabled) {
        engine.scheduleMidiSync(true, retrospect::Quantize::Free);
    }

    // --- Headless mode ---
    if (mode == RunMode::Headless) {
        retrospect::OscServer oscServer(engine, cfg.oscPort);
        if (!oscServer.start()) {
            jackAudioIO.shutdown();
            return 1;
        }

        fprintf(stderr, "Running headless on port %s\n", cfg.oscPort.c_str());
        fprintf(stderr, "JACK transport: master\n");
        fprintf(stderr, "Press Ctrl+C to stop\n");

        while (g_running && !jackAudioIO.isShutdown()) {
            auto frameStart = std::chrono::steady_clock::now();
            oscServer.pushState();
            auto frameEnd = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                frameEnd - frameStart);
            auto sleepTime = std::chrono::milliseconds(cfg.tuiRefreshMs) - elapsed;
            if (sleepTime.count() > 0) {
                std::this_thread::sleep_for(sleepTime);
            }
        }

        engine.setMidiSyncEnabled(false);
        oscServer.stop();
        jackAudioIO.shutdown();
        return 0;
    }

    // --- TUI mode ---
    retrospect::OscServer oscServer(engine, cfg.oscPort);
    if (!oscServer.start()) {
        jackAudioIO.shutdown();
        return 1;
    }

    retrospect::LocalEngineClient client(engine);
    retrospect::Tui tui(client);

    if (!tui.init()) {
        fprintf(stderr, "Failed to initialize TUI\n");
        oscServer.stop();
        jackAudioIO.shutdown();
        return 1;
    }

    tui.addMessage("Retrospect started - JACK audio (direct)");
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "SR: %.0fHz  Buffer: %d  Inputs: %d",
                 sampleRate, bufferSize, numInputChannels);
        tui.addMessage(buf);
    }
    tui.addMessage("OSC server on port " + cfg.oscPort);
    if (midiOutput) {
        tui.addMessage("MIDI sync output: " + midiOutput->getName().toStdString());
    }
    tui.addMessage("JACK transport: master");
    tui.addMessage("Press 'q' to quit");

    while (g_running && !jackAudioIO.isShutdown()) {
        auto frameStart = std::chrono::steady_clock::now();

        if (!tui.update()) break;

        oscServer.pushState();

        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            frameEnd - frameStart);
        auto sleepTime = std::chrono::milliseconds(cfg.tuiRefreshMs) - elapsed;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
    }

    engine.setMidiSyncEnabled(false);
    oscServer.stop();
    jackAudioIO.shutdown();
    tui.shutdown();

    return 0;
}

// ---------------------------------------------------------------------------
// ALSA audio path — uses JUCE AudioDeviceManager
// ---------------------------------------------------------------------------
static int runWithAlsa(const retrospect::Config& cfg, RunMode mode) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioDeviceManager deviceManager;

    // Set preferred audio backend if specified
    if (!cfg.audioBackend.empty()) {
        juce::String preferredBackend(cfg.audioBackend);
        auto& deviceTypes = deviceManager.getAvailableDeviceTypes();
        bool found = false;
        for (auto* deviceType : deviceTypes) {
            if (deviceType->getTypeName().containsIgnoreCase(preferredBackend)) {
                deviceManager.setCurrentAudioDeviceType(deviceType->getTypeName(), true);
                fprintf(stderr, "Selected audio backend: %s\n", deviceType->getTypeName().toRawUTF8());
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Warning: %s audio backend not found, using default\n",
                    preferredBackend.toRawUTF8());
        }
    }

    // Request up to 64 input channels; the device will provide what it supports
    auto error = deviceManager.initialise(64, 1, nullptr, true);
    if (error.isNotEmpty()) {
        fprintf(stderr, "Audio device error: %s\n", error.toRawUTF8());
        return 1;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    if (!device) {
        fprintf(stderr, "No audio device available\n");
        return 1;
    }

    double sampleRate = device->getCurrentSampleRate();
    int bufferSize = device->getCurrentBufferSizeSamples();
    int numInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
    if (numInputChannels < 1) numInputChannels = 1;

    int outputLatency = device->getOutputLatencyInSamples();
    int inputLatency = device->getInputLatencyInSamples();
    int roundTripLatency = outputLatency + inputLatency;

    fprintf(stderr, "Using audio device: %s\n", device->getName().toRawUTF8());
    fprintf(stderr, "  Sample rate: %.0f Hz\n", sampleRate);
    fprintf(stderr, "  Buffer size: %d samples\n", bufferSize);
    fprintf(stderr, "  Input channels: %d\n", numInputChannels);
    fprintf(stderr, "  Latency: %d in + %d out = %d samples (%.1f ms)\n",
            inputLatency, outputLatency, roundTripLatency,
            1000.0 * roundTripLatency / sampleRate);

    // Create engine with per-channel ring buffers and live detection
    retrospect::LoopEngine engine(cfg.maxLoops, cfg.maxLookbackBars, sampleRate, cfg.minBpm,
                                  numInputChannels, cfg.liveThreshold, cfg.liveWindowMs);
    if (cfg.latencyCompensation) {
        engine.setLatencyCompensation(static_cast<int64_t>(roundTripLatency));
    }

    applyEngineConfig(engine, cfg);

    // Set up MIDI output
    auto midiOutput = setupMidiOutput(engine, cfg);

    // Create and register audio callback
    AudioCallback audioCallback(engine);
    deviceManager.addAudioCallback(&audioCallback);

    if (cfg.midiSyncEnabled) {
        engine.scheduleMidiSync(true, retrospect::Quantize::Free);
    }

    // --- Headless mode (no TUI) ---
    if (mode == RunMode::Headless) {
        retrospect::OscServer oscServer(engine, cfg.oscPort);
        if (!oscServer.start()) {
            deviceManager.removeAudioCallback(&audioCallback);
            return 1;
        }

        fprintf(stderr, "Running headless on port %s\n", cfg.oscPort.c_str());
        if (midiOutput) {
            fprintf(stderr, "MIDI sync output: enabled\n");
        }
        fprintf(stderr, "Press Ctrl+C to stop\n");

        while (g_running) {
            auto frameStart = std::chrono::steady_clock::now();

            oscServer.pushState();

            auto frameEnd = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                frameEnd - frameStart);
            auto sleepTime = std::chrono::milliseconds(cfg.tuiRefreshMs) - elapsed;
            if (sleepTime.count() > 0) {
                std::this_thread::sleep_for(sleepTime);
            }
        }

        engine.setMidiSyncEnabled(false);
        oscServer.stop();
        deviceManager.removeAudioCallback(&audioCallback);
        return 0;
    }

    // --- TUI mode (default): audio + OSC server + TUI ---
    retrospect::OscServer oscServer(engine, cfg.oscPort);
    if (!oscServer.start()) {
        deviceManager.removeAudioCallback(&audioCallback);
        return 1;
    }

    retrospect::LocalEngineClient client(engine);
    retrospect::Tui tui(client);

    if (!tui.init()) {
        fprintf(stderr, "Failed to initialize TUI\n");
        oscServer.stop();
        deviceManager.removeAudioCallback(&audioCallback);
        return 1;
    }

    tui.addMessage("Retrospect started - JUCE audio active");
    tui.addMessage("Device: " + device->getName().toStdString());
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "SR: %.0fHz  Buffer: %d  Latency: %d samples (%.1fms)",
                 sampleRate, bufferSize, roundTripLatency,
                 1000.0 * roundTripLatency / sampleRate);
        tui.addMessage(buf);
    }
    tui.addMessage("OSC server on port " + cfg.oscPort);
    if (midiOutput) {
        tui.addMessage("MIDI sync output: " + midiOutput->getName().toStdString());
    }
    tui.addMessage("Press 'q' to quit");

    // Main loop: TUI at ~30fps
    while (g_running) {
        auto frameStart = std::chrono::steady_clock::now();

        if (!tui.update()) {
            break;
        }

        oscServer.pushState();

        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            frameEnd - frameStart);
        auto sleepTime = std::chrono::milliseconds(cfg.tuiRefreshMs) - elapsed;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
    }

    // Cleanup
    engine.setMidiSyncEnabled(false);
    oscServer.stop();
    deviceManager.removeAudioCallback(&audioCallback);
    tui.shutdown();

    return 0;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Load config file, then apply CLI overrides
    auto cfg = retrospect::Config::load();
    int exitCode = 0;
    if (!cfg.parseArgs(argc, argv, exitCode)) {
        if (cfg.showHelp) {
            printUsage(cfg);
        }
        return exitCode;
    }

    // Determine run mode
    RunMode mode = RunMode::Tui;
    if (cfg.headless) {
        mode = RunMode::Headless;
    } else if (!cfg.connectTarget.empty()) {
        mode = RunMode::TuiOnly;
    }

    // Handle --list-midi (needs JUCE init for device enumeration)
    if (cfg.listMidi) {
        juce::ScopedJuceInitialiser_GUI juceInit;
        auto devices = juce::MidiOutput::getAvailableDevices();
        if (devices.isEmpty()) {
            fprintf(stdout, "No MIDI output devices found.\n");
        } else {
            fprintf(stdout, "Available MIDI output devices:\n");
            for (const auto& d : devices) {
                fprintf(stdout, "  %s\n", d.name.toRawUTF8());
            }
        }
        return 0;
    }

    // --- TUI-only mode (no audio, no engine) ---
    if (mode == RunMode::TuiOnly) {
        auto colonPos = cfg.connectTarget.rfind(':');
        if (colonPos == std::string::npos) {
            fprintf(stderr, "Invalid connect target: %s (expected host:port)\n", cfg.connectTarget.c_str());
            return 1;
        }
        std::string host = cfg.connectTarget.substr(0, colonPos);
        std::string port = cfg.connectTarget.substr(colonPos + 1);

        retrospect::OscEngineClient client(host, port);
        if (!client.isValid()) {
            fprintf(stderr, "Failed to create OSC client\n");
            return 1;
        }

        retrospect::Tui tui(client);
        if (!tui.init()) {
            fprintf(stderr, "Failed to initialize TUI\n");
            return 1;
        }

        tui.addMessage("Connected to " + cfg.connectTarget);
        tui.addMessage("Press 'q' to quit");

        while (g_running) {
            auto frameStart = std::chrono::steady_clock::now();

            if (!tui.update()) break;

            auto frameEnd = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                frameEnd - frameStart);
            auto sleepTime = std::chrono::milliseconds(cfg.tuiRefreshMs) - elapsed;
            if (sleepTime.count() > 0) {
                std::this_thread::sleep_for(sleepTime);
            }
        }

        tui.shutdown();
        return 0;
    }

    // --- Modes that require audio ---
    bool useDirectJack = (cfg.audioBackend == "jack");

    if (useDirectJack) {
        return runWithJack(cfg, mode);
    } else {
        return runWithAlsa(cfg, mode);
    }
}
