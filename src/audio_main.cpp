#include "core/LoopEngine.h"
#include "tui/Tui.h"
#include "client/LocalEngineClient.h"
#include "client/OscEngineClient.h"
#include "server/OscServer.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <cstdio>
#include <memory>

static constexpr int kMaxLoops = 8;
static constexpr int kMaxLookbackBars = 8;
static constexpr double kMinBpm = 60.0;
static constexpr int kTuiRefreshMs = 33;
static constexpr const char* kDefaultOscPort = "7770";

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

        // Extract mono input (channel 0, or silence)
        const float* input = (numInputChannels > 0 && inputChannelData[0])
            ? inputChannelData[0] : nullptr;

        // Extract mono output (channel 0)
        float* output = (numOutputChannels > 0 && outputChannelData[0])
            ? outputChannelData[0] : nullptr;

        engine_.processBlock(input, output, numSamples);

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

static void printUsage() {
    fprintf(stdout, "Usage: retrospect [OPTIONS] [PORT]\n");
    fprintf(stdout, "Options:\n");
    fprintf(stdout, "  --jack                Use JACK audio backend\n");
    fprintf(stdout, "  --alsa                Use ALSA audio backend\n");
    fprintf(stdout, "  --headless            Run without TUI (server only)\n");
    fprintf(stdout, "  --connect HOST:PORT   Connect TUI to a remote server\n");
    fprintf(stdout, "  --midi-out NAME       Open MIDI output device (substring match)\n");
    fprintf(stdout, "  --list-midi           List available MIDI output devices\n");
    fprintf(stdout, "  --help                Show this help message\n");
    fprintf(stdout, "\nPORT: OSC server port (default: %s, used in TUI and headless modes)\n", kDefaultOscPort);
    fprintf(stdout, "\nExamples:\n");
    fprintf(stdout, "  retrospect                           TUI + server on port %s (default)\n", kDefaultOscPort);
    fprintf(stdout, "  retrospect 9000                      TUI + server on port 9000\n");
    fprintf(stdout, "  retrospect --headless                Headless server on port %s\n", kDefaultOscPort);
    fprintf(stdout, "  retrospect --headless 9000           Headless server on port 9000\n");
    fprintf(stdout, "  retrospect --connect localhost:7770  TUI-only, connect to remote\n");
    fprintf(stdout, "  retrospect --jack                    TUI + server using JACK\n");
    fprintf(stdout, "  retrospect --midi-out \"USB MIDI\"     Enable MIDI sync output\n");
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Parse command-line arguments
    juce::String preferredBackend;
    RunMode mode = RunMode::Tui;
    std::string serverPort = kDefaultOscPort;
    std::string connectTarget;  // "host:port" for TUI-only mode
    std::string midiOutName;    // MIDI output device name
    bool listMidi = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--jack") {
            preferredBackend = "JACK";
        } else if (arg == "--alsa") {
            preferredBackend = "ALSA";
        } else if (arg == "--headless") {
            mode = RunMode::Headless;
        } else if (arg == "--connect") {
            if (i + 1 < argc) {
                connectTarget = argv[++i];
                mode = RunMode::TuiOnly;
            } else {
                fprintf(stderr, "--connect requires HOST:PORT argument\n");
                return 1;
            }
        } else if (arg == "--midi-out") {
            if (i + 1 < argc) {
                midiOutName = argv[++i];
            } else {
                fprintf(stderr, "--midi-out requires a device name argument\n");
                return 1;
            }
        } else if (arg == "--list-midi") {
            listMidi = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg[0] != '-') {
            serverPort = arg;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    // Handle --list-midi (needs JUCE init for device enumeration)
    if (listMidi) {
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
        // Parse host:port
        auto colonPos = connectTarget.rfind(':');
        if (colonPos == std::string::npos) {
            fprintf(stderr, "Invalid connect target: %s (expected host:port)\n", connectTarget.c_str());
            return 1;
        }
        std::string host = connectTarget.substr(0, colonPos);
        std::string port = connectTarget.substr(colonPos + 1);

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

        tui.addMessage("Connected to " + connectTarget);
        tui.addMessage("Press 'q' to quit");

        while (g_running) {
            auto frameStart = std::chrono::steady_clock::now();

            if (!tui.update()) break;

            auto frameEnd = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                frameEnd - frameStart);
            auto sleepTime = std::chrono::milliseconds(kTuiRefreshMs) - elapsed;
            if (sleepTime.count() > 0) {
                std::this_thread::sleep_for(sleepTime);
            }
        }

        tui.shutdown();
        return 0;
    }

    // --- Modes that require audio ---
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioDeviceManager deviceManager;

    // Set preferred audio backend if specified
    if (preferredBackend.isNotEmpty()) {
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

    auto error = deviceManager.initialise(1, 1, nullptr, true);
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

    fprintf(stderr, "Using audio device: %s\n", device->getName().toRawUTF8());
    fprintf(stderr, "  Sample rate: %.0f Hz\n", sampleRate);
    fprintf(stderr, "  Buffer size: %d samples\n", bufferSize);

    // Create engine
    retrospect::LoopEngine engine(kMaxLoops, kMaxLookbackBars, sampleRate, kMinBpm);

    // Open MIDI output if requested
    std::unique_ptr<juce::MidiOutput> midiOutput;
    if (!midiOutName.empty()) {
        midiOutput = openMidiOutput(juce::String(midiOutName));
        if (midiOutput) {
            // Wire MIDI output to the engine's MidiSync
            juce::MidiOutput* rawPtr = midiOutput.get();
            engine.midiSync().setSendCallback([rawPtr](uint8_t statusByte) {
                rawPtr->sendMessageNow(juce::MidiMessage(statusByte));
            });
            engine.setMidiSyncEnabled(true);
        } else {
            fprintf(stderr, "Warning: MIDI output device '%s' not found\n", midiOutName.c_str());
            fprintf(stderr, "Available MIDI outputs:\n");
            auto devices = juce::MidiOutput::getAvailableDevices();
            for (const auto& d : devices) {
                fprintf(stderr, "  %s\n", d.name.toRawUTF8());
            }
        }
    }

    // Create and register audio callback
    AudioCallback audioCallback(engine);
    deviceManager.addAudioCallback(&audioCallback);

    // --- Headless mode (no TUI) ---
    if (mode == RunMode::Headless) {
        retrospect::OscServer oscServer(engine, serverPort);
        if (!oscServer.start()) {
            deviceManager.removeAudioCallback(&audioCallback);
            return 1;
        }

        fprintf(stderr, "Running headless on port %s\n", serverPort.c_str());
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
            auto sleepTime = std::chrono::milliseconds(kTuiRefreshMs) - elapsed;
            if (sleepTime.count() > 0) {
                std::this_thread::sleep_for(sleepTime);
            }
        }

        // Stop MIDI sync before shutting down
        engine.setMidiSyncEnabled(false);
        oscServer.stop();
        deviceManager.removeAudioCallback(&audioCallback);
        return 0;
    }

    // --- TUI mode (default): audio + OSC server + TUI ---
    retrospect::OscServer oscServer(engine, serverPort);
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
        char buf[64];
        snprintf(buf, sizeof(buf), "SR: %.0fHz  Buffer: %d", sampleRate, bufferSize);
        tui.addMessage(buf);
    }
    tui.addMessage("OSC server on port " + serverPort);
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
        auto sleepTime = std::chrono::milliseconds(kTuiRefreshMs) - elapsed;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
    }

    // Cleanup - stop MIDI sync before shutting down
    engine.setMidiSyncEnabled(false);
    oscServer.stop();
    deviceManager.removeAudioCallback(&audioCallback);
    tui.shutdown();

    return 0;
}
