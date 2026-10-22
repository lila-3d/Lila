#pragma once

#include "core/Log.h"

#include <cstddef>
#include <deque>
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
        if (mLines.size() > mKeep)
            mLines.pop_front();
        ++mAdded;
    }

    std::deque<OutputLine> mLines;
    std::string mPartial;
    LogLevel mPartialLevel = LogLevel::Output;
    size_t mKeep;
    size_t mAdded = 0;
};
} // namespace lila::ui
