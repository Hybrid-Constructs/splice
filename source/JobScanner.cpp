#include "JobScanner.h"
#include "HapSplitter.h"

namespace
{
    const char* videoExtensions = "mp4;mov;m4v;avi;mkv;webm;mpg;mpeg";

    /** Files macOS/Windows scatter about that should never count as input. */
    bool isJunk (const juce::File& f)
    {
        const auto name = f.getFileName();
        return name.startsWithChar ('.')            // .DS_Store, ._resource forks
            || name.equalsIgnoreCase ("Thumbs.db");
    }
}

//==============================================================================

juce::String JobScanner::Result::describe (bool joinMode) const
{
    juce::String s;

    if (jobs.isEmpty())
        s << (skipped > 0 ? "nothing usable" : "");
    else
        s << jobs.size() << (joinMode ? (jobs.size() == 1 ? " pair" : " pairs")
                                      : (jobs.size() == 1 ? " clip" : " clips"));

    if (skipped > 0)
    {
        if (s.isNotEmpty())
            s << "\n";

        s << skipped << " skipped";
    }

    return s;
}

//==============================================================================

juce::String JobScanner::stemFor (const juce::File& file, bool& isColour, bool& isMatte)
{
    const auto name = file.getFileNameWithoutExtension();

    isColour = false;
    isMatte  = false;

    // Longest suffix first, so "_rgb" isn't mistaken for something shorter.
    // A bare "_rgb.mp4" / "_a.mp4" leaves nothing to name the output after, so
    // those are treated as unconventional rather than pairing on an empty stem.
    if (name.endsWithIgnoreCase ("_rgb") && name.length() > 4)
    {
        isColour = true;
        return name.dropLastCharacters (4);
    }

    if (name.endsWithIgnoreCase ("_a") && name.length() > 2)
    {
        isMatte = true;
        return name.dropLastCharacters (2);
    }

    return {};
}

juce::String JobScanner::stemForPair (const juce::File& colour, const juce::File& matte)
{
    bool isColour = false, isMatte = false;

    if (const auto stem = stemFor (colour, isColour, isMatte); isColour)
        return stem;

    if (const auto stem = stemFor (matte, isColour, isMatte); isMatte)
        return stem;

    return colour.getFileNameWithoutExtension();
}

juce::Array<juce::File> JobScanner::gatherVideoFiles (const juce::StringArray& droppedPaths)
{
    juce::Array<juce::File> files;

    // A fresh extract dir per scan, so a re-drop doesn't see stale contents.
    zipHolder.reset();
    zipExtractDir = juce::File();

    for (const auto& path : droppedPaths)
    {
        const juce::File f (path);

        if (! f.exists() || isJunk (f))
            continue;

        if (f.isDirectory())
        {
            juce::Array<juce::File> found;
            f.findChildFiles (found, juce::File::findFiles, true);

            for (const auto& child : found)
                if (! isJunk (child) && child.hasFileExtension (videoExtensions))
                    files.add (child);
        }
        else if (f.hasFileExtension ("zip"))
        {
            juce::ZipFile zip (f);

            if (zip.getNumEntries() <= 0)
                continue;

            // TemporaryFile gives us a unique path; use a sibling dir for the
            // extracted contents so everything is cleaned up together.
            if (zipExtractDir == juce::File())
            {
                zipHolder = std::make_unique<juce::TemporaryFile> ("splice_zip");
                zipExtractDir = zipHolder->getFile().getParentDirectory()
                                  .getChildFile (zipHolder->getFile().getFileNameWithoutExtension()
                                                   + "_extracted");
                zipExtractDir.createDirectory();
            }

            auto target = zipExtractDir.getChildFile (f.getFileNameWithoutExtension());
            target.createDirectory();

            if (zip.uncompressTo (target, true).wasOk())
            {
                juce::Array<juce::File> found;
                target.findChildFiles (found, juce::File::findFiles, true);

                for (const auto& child : found)
                    if (! isJunk (child) && child.hasFileExtension (videoExtensions))
                        files.add (child);
            }
        }
        else if (f.hasFileExtension (videoExtensions))
        {
            files.add (f);
        }
    }

    return files;
}

JobScanner::Result JobScanner::scanForJoin (const juce::StringArray& droppedPaths)
{
    Result result;
    const auto files = gatherVideoFiles (droppedPaths);

    // Bucket by stem so _rgb and _a find each other regardless of drop order.
    // The key is lowercased — the suffix match is case-insensitive, so the stem
    // must be too, or "Clip_RGB" + "clip_a" would land in separate buckets and
    // both be reported as half a pair. The display name keeps its original case.
    struct Pair { juce::File colour, matte; juce::String displayStem; };
    juce::HashMap<juce::String, Pair> pairs;
    juce::StringArray order;              // keeps output order stable
    juce::Array<juce::File> unconventional;

    for (const auto& f : files)
    {
        bool isColour = false, isMatte = false;
        const auto stem = stemFor (f, isColour, isMatte);

        if (stem.isEmpty())
        {
            unconventional.add (f);
            continue;
        }

        const auto key = stem.toLowerCase();

        if (! pairs.contains (key))
        {
            Pair fresh;
            fresh.displayStem = stem;      // first spelling seen wins
            pairs.set (key, fresh);
            order.add (key);
        }

        auto p = pairs[key];

        if (isColour) p.colour = f;
        if (isMatte)  p.matte  = f;

        pairs.set (key, p);
    }

    for (const auto& key : order)
    {
        const auto p = pairs[key];

        if (p.colour != juce::File() && p.matte != juce::File())
            result.jobs.add ({ p.displayStem, p.colour, p.matte });
        else
            ++result.skipped;             // half a pair is no pair
    }

    // Exactly two loose files with no convention: treat as one explicit pair,
    // first dropped as colour. This is the old two-zone case, preserved.
    if (result.jobs.isEmpty() && order.isEmpty() && unconventional.size() == 2)
    {
        result.jobs.add ({ stemForPair (unconventional[0], unconventional[1]),
                           unconventional[0], unconventional[1] });
        return result;
    }

    result.skipped += unconventional.size();
    return result;
}

JobScanner::Result JobScanner::scanForSplit (const juce::StringArray& droppedPaths)
{
    Result result;

    for (const auto& f : gatherVideoFiles (droppedPaths))
    {
        if (HapSplitter::sourceHasAlpha (f))
            result.jobs.add ({ f.getFileNameWithoutExtension(), f, juce::File() });
        else
            ++result.skipped;
    }

    return result;
}
