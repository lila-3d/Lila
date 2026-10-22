#pragma once

#include <functional>
#include <thread>

namespace lila::mainq
{
/** Call once from the main thread at startup. */
void init();
bool isMainThread();

/** Queue work for the main thread. Safe from any thread. */
void post(std::function<void()> fn);

/** Run everything queued so far. Main thread only. Does nothing while held. */
void drain();

/** Holds queued work back (drain does nothing) while long work pumps the event loop; nests. */
class Hold
{
public:
    Hold();
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

/** Called (on the posting thread) whenever post() queues work, so the host event loop can schedule a drain. */
void setWakeup(std::function<void()> wake);
} // namespace lila::mainq
