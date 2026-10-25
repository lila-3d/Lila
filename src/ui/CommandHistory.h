#pragma once

#include <QString>
#include <QStringList>

#include <algorithm>

namespace lila::ui
{
/**
 * The quick command line's history: oldest first, each command once, at most `cap` entries. A walk starts on the newest
 * entry (what the line opens prefilled with); older() steps back and stops at the oldest, newer() steps forward and
 * past the newest gives an empty line.
 */
class CommandHistory
{
public:
    explicit CommandHistory(int cap = 200) : mCap(std::max(1, cap)) {}

    /** Replaces the entries, as loaded from prefs: blanks and repeats dropped (the later copy kept), then capped. */
    void setEntries(const QStringList& entries)
    {
        mEntries.clear();
        for (const QString& e : entries)
            push(e);
        resetWalk();
    }
    const QStringList& entries() const { return mEntries; }

    /** Records a command as the newest, moving it there if it was already in the list. */
    void add(const QString& command)
    {
        push(command);
        resetWalk();
    }

    /** The newest command, or "". */
    QString last() const { return mEntries.isEmpty() ? QString() : mEntries.back(); }

    void resetWalk() { mPos = std::max(0, int(mEntries.size()) - 1); }
    QString older()
    {
        if (mEntries.isEmpty())
            return QString();
        mPos = std::max(0, mPos - 1);
        return mEntries[mPos];
    }
    QString newer()
    {
        mPos = std::min(int(mEntries.size()), mPos + 1);
        return mPos < mEntries.size() ? mEntries[mPos] : QString();
    }

private:
    void push(const QString& command)
    {
        if (command.trimmed().isEmpty())
            return;
        mEntries.removeAll(command);
        mEntries.append(command);
        while (mEntries.size() > mCap)
            mEntries.removeFirst();
    }

    QStringList mEntries;
    int mCap;
    int mPos = 0;
};
} // namespace lila::ui
