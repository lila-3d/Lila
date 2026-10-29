#pragma once

#include "core/Log.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace lila::ui
{
struct OutputLine
{
    LogLevel level = LogLevel::Info;
    std::string text;
};

/**
 * The last few lines of the script output, for the status bar's output line. One entry per line; Python's stdout and
 * command echoes arrive in pieces, so they are joined up until their newline. Debug and blank lines are dropped.
 */
class OutputTail
{
public:
    explicit OutputTail(size_t keep = 8) : mKeep(keep) {}

    /** True when lines() changed. */
    bool add(LogLevel level, std::string_view text)
    {
        if (level == LogLevel::Debug)
            return false;
        size_t before = mAdded;
        if (level == LogLevel::Output || level == LogLevel::Echo)
        {
            if (!mPartial.empty() && mPartialLevel != level)
                flushPartial();
            mPartialLevel = level;
            mPartial += text;
            size_t nl;
            while ((nl = mPartial.find('\n')) != std::string::npos)
            {
                push(level, std::string_view(mPartial).substr(0, nl));
                mPartial.erase(0, nl + 1);
            }
        }
        else
        {
            flushPartial();
            size_t start = 0;
            while (start <= text.size())
            {
                size_t nl = text.find('\n', start);
                size_t end = nl == std::string_view::npos ? text.size() : nl;
                push(level, text.substr(start, end - start));
                start = end + 1;
            }
        }
        return mAdded != before;
    }

    const std::deque<OutputLine>& lines() const { return mLines; }

    /**
     * Ends a burst: the lines added since the last commit, which is one command's worth when called once the UI thread
     * is idle again. The burst's last info, warning or error becomes display(), else its last stdout line (a return
     * value), else its last echo. Echoes come before or after a command's report depending on where it ran, so the
     * report wins either way. True when display() changed.
     */
    bool commit()
    {
        std::optional<OutputLine>& pick = mBurstReport ? mBurstReport : mBurstOutput ? mBurstOutput : mBurstEcho;
        bool changed = pick.has_value();
        if (changed)
            mDisplay = std::move(*pick);
        mBurstReport.reset();
        mBurstOutput.reset();
        mBurstEcho.reset();
        return changed;
    }

    /** The line the status bar shows, or null before anything was committed. */
    const OutputLine* display() const { return mDisplay ? &*mDisplay : nullptr; }

    /** Clears display() until the next commit that brings a line. */
    void dismiss() { mDisplay.reset(); }

private:
    void flushPartial()
    {
        if (!mPartial.empty())
            push(mPartialLevel, mPartial);
        mPartial.clear();
    }

    void push(LogLevel level, std::string_view line)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.find_first_not_of(" \t") == std::string_view::npos)
            return;
        mLines.push_back({level, std::string(line)});
        (level == LogLevel::Echo ? mBurstEcho : level == LogLevel::Output ? mBurstOutput : mBurstReport) = mLines.back();
        if (mLines.size() > mKeep)
            mLines.pop_front();
        ++mAdded;
    }

    std::deque<OutputLine> mLines;
    std::optional<OutputLine> mBurstReport, mBurstOutput, mBurstEcho, mDisplay;
    std::string mPartial;
    LogLevel mPartialLevel = LogLevel::Output;
    size_t mKeep;
    size_t mAdded = 0;
};
} // namespace lila::ui
