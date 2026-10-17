#pragma once

#include <JuceHeader.h>

/**
    Turns whatever was dropped — a zip, a folder, or a loose selection of files —
    into a concrete list of jobs.

    Join pairs files by the "<stem>_rgb" / "<stem>_a" convention. Split just
    takes every video that carries an alpha channel.
*/
class JobScanner
{
public:
    /** One unit of work. For split, matteFile is unused. */
    struct Job
    {
        juce::String stem;      // the shared name, with _rgb/_a stripped
        juce::File   colourFile;
        juce::File   matteFile;
    };

    struct Result
    {
        juce::Array<Job> jobs;
        int skipped = 0;        // files we couldn't make a job out of

        /** Short summary for the drop zone, e.g. "12 pairs, 3 skipped". */
        juce::String describe (bool joinMode) const;
    };

    /**
        Expands the dropped paths and finds jobs.

        Zips are extracted into a temp folder that lives as long as this
        scanner, so the returned Files stay valid until it's destroyed or
        rescanned.
    */
    Result scanForJoin (const juce::StringArray& droppedPaths);
    Result scanForSplit (const juce::StringArray& droppedPaths);

    /** Strips a trailing _rgb / _a (case-insensitive). Empty if absent. */
    static juce::String stemFor (const juce::File& file, bool& isColour, bool& isMatte);

    /** The output name for a pair chosen by hand: the colour file's name with a
        trailing _rgb removed, else the matte's with _a removed, else the colour
        file's name as it is. */
    static juce::String stemForPair (const juce::File& colour, const juce::File& matte);

private:
    /** Flattens zips/folders/files into a plain list of video files. */
    juce::Array<juce::File> gatherVideoFiles (const juce::StringArray& droppedPaths);

    // Zips are unpacked here; cleaned up when the scanner is destroyed.
    std::unique_ptr<juce::TemporaryFile> zipHolder;
    juce::File zipExtractDir;

    JUCE_LEAK_DETECTOR (JobScanner)
};
