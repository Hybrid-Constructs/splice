#include "UpdateChecker.h"

bool isNewerVersion (const juce::String& candidate, const juce::String& current)
{
    const auto a = juce::StringArray::fromTokens (candidate.trim(), ".", {});
    const auto b = juce::StringArray::fromTokens (current.trim(), ".", {});

    // getIntValue stops at the first non-digit, so "1-beta" reads as 1.
    for (int i = 0; i < juce::jmax (a.size(), b.size()); ++i)
    {
        const int x = i < a.size() ? a[i].getIntValue() : 0;
        const int y = i < b.size() ? b[i].getIntValue() : 0;

        if (x != y)
            return x > y;
    }

    return false;
}

//==============================================================================

UpdateChecker::UpdateChecker() : juce::Thread ("UpdateCheck") {}

UpdateChecker::~UpdateChecker()
{
    {
        const juce::ScopedLock sl (streamLock);

        if (activeStream != nullptr)
            activeStream->cancel();
    }

    stopThread (2000);
}

void UpdateChecker::check (const juce::String& installedVersion,
                           std::function<void (const juce::String&)> onUpdateFound)
{
    if (! UpdateConfig::isConfigured() || isThreadRunning())
        return;

    currentVersion = installedVersion;
    callback       = std::move (onUpdateFound);

    startThread (juce::Thread::Priority::background);
}

void UpdateChecker::run()
{
    juce::String body;

    {
        juce::WebInputStream stream (juce::URL (UpdateConfig::feedUrl), false);
        stream.withConnectionTimeout (5000);

        {
            const juce::ScopedLock sl (streamLock);
            activeStream = &stream;
        }

        if (stream.connect (nullptr) && stream.getStatusCode() == 200)
            body = stream.readEntireStreamAsString();

        {
            const juce::ScopedLock sl (streamLock);
            activeStream = nullptr;
        }
    }

    if (threadShouldExit() || body.isEmpty())
        return;

    const auto feed = juce::JSON::parse (body);
    auto latest = feed["version"].toString();

    // The feed's revision is the release's build number, which the app carries as the
    // fourth component (1.0.0.N), so a revision-only release still counts as newer.
    const auto revision = feed["revision"].toString();

    if (latest.isNotEmpty() && revision.isNotEmpty() && revision.containsOnly ("0123456789"))
        latest << "." << revision;

    if (latest.isEmpty() || ! isNewerVersion (latest, currentVersion))
        return;

    juce::MessageManager::callAsync ([weak = juce::WeakReference<UpdateChecker> (this), latest]
    {
        if (auto* self = weak.get())
            if (self->callback)
                self->callback (latest);
    });
}
