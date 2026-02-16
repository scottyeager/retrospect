#include "core/LoopEngine.h"
#include "tui/Tui.h"

#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <csignal>
#include <atomic>

// Simulated audio parameters
static constexpr double kSampleRate = 44100.0;
static constexpr int kBlockSize = 512;
static constexpr int kMaxLoops = 8;
static constexpr int kMaxLookbackBars = 8;
static constexpr double kMinBpm = 60.0;

// TUI refresh rate (target ~30fps)
static constexpr int kTuiRefreshMs = 33;

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

/// Generate a simple sine tone for simulation purposes.
/// This produces a continuous tone so that captured loops have audible content
/// when eventually played back through real audio output.
static void generateSimulatedInput(float* buffer, int numSamples,
                                   double& phase, double freq) {
    double phaseInc = 2.0 * M_PI * freq / kSampleRate;
    for (int i = 0; i < numSamples; ++i) {
        buffer[i] = static_cast<float>(std::sin(phase) * 0.3);
        phase += phaseInc;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
}

int main() {
    // Set up signal handler for clean shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Create engine
    retrospect::LoopEngine engine(kMaxLoops, kMaxLookbackBars, kSampleRate, kMinBpm);

    // Create TUI
    retrospect::Tui tui(engine);

    // Wire engine callbacks to TUI
    retrospect::EngineCallbacks callbacks;
    callbacks.onMessage = [&tui](const std::string& msg) {
        tui.addMessage(msg);
    };
    callbacks.onStateChanged = []() {
        // TUI redraws every frame anyway
    };
    callbacks.onBeat = [](const retrospect::MetronomePosition&) {
        // Could add audio click here
    };
    callbacks.onBar = [](const retrospect::MetronomePosition&) {
        // Could add accented click here
    };
    engine.setCallbacks(std::move(callbacks));

    // Initialize TUI
    if (!tui.init()) {
        fprintf(stderr, "Failed to initialize TUI\n");
        return 1;
    }

    tui.addMessage("Retrospect started - simulation mode");
    tui.addMessage("Audio is simulated (sine wave input)");
    tui.addMessage("Press 'q' to quit");

    // Simulation loop: runs the audio engine in simulated time and
    // updates the TUI at a reasonable frame rate.
    //
    // In a real build with JUCE, the audio callback would drive
    // processBlock() instead of this simulation loop.

    float inputBuffer[kBlockSize];
    float outputBuffer[kBlockSize];
    double phase = 0.0;
    double simFreq = 440.0; // A4

    // Track how many audio samples to process per TUI frame to maintain
    // approximate real-time pacing
    double samplesPerFrame = kSampleRate * kTuiRefreshMs / 1000.0;

    while (g_running) {
        auto frameStart = std::chrono::steady_clock::now();

        // Process enough audio blocks to fill this frame's time
        int samplesThisFrame = static_cast<int>(samplesPerFrame);
        int blocksThisFrame = (samplesThisFrame + kBlockSize - 1) / kBlockSize;

        for (int block = 0; block < blocksThisFrame; ++block) {
            int samplesInBlock = std::min(kBlockSize,
                                          samplesThisFrame - block * kBlockSize);
            if (samplesInBlock <= 0) break;

            // Generate simulated input
            generateSimulatedInput(inputBuffer, samplesInBlock, phase, simFreq);

            // Clear output
            std::memset(outputBuffer, 0, sizeof(float) * static_cast<size_t>(samplesInBlock));

            // Process
            engine.processBlock(inputBuffer, outputBuffer, samplesInBlock);
        }

        // Update TUI
        if (!tui.update()) {
            break;
        }

        // Sleep to maintain frame rate
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
