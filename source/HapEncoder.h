#pragma once

#include <JuceHeader.h>
#include <functional>

/**
    Combines two video files into a single Hap Alpha .mov:
    the RGB comes from the colour clip, the alpha from the luminance of the
    matte clip.

    Everything runs on a background thread. Progress and completion are
    reported through the callbacks, which are invoked on the message thread.
*/
class HapEncoder : private juce::Thread
{
public:
    /** How the matte clip's pixels are turned into an alpha value. */
    enum class MatteChannel
    {
        luminance,  // Rec.709 weighted luma — the default
        red,
        green,
        blue,
        alpha       // the matte's own alpha channel
    };

    /** What to do when the two clips differ in length. */
    enum class LengthMode
    {
        shortest,   // stop at the end of the shorter clip
        colour,     // run the colour clip's full length, holding the last matte frame
        matte       // run the matte clip's full length, holding the last colour frame
    };

    struct Settings
    {
        juce::File   colourFile;
        juce::File   matteFile;
        juce::File   outputFile;
        MatteChannel matteChannel = MatteChannel::luminance;
        LengthMode   lengthMode   = LengthMode::shortest;
        bool         invertMatte  = false;
        int          chunkCount   = 4;   // Hap chunks: enables parallel GPU decode
    };

    HapEncoder();
    ~HapEncoder() override;

    /** Kicks off an encode. Returns false (with an error) if one is already running. */
    bool start (const Settings& settings, juce::String& error);

    /** Asks a running encode to stop; blocks briefly until the thread is done. */
    void cancel();

    bool isEncoding() const { return isThreadRunning(); }

    /** Called on the message thread with 0.0–1.0, or -1.0 when the total is unknown. */
    std::function<void (double progress, const juce::String& status)> onProgress;

    /** Called on the message thread when the encode finishes, fails or is cancelled. */
    std::function<void (bool success, const juce::String& message)> onFinished;

    /** Reads width/height/duration/frame-rate without starting an encode. */
    struct ClipInfo
    {
        bool   valid = false;
        int    width = 0, height = 0;
        double frameRate = 0.0;
        double durationSeconds = 0.0;
        juce::String codecName;
        juce::String describe() const;
    };

    static ClipInfo probe (const juce::File& file);

private:
    void run() override;

    // Does the actual work; returns false and fills `error` on failure.
    bool encode (juce::String& error);

    void postProgress (double progress, const juce::String& status);

    Settings current;
    std::atomic<bool> cancelled { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HapEncoder)
};
