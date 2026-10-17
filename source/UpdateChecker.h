#pragma once

#include <JuceHeader.h>

/**
    Where to look for updates and where to send the user when there is one.

    The feed is a public, read-only route that serves Splice's version number
    and nothing else, so no key is needed. Clearing `downloadUrl` turns the
    check off: no request is made and no button is shown.
*/
namespace UpdateConfig
{
    inline const char* const feedUrl = "https://api.hybridconstructs.com/v2/public/updates/splice";

    /** Opened in the browser when the user clicks the update button. */
    inline const char* const downloadUrl = "https://hybridconstructs.com/splice";

    inline bool isConfigured()
    {
        return downloadUrl[0] != '\0';
    }
}

/** True if `candidate` is a higher dotted version than `current` ("1.10.0" > "1.9.2"). */
bool isNewerVersion (const juce::String& candidate, const juce::String& current);

/**
    Asks the update feed for the latest version once, on a worker thread.

    This is the app's only network call. It is a plain GET with no parameters:
    nothing identifies the machine or the user. Any failure (offline, timeout,
    bad reply) reads as "no update", since there is nothing useful to tell the
    user.
*/
class UpdateChecker : private juce::Thread
{
public:
    UpdateChecker();
    ~UpdateChecker() override;

    /** Starts a check. `onUpdateFound` is called on the message thread with the
        newer version string, and only if there is one. Ignored if a check is
        already running or no download page is configured. */
    void check (const juce::String& currentVersion,
                std::function<void (const juce::String& latestVersion)> onUpdateFound);

private:
    void run() override;

    juce::String currentVersion;
    std::function<void (const juce::String&)> callback;

    juce::CriticalSection streamLock;
    juce::WebInputStream* activeStream = nullptr;   // so the destructor can abort a slow read

    JUCE_DECLARE_WEAK_REFERENCEABLE (UpdateChecker)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};
