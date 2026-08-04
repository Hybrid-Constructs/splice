#include "BatchRunner.h"

BatchRunner::BatchRunner() : juce::Thread ("BatchRun") {}

BatchRunner::~BatchRunner()
{
    cancel();
}

bool BatchRunner::start (const juce::Array<JobScanner::Job>& jobs,
                         const Settings& settings,
                         juce::String& error)
{
    if (isThreadRunning())
    {
        error = "A batch is already running.";
        return false;
    }

    if (jobs.isEmpty())
    {
        error = "Nothing to do.";
        return false;
    }

    queue   = jobs;
    current = settings;

    startThread (juce::Thread::Priority::normal);
    return true;
}

void BatchRunner::cancel()
{
    signalThreadShouldExit();
    stopThread (10000);
}

void BatchRunner::run()
{
    int done = 0, failed = 0;
    const int total = queue.size();

    for (int i = 0; i < total && ! threadShouldExit(); ++i)
    {
        const auto& job = queue.getReference (i);

        if (onJobStarted != nullptr)
            juce::MessageManager::callAsync ([cb = onJobStarted, i] { cb (i); });

        // NOTE: the engines deliver onFinished via MessageManager::callAsync,
        // so we must NOT wait on that callback here — this thread would block
        // until the message thread ran, and ~BatchRunner (called from the
        // message thread) would then deadlock against it. Instead we poll the
        // engine's own thread-safe isEncoding()/isSplitting() and read success
        // from the output file afterwards.
        std::atomic<bool> ok { false };

        // Scale each job's own progress into its slice of the batch.
        auto reportSlice = [this, i, total] (double jobProgress)
        {
            if (onProgress == nullptr)
                return;

            const double p = jobProgress < 0.0
                               ? -1.0
                               : ((double) i + juce::jlimit (0.0, 1.0, jobProgress))
                                    / (double) total;

            juce::MessageManager::callAsync ([cb = onProgress, p] { cb (p); });
        };

        if (current.joinMode)
        {
            HapEncoder encoder;

            HapEncoder::Settings s;
            s.colourFile = job.colourFile;
            s.matteFile  = job.matteFile;
            s.outputFile = current.outputDirectory.getChildFile (job.stem + "_hap.mov");

            encoder.onProgress = [reportSlice] (double p, const juce::String&)
            {
                reportSlice (p);
            };

            juce::String error;

            if (! encoder.start (s, error))
            {
                ++failed;
                continue;
            }

            while (encoder.isEncoding())
            {
                if (threadShouldExit())
                {
                    encoder.cancel();
                    break;
                }

                wait (50);
            }

            encoder.onProgress = nullptr;
            encoder.cancel();   // joins the encoder thread before it goes away

            // The encoder deletes its output on failure, so its presence is the
            // success signal.
            ok = s.outputFile.existsAsFile() && s.outputFile.getSize() > 0;
        }
        else
        {
            HapSplitter splitter;

            HapSplitter::Settings s;
            s.sourceFile      = job.colourFile;
            s.outputDirectory = current.outputDirectory;
            s.quality         = current.quality;

            splitter.onProgress = [reportSlice] (double p) { reportSlice (p); };

            juce::String error;

            if (! splitter.start (s, error))
            {
                ++failed;
                continue;
            }

            while (splitter.isSplitting())
            {
                if (threadShouldExit())
                {
                    splitter.cancel();
                    break;
                }

                wait (50);
            }

            splitter.onProgress = nullptr;
            splitter.cancel();

            // Both outputs must exist for the split to count as done.
            const auto rgbOut = current.outputDirectory.getChildFile (job.stem + "_rgb.mp4");
            const auto aOut   = current.outputDirectory.getChildFile (job.stem + "_a.mp4");
            ok = rgbOut.existsAsFile() && aOut.existsAsFile();
        }

        if (ok)
            ++done;
        else
            ++failed;
    }

    if (onProgress != nullptr)
        juce::MessageManager::callAsync ([cb = onProgress] { cb (1.0); });

    if (onFinished != nullptr)
        juce::MessageManager::callAsync ([cb = onFinished, done, failed]
        {
            cb (done, failed);
        });
}
