#include "retopo/Internal.h"

#include "core/Progress.h"

#include <chrono>
#include <cstdio>
#include <limits>

namespace lila::retopo::detail
{
namespace
{
thread_local Budget* tCurrent = nullptr;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
} // namespace

Budget::Budget(double seconds, progress::Scope* scope) : mSeconds(seconds), mStart(now()), mScope(scope), mPrev(tCurrent)
{
    tCurrent = this;
}

Budget::~Budget() { tCurrent = mPrev; }

void Budget::check() const
{
    if (mScope && mScope->cancelled())
        throw Interrupted("cancelled " + where());
    if (mSeconds > 0.0 && now() - mStart > mSeconds)
    {
        char took[32];
        std::snprintf(took, sizeof(took), "%g", mSeconds);
        throw Interrupted(std::string("gave up after ") + took + " s, " + where());
    }
}

void Budget::reached(const char* stage, double fraction)
{
    if (!tCurrent)
        return;
    tCurrent->mStage = stage;
    if (tCurrent->mScope)
    {
        tCurrent->mScope->set(fraction * tCurrent->mScope->total());
        tCurrent->mScope->setStatus(std::string("done: ") + stage);
    }
    tCurrent->check();
}

void Budget::checkpoint()
{
    if (tCurrent)
        tCurrent->check();
}

double Budget::remaining()
{
    if (!tCurrent || tCurrent->mSeconds <= 0.0)
        return std::numeric_limits<double>::infinity();
    return tCurrent->mSeconds - (now() - tCurrent->mStart);
}

void Budget::note(std::string fact)
{
    if (tCurrent)
        tCurrent->mNotes.push_back(std::move(fact));
}

std::string Budget::where() const
{
    std::string s = mStage.empty() ? std::string("while starting") : "after " + mStage;
    if (!mNotes.empty())
    {
        s += " (";
        for (size_t i = 0; i < mNotes.size(); ++i)
            s += (i ? ", " : "") + mNotes[i];
        s += ")";
    }
    return s;
}
} // namespace lila::retopo::detail
