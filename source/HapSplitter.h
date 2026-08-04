#pragma once

#include <JuceHeader.h>
#include <functional>

/**
    The reverse of HapEncoder: takes one clip that carries an alpha channel and
    writes two H.264 .mp4s beside it — "<name>_rgb.mp4" (colour) and
    "<name>_a.mp4" (alpha as greyscale).

    Runs on a background thread; callbacks fire on the message thread.
*/
class HapSplitter : private juce::Thread
{
public:
    struct Settings
    {
        juce::File sourceFile;
        juce::File outputDirectory;

        /** 0.0 = smallest file, 1.0 = best quality. Maps to an x264 CRF. */
        double quality = 0.75;

        /**
            Divide RGB back out by alpha before writing. HapEncoder premultiplies
            against black, so undoing it here makes split -> re-encode round-trip
            back to the original colour.
        */
        bool unpremultiply = true;
    };

    HapSplitter();
    ~HapSplitter() override;

    bool start (const Settings& settings, juce::String& error);
    void cancel();
    bool isSplitting() const { return isThreadRunning(); }

    /** True if the file has an alpha channel we can usefully split out. */
    static bool sourceHasAlpha (const juce::File& file);

    std::function<void (double progress)> onProgress;
    std::function<void (bool success, const juce::String& message)> onFinished;

private:
    void run() override;
    bool split (juce::String& error);

    Settings current;
    std::atomic<bool> cancelled { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HapSplitter)
};
