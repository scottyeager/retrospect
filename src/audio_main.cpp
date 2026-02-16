#include "core/LoopEngine.h"
#include "tui/Tui.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <cstdio>

static constexpr int kMaxLoops = 8;
static constexpr int kMaxLookbackBars = 8;
static constexpr double kMinBpm = 60.0;
static constexpr int kTuiRefreshMs = 33;

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

int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // JUCE requires this for AudioDeviceManager
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Set up audio device
    juce::AudioDeviceManager deviceManager;
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

    // Create engine with the device's actual sample rate
    retrospect::LoopEngine engine(kMaxLoops, kMaxLookbackBars, sampleRate, kMinBpm);

    // Create and register audio callback
    AudioCallback audioCallback(engine);
    deviceManager.addAudioCallback(&audioCallback);

    // Create TUI
    retrospect::Tui tui(engine);

    // Wire engine callbacks to TUI (these fire from TUI thread via schedule methods)
    retrospect::EngineCallbacks callbacks;
    callbacks.onMessage = [&tui](const std::string& msg) {
        tui.addMessage(msg);
    };
    callbacks.onStateChanged = []() {};
    callbacks.onBeat = [](const retrospect::MetronomePosition&) {};
    callbacks.onBar = [](const retrospect::MetronomePosition&) {};
    engine.setCallbacks(std::move(callbacks));

    // Initialize TUI
    if (!tui.init()) {
        fprintf(stderr, "Failed to initialize TUI\n");
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
    tui.addMessage("Press 'q' to quit");

    // Main loop: TUI at ~30fps
    while (g_running) {
        auto frameStart = std::chrono::steady_clock::now();

        if (!tui.update()) {
            break;
        }

        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            frameEnd - frameStart);
        auto sleepTime = std::chrono::milliseconds(kTuiRefreshMs) - elapsed;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
    }

    // Cleanup
    deviceManager.removeAudioCallback(&audioCallback);
    tui.shutdown();

    return 0;
}
