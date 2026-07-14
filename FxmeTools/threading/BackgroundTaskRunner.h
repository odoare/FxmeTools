/*
  ==============================================================================

    BackgroundTaskRunner.h

    Runs a batch of independent jobs (e.g. "load and analyze speaker N", one
    job per speaker) on a small internal juce::ThreadPool, reports aggregate
    progress and calls a completion callback — both always delivered on the
    MESSAGE THREAD (via juce::AsyncUpdater), so the callbacks can safely touch
    Components, ValueTrees or any other message-thread-affine state without
    the caller having to hop threads itself.

    Typical use: a button click assembles N independent closures (each
    touching its own private data, e.g. its own analysis engine instance),
    calls runJobs(), and drives a juce::ProgressBar (which takes a double& it
    polls itself) from onProgress:

        double progress = 0.0;
        juce::ProgressBar bar { progress };   // add as a child component

        runner.runJobs (std::move (jobs),
            [this, &progress] (float p) { progress = (double) p; },
            [this] { applyResults(); refreshUi(); });

    Jobs themselves must be self-contained: no two jobs in the same batch may
    touch the same data, and a job must not touch anything message-thread
    affine (only onFinished, running back on the message thread once every
    job has completed, is a safe place to do that).

    Cancellation (cancelAndWait()) is best-effort: pending (not yet started)
    jobs are dropped immediately; jobs already running are plain
    std::function<void()> closures with no way to check "should I stop early",
    so cancelAndWait() can only wait for them to finish naturally. A job that
    finishes after cancellation is detected internally and its result is
    discarded rather than corrupting a subsequently-started batch.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <vector>

namespace fxme
{

class BackgroundTaskRunner : private juce::AsyncUpdater
{
public:
    /** One independent unit of work, run on a background thread. Must not
        touch any juce::Component or other message-thread-affine state. */
    using Job = std::function<void()>;

    /** maxConcurrentJobs defaults to a conservative fraction of the machine's
        core count (leaving headroom for the audio thread and the host),
        clamped to [1, 4]. */
    explicit BackgroundTaskRunner (int maxConcurrentJobs =
        juce::jlimit (1, 4, juce::SystemStats::getNumCpus() / 2));
    ~BackgroundTaskRunner() override;

    /** Starts running `jobs` in the background; any batch already running is
        cancelled first (see cancelAndWait()). onProgress(fractionDone 0..1)
        and onFinished() are always invoked on the message thread; onFinished
        fires exactly once per call, even when `jobs` is empty. */
    void runJobs (std::vector<Job> jobs,
                  std::function<void (float)> onProgress,
                  std::function<void()> onFinished);

    /** True from runJobs() until onFinished has been delivered. */
    bool isRunning() const noexcept;

    /** Drops any not-yet-started jobs and blocks until in-flight ones finish.
        Neither onProgress nor onFinished is called for the cancelled batch.
        Safe to call when nothing is running. */
    void cancelAndWait();

private:
    void handleAsyncUpdate() override;

    juce::ThreadPool pool;
    std::atomic<int> generation { 0 };   // bumped on every runJobs/cancelAndWait
    std::atomic<int> numCompleted { 0 };
    std::atomic<bool> running { false };
    int numTotal = 0;
    std::function<void (float)> onProgressCb;
    std::function<void()> onFinishedCb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BackgroundTaskRunner)
};

} // namespace fxme
