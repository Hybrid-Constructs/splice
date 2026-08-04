#pragma once

#include <JuceHeader.h>
#include "JobScanner.h"
#include "BatchRunner.h"

/**
    One half of the RGB / A pair display. Shows its letter when empty and a
    filename once it's holding one side of a job.

    Any drop lands here — a single file, a folder, a zip, a multi-file selection
    — and is handed up whole; MainComponent decides whether it set one side or
    replaced the entire batch.
*/
class DropZone : public juce::Component,
                 public juce::FileDragAndDropTarget
{
public:
    /** Which side of the pair this zone represents. */
    enum class Side { colour, matte };

    DropZone (Side s, const juce::String& letterToShow);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

    void setLetter (const juce::String& newLetter);

    /** The filename shown in the middle; empty returns the zone to its letter. */
    void setDisplayName (const juce::String& name);

    Side getSide() const noexcept { return side; }

    /** Reports the drop along with which zone received it. */
    std::function<void (Side, const juce::StringArray& paths)> onFilesDropped;

private:
    void browseForFiles();

    Side side;
    juce::String letter, displayName;
    bool dragging = false;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DropZone)
};

//==============================================================================

class MainComponent : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Mode { join, split };

    void setMode (Mode newMode);
    void handleDrop (DropZone::Side side, const juce::StringArray& paths);
    void rescan();
    void clearJobs();
    void showJob (int index);
    void updateReadyState();
    void render();

    Mode mode = Mode::join;

    DropZone colourZone { DropZone::Side::colour, "RGB" };
    DropZone matteZone  { DropZone::Side::matte,  "A" };

    // "12 pairs · 3 skipped" — sits between the zones and the button.
    juce::Label badge;

    juce::TextButton joinTab  { "Join" };
    juce::TextButton splitTab { "Split" };
    juce::Slider quality { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::TextButton renderButton { "Render" };
    juce::ProgressBar progressBar { progress };

    double progress = 0.0;

    // Explicit single-file assignments, used when no batch is loaded.
    juce::File explicitColour, explicitMatte;

    juce::StringArray droppedPaths;
    JobScanner scanner;
    JobScanner::Result scanResult;
    BatchRunner batch;

    juce::File lastOutputDir;
    std::unique_ptr<juce::FileChooser> saveChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
