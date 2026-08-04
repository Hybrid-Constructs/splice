#pragma once

#include <JuceHeader.h>
#include "JobScanner.h"
#include "HapEncoder.h"
#include "HapSplitter.h"

/**
    Runs a list of jobs one after another, joining or splitting each.

    Owns its own encoder/splitter and drives them from a worker thread, waiting
    for each to finish before starting the next. Progress is reported across the
    whole batch, not per job.
*/
class BatchRunner : private juce::Thread
{
public:
    BatchRunner();
    ~BatchRunner() override;

    struct Settings
    {
        bool       joinMode = true;
        juce::File outputDirectory;
        double     quality = 0.75;   // split only
    };

    bool start (const juce::Array<JobScanner::Job>& jobs,
                const Settings& settings,
                juce::String& error);

    void cancel();
    bool isRunning() const { return isThreadRunning(); }

    /** 0..1 across the whole batch. */
    std::function<void (double progress)> onProgress;

    /** Fires as each job starts, so the UI can show which pair is encoding. */
    std::function<void (int jobIndex)> onJobStarted;

    /** done = jobs that succeeded, failed = jobs that errored. */
    std::function<void (int done, int failed)> onFinished;

private:
    void run() override;

    juce::Array<JobScanner::Job> queue;
    Settings current;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BatchRunner)
};
