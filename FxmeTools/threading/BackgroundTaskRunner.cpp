/*
  ==============================================================================

    BackgroundTaskRunner.cpp

  ==============================================================================
*/

#include "BackgroundTaskRunner.h"

namespace fxme
{

BackgroundTaskRunner::BackgroundTaskRunner (int maxConcurrentJobs)
    : pool (juce::jmax (1, maxConcurrentJobs))
{
}

BackgroundTaskRunner::~BackgroundTaskRunner()
{
    cancelAndWait();
}

void BackgroundTaskRunner::runJobs (std::vector<Job> jobs,
                                    std::function<void (float)> onProgress,
                                    std::function<void()> onFinished)
{
    cancelAndWait();

    numTotal = (int) jobs.size();
    numCompleted.store (0);
    onProgressCb = std::move (onProgress);
    onFinishedCb = std::move (onFinished);
    running.store (true);   // set before the empty-batch early-out too, so
                            // handleAsyncUpdate's exchange (false) below sees
                            // a true->false transition and still fires onFinished

    if (numTotal == 0)
    {
        triggerAsyncUpdate();
        return;
    }

    const int myGeneration = generation.load();

    for (auto& job : jobs)
    {
        pool.addJob ([this, job, myGeneration]
        {
            job();

            // Discard the result if a newer batch has started (or this one
            // was cancelled) since this job was queued.
            if (myGeneration == generation.load())
            {
                ++numCompleted;
                triggerAsyncUpdate();
            }
        });
    }
}

bool BackgroundTaskRunner::isRunning() const noexcept
{
    return running.load();
}

void BackgroundTaskRunner::cancelAndWait()
{
    ++generation;    // any in-flight job's completion is now discarded
    pool.removeAllJobs (true, 5000);
    cancelPendingUpdate();
    running.store (false);
}

void BackgroundTaskRunner::handleAsyncUpdate()
{
    const int done = juce::jmin (numCompleted.load(), numTotal);

    if (onProgressCb)
        onProgressCb (numTotal > 0 ? (float) done / (float) numTotal : 1.0f);

    if (done >= numTotal && running.exchange (false))
        if (onFinishedCb)
            onFinishedCb();
}

} // namespace fxme
