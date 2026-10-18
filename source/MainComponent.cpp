#include "MainComponent.h"

namespace
{
    const juce::Colour backgroundColour { 0xff1b1d21 };
    const juce::Colour panelColour      { 0xff26292f };
    const juce::Colour accentColour     { 0xff4da3ff };
    const juce::Colour dimTextColour    { 0xff9aa0a8 };

    const char* videoWildcards = "*.mp4;*.mov;*.m4v;*.avi;*.mkv;*.webm;*.mpg;*.mpeg;*.zip";
    const char* droppableExtensions = "mp4;mov;m4v;avi;mkv;webm;mpg;mpeg;zip";

    /** True if this drop is a batch (a folder, a zip, or more than one file). */
    bool looksLikeBatch (const juce::StringArray& paths)
    {
        if (paths.size() > 1)
            return true;

        if (paths.size() == 1)
        {
            const juce::File f (paths[0]);
            return f.isDirectory() || f.hasFileExtension ("zip");
        }

        return false;
    }
}

//==============================================================================

DropZone::DropZone (Side s, const juce::String& letterToShow)
    : side (s), letter (letterToShow)
{
    setInterceptsMouseClicks (true, false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void DropZone::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    const bool hasContent = displayName.isNotEmpty();

    g.setColour (dragging ? accentColour.withAlpha (0.18f) : panelColour);
    g.fillRoundedRectangle (bounds, 10.0f);

    // Dashed while empty, solid once filled.
    if (! hasContent && ! dragging)
    {
        g.setColour (juce::Colours::white.withAlpha (0.14f));

        juce::Path outline;
        outline.addRoundedRectangle (bounds.reduced (0.5f), 10.0f);

        const float dashes[] = { 6.0f, 5.0f };
        juce::Path dashed;
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
        g.fillPath (dashed);
    }
    else
    {
        g.setColour (dragging ? accentColour : accentColour.withAlpha (0.55f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 10.0f, 1.5f);
    }

    const float side_ = (float) juce::jmin (getWidth(), getHeight());

    if (! hasContent)
    {
        // A down arrow stacked over the letter, centred as one block. Same
        // arrow as the app sketch on the Splice web page.
        const float arrowSize    = side_ * 0.26f;
        const float letterHeight = side_ * 0.22f;
        const float gap          = side_ * 0.03f;

        auto stack = getLocalBounds().toFloat()
                         .withSizeKeepingCentre ((float) getWidth(), arrowSize + gap + letterHeight);
        const auto arrowArea = stack.removeFromTop (arrowSize).withSizeKeepingCentre (arrowSize, arrowSize);
        stack.removeFromTop (gap);

        // Drawn on a 48-unit grid, then scaled to the zone.
        const float unit = arrowSize / 48.0f;
        juce::Path arrow;
        arrow.startNewSubPath (24.0f, 10.0f);
        arrow.lineTo (24.0f, 32.0f);
        arrow.startNewSubPath (14.0f, 24.0f);
        arrow.lineTo (24.0f, 34.0f);
        arrow.lineTo (34.0f, 24.0f);
        arrow.applyTransform (juce::AffineTransform::scale (unit)
                                  .translated (arrowArea.getX(), arrowArea.getY()));

        g.setColour (accentColour);
        g.strokePath (arrow, juce::PathStrokeType (5.0f * unit, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        g.setColour (dragging ? accentColour : dimTextColour);
        g.setFont (juce::FontOptions (letterHeight, juce::Font::bold));
        g.drawText (letter, stack, juce::Justification::centred, false);
        return;
    }

    auto area = getLocalBounds().reduced (10);

    g.setColour (accentColour);
    g.setFont (juce::FontOptions (side_ * 0.16f, juce::Font::bold));
    g.drawText (letter, area.removeFromTop ((int) (side_ * 0.22f)),
                juce::Justification::centred, false);

    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (11.5f));
    g.drawFittedText (displayName, area, juce::Justification::centred, 3, 0.7f);
}

bool DropZone::isInterestedInFileDrag (const juce::StringArray& files)
{
    // Folders and zips are expanded later, so accept anything that might hold
    // video rather than validating contents mid-drag.
    for (const auto& f : files)
    {
        const juce::File file (f);

        if (file.isDirectory() || file.hasFileExtension (droppableExtensions))
            return true;
    }

    return false;
}

void DropZone::fileDragEnter (const juce::StringArray&, int, int)
{
    dragging = true;
    repaint();
}

void DropZone::fileDragExit (const juce::StringArray&)
{
    dragging = false;
    repaint();
}

void DropZone::filesDropped (const juce::StringArray& files, int, int)
{
    dragging = false;
    repaint();

    if (onFilesDropped != nullptr)
        onFilesDropped (side, files);
}

void DropZone::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasDraggedSinceMouseDown())
        browseForFiles();
}

void DropZone::browseForFiles()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a video, folder or zip",
                                                   juce::File(), videoWildcards);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::canSelectDirectories
                            | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
    {
        juce::StringArray paths;

        for (const auto& f : fc.getResults())
            paths.add (f.getFullPathName());

        if (! paths.isEmpty() && onFilesDropped != nullptr)
            onFilesDropped (side, paths);
    });
}

void DropZone::setLetter (const juce::String& newLetter)
{
    letter = newLetter;
    repaint();
}

void DropZone::setDisplayName (const juce::String& name)
{
    displayName = name;
    repaint();
}

//==============================================================================

MainComponent::MainComponent()
{
    addAndMakeVisible (colourZone);
    addAndMakeVisible (matteZone);

    auto onDrop = [this] (DropZone::Side s, const juce::StringArray& paths)
    {
        handleDrop (s, paths);
    };

    colourZone.onFilesDropped = onDrop;
    matteZone .onFilesDropped = onDrop;

    badge.setJustificationType (juce::Justification::centred);
    badge.setColour (juce::Label::textColourId, dimTextColour);
    badge.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (badge);

    auto setUpTab = [this] (juce::TextButton& b, Mode m)
    {
        b.setClickingTogglesState (false);
        b.setLookAndFeel (&tabLook);
        b.setWantsKeyboardFocus (false);
        b.setColour (juce::TextButton::buttonColourId, panelColour);
        b.setColour (juce::TextButton::buttonOnColourId, accentColour);
        b.setColour (juce::TextButton::textColourOffId, dimTextColour);
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        b.onClick = [this, m] { setMode (m); };
        addAndMakeVisible (b);
    };

    setUpTab (joinTab,  Mode::join);
    setUpTab (splitTab, Mode::split);

    // Update is the third segment: same fill and text as the tabs, never "on".
    updateButton.setButtonText ("Update");
    updateButton.setLookAndFeel (&tabLook);
    updateButton.setWantsKeyboardFocus (false);
    updateButton.setColour (juce::TextButton::buttonColourId, panelColour);
    updateButton.setColour (juce::TextButton::textColourOffId, dimTextColour);
    updateButton.onClick = [] { juce::URL (UpdateConfig::downloadUrl).launchInDefaultBrowser(); };
    addChildComponent (updateButton);

    updateChecker.check (JUCE_APPLICATION_VERSION_STRING, [this] (const juce::String& latest)
    {
        juce::ignoreUnused (latest);
        updateButton.setVisible (true);
        resized();
    });

    // Quality only affects the mp4s the splitter writes.
    quality.setRange (0.0, 1.0, 0.01);
    quality.setValue (0.75, juce::dontSendNotification);
    quality.setColour (juce::Slider::backgroundColourId, panelColour);
    quality.setColour (juce::Slider::trackColourId, accentColour);
    quality.setColour (juce::Slider::thumbColourId, accentColour);
    addChildComponent (quality);

    renderLook.disabledFillColour = panelColour;
    renderButton.setLookAndFeel (&renderLook);
    renderButton.setWantsKeyboardFocus (false);
    renderButton.setColour (juce::TextButton::buttonColourId, accentColour);
    renderButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    renderButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    renderButton.onClick = [this] { render(); };
    addAndMakeVisible (renderButton);

    progressBar.setColour (juce::ProgressBar::backgroundColourId, panelColour);
    progressBar.setColour (juce::ProgressBar::foregroundColourId, accentColour);
    progressBar.setLookAndFeel (&progressLook);
    addChildComponent (progressBar);

    batch.onProgress = [this] (double p) { progress = p < 0.0 ? -1.0 : p; };

    // Walk the zones through the batch so you can watch it work.
    batch.onJobStarted = [this] (int index) { showJob (index); };

    batch.onFinished = [this] (int done, int failed)
    {
        progressBar.setVisible (false);
        renderButton.setVisible (true);

        juce::String s;
        s << done << " done";

        if (failed > 0)
            s << " · " << failed << " failed";

        // Clear the job list and empty the zones, ready for the next drop —
        // but leave the result in the badge so it's still readable.
        clearJobs();
        badge.setText (s, juce::dontSendNotification);

        updateReadyState();
    };

    setMode (Mode::join);
    setSize (460, 400);
}

MainComponent::~MainComponent()
{
    joinTab.setLookAndFeel (nullptr);
    splitTab.setLookAndFeel (nullptr);
    updateButton.setLookAndFeel (nullptr);
    progressBar.setLookAndFeel (nullptr);
    renderButton.setLookAndFeel (nullptr);
    batch.onProgress   = nullptr;
    batch.onJobStarted = nullptr;
    batch.onFinished   = nullptr;
    batch.cancel();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);
}

void MainComponent::setMode (Mode newMode)
{
    mode = newMode;

    joinTab.setToggleState (mode == Mode::join, juce::dontSendNotification);
    splitTab.setToggleState (mode == Mode::split, juce::dontSendNotification);

    // Split has one input, so the matte zone stands down and the colour zone
    // becomes the source.
    colourZone.setLetter (mode == Mode::join ? "RGB" : "RGBA");
    matteZone.setVisible (mode == Mode::join);
    quality.setVisible (mode == Mode::split);

    // The same drop means different things per mode, so re-evaluate it.
    rescan();
    resized();
}

void MainComponent::handleDrop (DropZone::Side side, const juce::StringArray& paths)
{
    if (looksLikeBatch (paths) || mode == Mode::split)
    {
        // A folder/zip/multi-drop on either zone replaces the whole batch —
        // there's no wrong place to drop one.
        droppedPaths = paths;
        explicitColour = explicitMatte = juce::File();
        rescan();
        return;
    }

    // A single file sets just the side it landed on.
    const juce::File f (paths[0]);

    if (! f.hasFileExtension (droppableExtensions))
        return;

    if (side == DropZone::Side::colour)
        explicitColour = f;
    else
        explicitMatte = f;

    droppedPaths.clear();
    scanResult = {};

    // Once both sides are set by hand, that's a one-job batch.
    if (explicitColour != juce::File() && explicitMatte != juce::File())
        scanResult.jobs.add ({ JobScanner::stemForPair (explicitColour, explicitMatte),
                               explicitColour, explicitMatte });

    colourZone.setDisplayName (explicitColour != juce::File()
                                 ? explicitColour.getFileName() : juce::String());
    matteZone.setDisplayName (explicitMatte != juce::File()
                                ? explicitMatte.getFileName() : juce::String());
    badge.setText ({}, juce::dontSendNotification);

    updateReadyState();
}

void MainComponent::rescan()
{
    if (droppedPaths.isEmpty())
    {
        // Nothing batched — fall back to whatever was assigned by hand.
        if (explicitColour == juce::File() && explicitMatte == juce::File())
        {
            scanResult = {};
            colourZone.setDisplayName ({});
            matteZone.setDisplayName ({});
            badge.setText ({}, juce::dontSendNotification);
        }

        updateReadyState();
        return;
    }

    scanResult = mode == Mode::join ? scanner.scanForJoin (droppedPaths)
                                    : scanner.scanForSplit (droppedPaths);

    showJob (0);

    // The zones already show the pair, so the badge only carries what they
    // can't: how many more there are, and what got dropped.
    juce::String b;

    if (scanResult.jobs.size() > 1)
        b << scanResult.jobs.size() << (mode == Mode::join ? " pairs" : " clips");

    if (scanResult.skipped > 0)
    {
        if (b.isNotEmpty())
            b << " · ";

        b << scanResult.skipped << " skipped";
    }

    if (scanResult.jobs.isEmpty() && scanResult.skipped == 0)
        b = "nothing usable";

    badge.setText (b, juce::dontSendNotification);
    updateReadyState();
}

void MainComponent::clearJobs()
{
    // Everything that describes the pending work: the scanned jobs, the raw
    // drop they came from, and any hand-assigned sides.
    scanResult = {};
    droppedPaths.clear();
    explicitColour = explicitMatte = juce::File();

    colourZone.setDisplayName ({});
    matteZone.setDisplayName ({});
    badge.setText ({}, juce::dontSendNotification);
}

void MainComponent::showJob (int index)
{
    if (! juce::isPositiveAndBelow (index, scanResult.jobs.size()))
    {
        colourZone.setDisplayName ({});
        matteZone.setDisplayName ({});
        return;
    }

    const auto& job = scanResult.jobs.getReference (index);

    colourZone.setDisplayName (job.colourFile.getFileName());
    matteZone.setDisplayName (job.matteFile != juce::File()
                                ? job.matteFile.getFileName() : juce::String());
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (18);

    auto tabRow = area.removeFromTop (28);
    // Join | Split, plus | Update when the feed reports a newer version: attached
    // segments, centred as a group.
    const bool hasUpdate = updateButton.isVisible();
    const int segments = hasUpdate ? 3 : 2;
    const int segmentWidth = juce::jmin (100, tabRow.getWidth() / segments);
    auto tabs = tabRow.withSizeKeepingCentre (segmentWidth * segments, tabRow.getHeight());
    joinTab.setBounds (tabs.removeFromLeft (segmentWidth));
    splitTab.setBounds (tabs.removeFromLeft (segmentWidth));
    updateButton.setBounds (tabs);

    joinTab.setConnectedEdges (juce::Button::ConnectedOnRight);
    splitTab.setConnectedEdges (hasUpdate ? juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight
                                          : juce::Button::ConnectedOnLeft);
    updateButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

    area.removeFromTop (14);

    auto buttonRow = area.removeFromBottom (40);
    area.removeFromBottom (12);

    if (mode == Mode::split)
    {
        quality.setBounds (area.removeFromBottom (24));
        area.removeFromBottom (10);
    }

    badge.setBounds (area.removeFromBottom (18));
    area.removeFromBottom (8);

    const int gap = 14;

    if (mode == Mode::join)
    {
        // Two squares side by side, sized by whichever axis runs out first.
        const int side = juce::jmin ((area.getWidth() - gap) / 2, area.getHeight());
        auto boxes = area.withSizeKeepingCentre (side * 2 + gap, side);

        colourZone.setBounds (boxes.removeFromLeft (side));
        boxes.removeFromLeft (gap);
        matteZone.setBounds (boxes.removeFromLeft (side));
    }
    else
    {
        const int side = juce::jmin (area.getWidth(), area.getHeight());
        colourZone.setBounds (area.withSizeKeepingCentre (side, side));
    }

    renderButton.setBounds (buttonRow);
    progressBar.setBounds (buttonRow);
}

void MainComponent::updateReadyState()
{
    renderButton.setEnabled (! scanResult.jobs.isEmpty() && ! batch.isRunning());
}

void MainComponent::render()
{
    if (scanResult.jobs.isEmpty())
        return;

    const auto firstSource = scanResult.jobs.getReference (0).colourFile;
    const auto startDir = lastOutputDir != juce::File() ? lastOutputDir
                                                        : firstSource.getParentDirectory();

    saveChooser = std::make_unique<juce::FileChooser> ("Choose an output folder", startDir);

    saveChooser->launchAsync (juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();

        if (dir == juce::File())
            return;

        lastOutputDir = dir;

        BatchRunner::Settings s;
        s.joinMode        = (mode == Mode::join);
        s.outputDirectory = dir;
        s.quality         = quality.getValue();

        juce::String error;

        if (! batch.start (scanResult.jobs, s, error))
            return;

        progress = 0.0;
        renderButton.setVisible (false);
        progressBar.setVisible (true);
        updateReadyState();
    });
}
