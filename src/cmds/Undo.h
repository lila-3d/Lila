#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lila
{
/** Buffers an undo action shares with the scene or with other steps: each key counts once across the whole stack. */
struct UndoShared
{
    std::vector<std::pair<const void*, size_t>> items;
    void add(const void* key, size_t bytes)
    {
        if (key)
            items.emplace_back(key, bytes);
    }
};

/** One reversible primitive edit. Subsystems record these; commands never write their own undo. */
struct UndoAction
{
    virtual ~UndoAction() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    /** True for selection changes, which do not make the scene unsaved. */
    virtual bool selectionOnly() const { return false; }
    /** Approximate bytes this action owns outright, for the undo memory budget. */
    virtual size_t bytes() const { return 64; }
    /** Shared buffers (meshes, data payloads) this action keeps alive. */
    virtual void sharedBytes(UndoShared&) const {}
};

/** Main thread only. */
class UndoStack
{
public:
    static UndoStack& get();

    /** Chunks nest; only the outermost one lands on the stack. */
    void openChunk(std::string label);
    void closeChunk();

    /** Dropped silently while undoing/redoing or when recording is off. */
    void record(std::unique_ptr<UndoAction> action);

    bool undo();
    bool redo();
    void clear();
    /** Drops all undo/redo steps but keeps the scene state id, so an unchanged scene stays saved. */
    void flush();
    /** Drops the steps undo would revert, keeping the redo steps and the scene state id. */
    void flushPast();
    /** Drops the steps redo would replay. */
    void flushFuture();

    bool canUndo() const { return !mDone.empty(); }
    bool canRedo() const { return !mUndone.empty(); }
    std::string undoLabel() const { return mDone.empty() ? std::string() : mDone.back().label; }
    std::string redoLabel() const { return mUndone.empty() ? std::string() : mUndone.back().label; }

    void setEnabled(bool on) { mEnabled = on; }
    bool enabled() const { return mEnabled; }
    bool replaying() const { return mReplaying; }

    /** 0 keeps every step. */
    void setLimit(size_t limit);
    size_t limit() const { return mLimit; }
    /** Oldest steps drop while the stack holds more than this many bytes; 0 is unlimited. */
    void setByteBudget(size_t bytes);
    size_t byteBudget() const { return mBudget; }
    /** Approximate bytes held by every undo and redo step, shared buffers counted once. */
    size_t memoryBytes() const { return mOwnedBytes + mSharedBytes; }

    struct Step
    {
        std::string label;
        bool selectionOnly = false;
    };
    /** Undo steps oldest first, then redo steps in the order redo would replay them. */
    std::vector<Step> steps() const;
    /** Steps undo can go back through; steps()[undoCount() - 1] is the current one. */
    size_t undoCount() const { return mDone.size(); }
    size_t redoCount() const { return mUndone.size(); }
    /** Undoes or redoes until undoCount() == position, notifying listeners once. False if out of range. */
    bool jumpTo(size_t position);

    /** Changes whenever the stack does (a step pushed, undone, redone or cleared), so callers can tell if theirs is still on top. */
    uint64_t serial() const { return mSerial; }

    /** Identifies the scene state the stack sits at: equal ids mean undo/redo brought the scene back to the same state. */
    uint64_t stateId() const;

    /** Called after a step is pushed, undone, redone or the stack cleared. */
    void addListener(std::function<void()> fn) { mListeners.push_back(std::move(fn)); }

private:
    struct Chunk
    {
        std::string label;
        std::vector<std::unique_ptr<UndoAction>> actions;
        uint64_t id = 0;
        bool selectionOnly = false;
        size_t owned = 0;
        UndoShared shared;
    };
    struct SharedEntry
    {
        size_t bytes = 0;
        size_t refs = 0;
    };

    void push(Chunk c);
    void notify();
    void account(const Chunk& c, bool add);
    void resetAccounting();
    void trim();

    std::deque<Chunk> mDone;
    std::vector<Chunk> mUndone;
    Chunk mOpen;
    int mDepth = 0;
    bool mEnabled = true;
    bool mReplaying = false;
    size_t mLimit = 0;
    size_t mBudget = 0;
    size_t mOwnedBytes = 0;
    size_t mSharedBytes = 0;
    std::unordered_map<const void*, SharedEntry> mShared;
    bool mQuiet = false;
    uint64_t mSerial = 0;
    uint64_t mNextId = 1;
    uint64_t mBaseId = 0;
    std::vector<std::function<void()>> mListeners;
};

/** RAII chunk. */
struct UndoChunk
{
    explicit UndoChunk(std::string label) { UndoStack::get().openChunk(std::move(label)); }
    ~UndoChunk() { UndoStack::get().closeChunk(); }
    UndoChunk(const UndoChunk&) = delete;
    UndoChunk& operator=(const UndoChunk&) = delete;
};
} // namespace lila
