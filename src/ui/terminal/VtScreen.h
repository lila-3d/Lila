#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::term
{
enum Attr : uint8_t
{
    Bold = 1 << 0,
    Dim = 1 << 1,
    Italic = 1 << 2,
    Underline = 1 << 3,
    Inverse = 1 << 4,
    Hidden = 1 << 5,
    Strike = 1 << 6,
};

enum class MouseTracking : uint8_t
{
    Off,
    X10,    // 9: presses only
    Normal, // 1000: presses and releases
    Button, // 1002: plus motion while a button is held
    Any,    // 1003: plus all motion
};

enum class MouseEncoding : uint8_t
{
    Default, // CSI M with bytes offset by 32
    Utf8,    // 1005
    Sgr,     // 1006: CSI < b;x;y M/m
    Urxvt,   // 1015: CSI b;x;y M
};

/** Gray level 0..255, or kDefaultColor for the terminal's own foreground/background. */
constexpr int16_t kDefaultColor = -1;

struct Cell
{
    char32_t ch = U' ';
    int16_t fg = kDefaultColor;
    int16_t bg = kDefaultColor;
    uint8_t attrs = 0;
    uint8_t width = 1; // 2 = first half of a wide char, 0 = its trailing half

    bool operator==(const Cell&) const = default;
};

using Line = std::vector<Cell>;

/** Perceived lightness (0..255) of an sRGB color; how every SGR color is shown in a grayscale UI. */
int grayFromRgb(int r, int g, int b);
/** Gray level for an xterm 256-color index. */
int grayFromIndex(int index);
/** Display width of a code point: 0, 1 or 2. */
int charWidth(char32_t c);

/**
 * VT100/xterm screen model: feed it the bytes a program writes and it keeps the grid, cursor and scrollback.
 * Handles UTF-8, C0 controls, CSI cursor movement/erase/scroll/insert/delete, SGR (colors folded to gray),
 * DECSTBM, DECSC/DECRC, the alternate screen, cursor visibility, application cursor keys, bracketed paste, mouse
 * tracking modes, focus reporting and synchronized output. Erasing the whole main screen (ED 2) first moves its
 * contents into the scrollback, as Windows Terminal does, so full redraws keep their history; ED 3 clears it.
 * OSC strings are swallowed. Replies to queries (DSR, DA) are queued for takeResponses().
 */
class VtScreen
{
public:
    VtScreen(int cols = 80, int rows = 24, int scrollback = 5000);

    void feed(std::string_view bytes);
    void resize(int cols, int rows);
    void setScrollbackLimit(int lines);

    int cols() const { return mCols; }
    int rows() const { return mRows; }
    int scrollbackSize() const { return int(mScrollback.size()); }

    /** Visible screen row 0..rows-1. */
    const Line& screenLine(int row) const { return (*mActive)[row]; }
    /** -scrollbackSize()..rows-1; negative indexes reach into history (main screen only). */
    const Line& historyLine(int index) const;

    /** Lines dropped from the top of the scrollback so far; lets callers keep stable line ids. */
    int64_t droppedLines() const { return mDropped; }

    int cursorX() const { return mCursorX; }
    int cursorY() const { return mCursorY; }
    bool cursorVisible() const { return mCursorVisible; }
    bool altScreen() const { return mActive == &mAlt; }
    bool bracketedPaste() const { return mBracketedPaste; }
    bool appCursorKeys() const { return mAppCursorKeys; }
    /** DECSET 9/1000/1002/1003: which mouse events the program asked for. */
    MouseTracking mouseTracking() const { return mMouseTracking; }
    /** DECSET 1005/1006/1015: how mouse reports are encoded. */
    MouseEncoding mouseEncoding() const { return mMouseEncoding; }
    /** DECSET 1007: the wheel sends cursor keys on the alternate screen. On by default, like Windows Terminal. */
    bool alternateScroll() const { return mAlternateScroll; }
    /** DECSET 2026: the program is mid-frame; a view should hold its repaint until this clears. */
    bool synchronizedOutput() const { return mSyncOutput; }
    /** DECSET 1004: the program wants CSI I / CSI O on focus changes. */
    bool focusReporting() const { return mFocusReporting; }
    const std::string& title() const { return mTitle; }

    /** Text of history lines [fromLine, toLine] with columns clipped at either end, trailing spaces trimmed. */
    std::string text(int fromLine, int fromCol, int toLine, int toCol) const;
    std::string lineText(int index) const;

    struct Match
    {
        int line = 0; // history index, as historyLine()
        int col = 0;  // cell column
        int length = 0; // in cells
    };
    /**
     * The nearest occurrence of needle (UTF-8, case-insensitive for ASCII unless matchCase) strictly before (backward)
     * or after the cell (line, col), on the visible screen and history. Matches don't span lines.
     */
    std::optional<Match> find(std::string_view needle, int line, int col, bool backward, bool matchCase = false) const;

    /** Bytes the terminal must send back to the program (DSR/DA replies). */
    std::string takeResponses();

    /** Bumped on every change, so a view can skip repaints. */
    uint64_t revision() const { return mRevision; }

private:
    enum class State : uint8_t
    {
        Ground,
        Escape,
        EscapeIntermediate,
        Csi,
        Osc,
        OscEscape,
        String, // DCS/SOS/PM/APC: ignored up to ST
        StringEscape,
    };

    void step(char32_t c);
    void put(char32_t c);
    void control(char c);
    void escape(char c);
    void csi(char final);
    void sgr();
    void setMode(bool on);
    void lineFeed();
    void reverseIndex();
    void carriageReturn() { mCursorX = 0, mWrapPending = false; }
    void scrollUp(int top, int bottom, int n);
    void scrollDown(int top, int bottom, int n);
    void eraseCells(int row, int from, int to);
    void eraseDisplayToHistory();
    void moveTo(int x, int y);
    void enterAlt(bool clear, bool saveCursor);
    void leaveAlt(bool restoreCursor);
    void saveCursor();
    void restoreCursor();
    void trimScrollback();
    Cell blank() const;
    int param(size_t i, int fallback) const;

    using Grid = std::vector<Line>;

    int mCols, mRows;
    int mScrollbackLimit;
    Grid mMain, mAlt;
    Grid* mActive = &mMain;
    std::deque<Line> mScrollback;
    int64_t mDropped = 0;

    int mCursorX = 0, mCursorY = 0;
    bool mWrapPending = false;
    bool mCursorVisible = true;
    bool mAutoWrap = true;
    bool mOriginMode = false;
    bool mInsertMode = false;
    bool mBracketedPaste = false;
    bool mAppCursorKeys = false;
    MouseTracking mMouseTracking = MouseTracking::Off;
    MouseEncoding mMouseEncoding = MouseEncoding::Default;
    bool mAlternateScroll = true;
    bool mSyncOutput = false;
    bool mFocusReporting = false;
    int mTop = 0, mBottom = 0; // scroll region, inclusive

    Cell mPen; // current SGR state; ch unused

    struct Saved
    {
        int x = 0, y = 0;
        Cell pen;
        bool originMode = false;
    };
    Saved mSaved, mSavedMain;

    State mState = State::Ground;
    std::vector<int> mParams;
    bool mParamStarted = false;
    char mPrivate = 0;      // '?', '>', '<', '=' after CSI
    char mIntermediate = 0; // ' ', '!', '"', ...
    bool mColon = false;    // ':' sub-parameters seen
    bool mGraphicsG0 = false; // ESC ( 0: DEC line drawing
    std::string mOsc;
    std::string mTitle;
    std::string mResponses;
    char32_t mLastChar = U' ';

    uint32_t mUtf8 = 0;
    int mUtf8Need = 0;

    uint64_t mRevision = 0;
};
} // namespace lila::term
