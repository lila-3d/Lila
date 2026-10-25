#include "ui/terminal/VtScreen.h"

#include <algorithm>
#include <cmath>

namespace lila::term
{
namespace
{
constexpr int kTabWidth = 8;
constexpr size_t kMaxOsc = 4096;
constexpr int kMaxParam = 65535;

constexpr Rgb kAnsi[16] = {
    {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},   {0, 0, 238},     {205, 0, 205},
    {0, 205, 205},   {229, 229, 229}, {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
};

// DEC special graphics for 0x60..0x7E, selected with ESC ( 0.
constexpr char32_t kLineDrawing[31] = {
    U'◆', U'▒', U'␉', U'␌', U'␍', U'␊', U'°', U'±',
    U'␤', U'␋', U'┘', U'┐', U'┌', U'└', U'┼', U'⎺',
    U'⎻', U'─', U'⎼', U'⎽', U'├', U'┤', U'┴', U'┬',
    U'│', U'≤', U'≥', U'π', U'≠', U'£', U'·',
};

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80)
        out += char(c);
    else if (c < 0x800)
    {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000)
    {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
    else
    {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

bool inRange(char32_t c, char32_t lo, char32_t hi)
{
    return c >= lo && c <= hi;
}

std::u32string decodeUtf8(std::string_view s)
{
    std::u32string out;
    for (size_t i = 0; i < s.size();)
    {
        auto b = (unsigned char)s[i];
        int need = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
        char32_t c = need == 0 ? b : need == 1 ? b & 0x1F : need == 2 ? b & 0x0F : b & 0x07;
        ++i;
        for (int k = 0; k < need && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80; ++k, ++i)
            c = (c << 6) | ((unsigned char)s[i] & 0x3F);
        out += need < 0 ? U'�' : c;
    }
    return out;
}
} // namespace

int grayFromRgb(int r, int g, int b)
{
    auto lin = [](int v) {
        double c = v / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    double y = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
    double s = y <= 0.0031308 ? y * 12.92 : 1.055 * std::pow(y, 1.0 / 2.4) - 0.055;
    return std::clamp(int(std::lround(s * 255.0)), 0, 255);
}

int grayFromIndex(int index)
{
    index = std::clamp(index, 0, 255);
    if (index < 16)
        return grayFromRgb(kAnsi[index].r, kAnsi[index].g, kAnsi[index].b);
    if (index < 232)
    {
        static constexpr int kLevels[6] = {0, 95, 135, 175, 215, 255};
        int i = index - 16;
        return grayFromRgb(kLevels[i / 36], kLevels[(i / 6) % 6], kLevels[i % 6]);
    }
    return 8 + (index - 232) * 10;
}

int grayFromColor(Color c)
{
    if (c & kRgbColor)
        return grayFromRgb((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
    return grayFromIndex(c);
}

Rgb rgbFromColor(Color c, const Rgb* ansi16)
{
    if (c & kRgbColor)
        return {(c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff};
    int index = std::clamp(int(c), 0, 255);
    if (index < 16)
        return (ansi16 ? ansi16 : kAnsi)[index];
    if (index < 232)
    {
        static constexpr int kLevels[6] = {0, 95, 135, 175, 215, 255};
        int i = index - 16;
        return {kLevels[i / 36], kLevels[(i / 6) % 6], kLevels[i % 6]};
    }
    int v = 8 + (index - 232) * 10;
    return {v, v, v};
}

int charWidth(char32_t c)
{
    if (c < 0x300)
        return 1;
    if (inRange(c, 0x0300, 0x036F) || inRange(c, 0x1AB0, 0x1AFF) || inRange(c, 0x1DC0, 0x1DFF) || inRange(c, 0x200B, 0x200F) ||
        inRange(c, 0x20D0, 0x20FF) || inRange(c, 0xFE00, 0xFE0F) || inRange(c, 0xFE20, 0xFE2F))
        return 0;
    if (inRange(c, 0x1100, 0x115F) || inRange(c, 0x2E80, 0x303E) || inRange(c, 0x3041, 0x33FF) || inRange(c, 0x3400, 0x4DBF) ||
        inRange(c, 0x4E00, 0x9FFF) || inRange(c, 0xA000, 0xA4CF) || inRange(c, 0xAC00, 0xD7A3) || inRange(c, 0xF900, 0xFAFF) ||
        inRange(c, 0xFE30, 0xFE4F) || inRange(c, 0xFF00, 0xFF60) || inRange(c, 0xFFE0, 0xFFE6) || inRange(c, 0x1F300, 0x1F64F) ||
        inRange(c, 0x1F680, 0x1F6FF) || inRange(c, 0x1F900, 0x1F9FF) || inRange(c, 0x20000, 0x3FFFD))
        return 2;
    return 1;
}

VtScreen::VtScreen(int cols, int rows, int scrollback)
    : mCols(std::max(1, cols)), mRows(std::max(1, rows)), mScrollbackLimit(std::max(0, scrollback))
{
    mMain.assign(mRows, Line(mCols));
    mAlt.assign(mRows, Line(mCols));
    mBottom = mRows - 1;
}

const Line& VtScreen::historyLine(int index) const
{
    if (index < 0)
        return mScrollback[mScrollback.size() + index];
    return (*mActive)[index];
}

Cell VtScreen::blank() const
{
    Cell c;
    c.bg = mPen.bg;
    return c;
}

int VtScreen::param(size_t i, int fallback) const
{
    return i < mParams.size() && mParams[i] >= 0 ? mParams[i] : fallback;
}

std::string VtScreen::takeResponses()
{
    std::string out;
    out.swap(mResponses);
    return out;
}

void VtScreen::setScrollbackLimit(int lines)
{
    mScrollbackLimit = std::max(0, lines);
    trimScrollback();
}

void VtScreen::trimScrollback()
{
    while (int(mScrollback.size()) > mScrollbackLimit)
    {
        mScrollback.pop_front();
        ++mDropped;
    }
}

void VtScreen::feed(std::string_view bytes)
{
    for (unsigned char b : bytes)
    {
        char32_t c;
        if (mUtf8Need > 0)
        {
            if ((b & 0xC0) == 0x80)
            {
                mUtf8 = (mUtf8 << 6) | (b & 0x3F);
                if (--mUtf8Need > 0)
                    continue;
                c = mUtf8 > 0x10FFFF || inRange(mUtf8, 0xD800, 0xDFFF) ? U'�' : char32_t(mUtf8);
            }
            else
            {
                // Truncated sequence: emit a replacement, then treat this byte afresh.
                mUtf8Need = 0;
                if (mState == State::Ground)
                    put(U'�');
                if (b >= 0x80)
                {
                    if ((b & 0xE0) == 0xC0)
                        mUtf8 = b & 0x1F, mUtf8Need = 1;
                    else if ((b & 0xF0) == 0xE0)
                        mUtf8 = b & 0x0F, mUtf8Need = 2;
                    else if ((b & 0xF8) == 0xF0)
                        mUtf8 = b & 0x07, mUtf8Need = 3;
                    continue;
                }
                c = b;
            }
        }
        else if (b < 0x80)
            c = b;
        else if ((b & 0xE0) == 0xC0)
        {
            mUtf8 = b & 0x1F, mUtf8Need = 1;
            continue;
        }
        else if ((b & 0xF0) == 0xE0)
        {
            mUtf8 = b & 0x0F, mUtf8Need = 2;
            continue;
        }
        else if ((b & 0xF8) == 0xF0)
        {
            mUtf8 = b & 0x07, mUtf8Need = 3;
            continue;
        }
        else
            c = U'�';

        step(c);
    }
    ++mRevision;
}

void VtScreen::step(char32_t c)
{
    if (c == 0x18 || c == 0x1A)
    {
        mState = State::Ground;
        return;
    }

    switch (mState)
    {
    case State::Ground:
        if (c < 0x20 || c == 0x7F)
            control(char(c));
        else
            put(c);
        break;

    case State::Escape:
        if (c == 0x1B)
            break;
        if (c < 0x20)
        {
            control(char(c));
            break;
        }
        if (c == '[')
        {
            mState = State::Csi;
            mParams.clear();
            mParamStarted = false;
            mPrivate = 0;
            mIntermediate = 0;
            mColon = false;
        }
        else if (c == ']')
        {
            mState = State::Osc;
            mOsc.clear();
        }
        else if (c == 'P' || c == 'X' || c == '^' || c == '_')
            mState = State::String;
        else if (c >= 0x20 && c <= 0x2F)
        {
            mIntermediate = char(c);
            mState = State::EscapeIntermediate;
        }
        else
        {
            mState = State::Ground;
            escape(char(c));
        }
        break;

    case State::EscapeIntermediate:
        if (c == 0x1B)
        {
            mState = State::Escape;
            break;
        }
        if (c >= 0x20 && c <= 0x2F)
            break;
        mState = State::Ground;
        if (mIntermediate == '(')
            mGraphicsG0 = c == '0';
        else if (mIntermediate == '#' && c == '8')
        {
            for (Line& line : *mActive)
                for (Cell& cell : line)
                    cell = Cell{U'E'};
        }
        break;

    case State::Csi:
        if (c == 0x1B)
        {
            mState = State::Escape;
            break;
        }
        if (c < 0x20)
            control(char(c));
        else if (c >= '0' && c <= '9')
        {
            if (!mParamStarted)
            {
                mParams.push_back(0);
                mParamStarted = true;
            }
            int& p = mParams.back();
            p = p < 0 ? int(c - '0') : std::min(kMaxParam, p * 10 + int(c - '0'));
        }
        else if (c == ';' || c == ':')
        {
            if (!mParamStarted)
                mParams.push_back(-1);
            mParamStarted = false;
            // Colon sub-parameters are flattened; SGR copes with the 38:2::r:g:b form.
            mColon |= c == ':';
        }
        else if (c >= '<' && c <= '?')
        {
            if (mParams.empty() && !mParamStarted)
                mPrivate = char(c);
        }
        else if (c >= 0x20 && c <= 0x2F)
            mIntermediate = char(c);
        else if (c >= 0x40 && c <= 0x7E)
        {
            if (!mParamStarted && !mParams.empty())
                mParams.push_back(-1);
            mState = State::Ground;
            csi(char(c));
        }
        else
            mState = State::Ground;
        break;

    case State::Osc:
        if (c == 0x07)
        {
            mState = State::Ground;
            if (mOsc.size() > 2 && (mOsc[0] == '0' || mOsc[0] == '2') && mOsc[1] == ';')
                mTitle = mOsc.substr(2);
        }
        else if (c == 0x1B)
            mState = State::OscEscape;
        else if (mOsc.size() < kMaxOsc)
            appendUtf8(mOsc, c);
        break;

    case State::OscEscape:
        if (mOsc.size() > 2 && (mOsc[0] == '0' || mOsc[0] == '2') && mOsc[1] == ';')
            mTitle = mOsc.substr(2);
        mState = State::Ground;
        if (c != '\\')
        {
            mState = State::Escape;
            step(c);
        }
        break;

    case State::String:
        if (c == 0x1B)
            mState = State::StringEscape;
        break;

    case State::StringEscape:
        mState = c == '\\' ? State::Ground : State::String;
        break;
    }
}

void VtScreen::control(char c)
{
    switch (c)
    {
    case 0x08:
        if (mCursorX > 0)
            --mCursorX;
        mWrapPending = false;
        break;
    case 0x09:
        mCursorX = std::min(mCols - 1, (mCursorX / kTabWidth + 1) * kTabWidth);
        mWrapPending = false;
        break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        lineFeed();
        break;
    case 0x0D:
        carriageReturn();
        break;
    case 0x1B:
        mState = State::Escape;
        break;
    default:
        break;
    }
}

void VtScreen::escape(char c)
{
    switch (c)
    {
    case '7':
        saveCursor();
        break;
    case '8':
        restoreCursor();
        break;
    case 'D':
        lineFeed();
        break;
    case 'E':
        carriageReturn();
        lineFeed();
        break;
    case 'M':
        reverseIndex();
        break;
    case 'c':
    {
        int sb = mScrollbackLimit;
        std::deque<Line> history = std::move(mScrollback);
        int64_t dropped = mDropped;
        uint64_t rev = mRevision;
        *this = VtScreen(mCols, mRows, sb);
        mScrollback = std::move(history);
        mDropped = dropped;
        mRevision = rev;
        break;
    }
    default:
        break;
    }
}

void VtScreen::put(char32_t c)
{
    if (mGraphicsG0 && c >= 0x60 && c <= 0x7E)
        c = kLineDrawing[c - 0x60];
    int w = charWidth(c);
    if (w == 0)
        return;
    mLastChar = c;

    if (mWrapPending && mAutoWrap)
    {
        carriageReturn();
        lineFeed();
    }
    mWrapPending = false;

    if (w == 2 && mCursorX == mCols - 1)
    {
        if (mCols < 2)
            return;
        if (mAutoWrap)
        {
            eraseCells(mCursorY, mCursorX, mCols);
            carriageReturn();
            lineFeed();
        }
        else
            mCursorX = mCols - 2;
    }

    Line& line = (*mActive)[mCursorY];
    if (mInsertMode)
    {
        line.insert(line.begin() + mCursorX, w, blank());
        line.resize(mCols);
        if (line.back().width == 2)
            line.back() = blank();
    }

    // Overwriting half of a wide char leaves the other half blank.
    auto clearPair = [&](int x) {
        if (x < 0 || x >= mCols)
            return;
        if (line[x].width == 0 && x > 0)
            line[x - 1] = blank();
        if (line[x].width == 2 && x + 1 < mCols)
            line[x + 1] = blank();
    };
    clearPair(mCursorX);
    if (w == 2)
        clearPair(mCursorX + 1);

    Cell cell = mPen;
    cell.ch = c;
    cell.width = uint8_t(w);
    line[mCursorX] = cell;
    if (w == 2)
    {
        cell.ch = 0;
        cell.width = 0;
        line[mCursorX + 1] = cell;
    }

    mCursorX += w;
    if (mCursorX >= mCols)
    {
        mCursorX = mCols - 1;
        mWrapPending = mAutoWrap;
    }
}

void VtScreen::lineFeed()
{
    mWrapPending = false;
    if (mCursorY == mBottom)
        scrollUp(mTop, mBottom, 1);
    else if (mCursorY < mRows - 1)
        ++mCursorY;
}

void VtScreen::reverseIndex()
{
    mWrapPending = false;
    if (mCursorY == mTop)
        scrollDown(mTop, mBottom, 1);
    else if (mCursorY > 0)
        --mCursorY;
}

void VtScreen::scrollUp(int top, int bottom, int n)
{
    Grid& g = *mActive;
    n = std::clamp(n, 0, bottom - top + 1);
    bool toHistory = mActive == &mMain && top == 0 && mScrollbackLimit > 0;
    for (int i = 0; i < n; ++i)
    {
        if (toHistory)
            mScrollback.push_back(g[top]);
        std::rotate(g.begin() + top, g.begin() + top + 1, g.begin() + bottom + 1);
        g[bottom].assign(mCols, blank());
    }
    if (toHistory)
        trimScrollback();
}

void VtScreen::scrollDown(int top, int bottom, int n)
{
    Grid& g = *mActive;
    n = std::clamp(n, 0, bottom - top + 1);
    for (int i = 0; i < n; ++i)
    {
        std::rotate(g.begin() + top, g.begin() + bottom, g.begin() + bottom + 1);
        g[top].assign(mCols, blank());
    }
}

void VtScreen::eraseCells(int row, int from, int to)
{
    Line& line = (*mActive)[row];
    from = std::clamp(from, 0, mCols);
    to = std::clamp(to, 0, mCols);
    if (from > 0 && from < mCols && line[from].width == 0)
        line[from - 1] = blank();
    if (to < mCols && to > 0 && line[to].width == 0)
        line[to] = blank();
    for (int x = from; x < to; ++x)
        line[x] = blank();
}

void VtScreen::eraseDisplayToHistory()
{
    if (mActive == &mMain && mScrollbackLimit > 0)
    {
        auto used = [](const Line& line) {
            return std::any_of(line.begin(), line.end(), [](const Cell& c) {
                return (c.ch != U' ' && c.ch != 0) || c.bg != kDefaultColor || (c.attrs & (Inverse | Underline | Strike));
            });
        };
        int last = mRows - 1;
        while (last >= 0 && !used(mMain[last]))
            --last;
        for (int y = 0; y <= last; ++y)
            mScrollback.push_back(mMain[y]);
        trimScrollback();
    }
    for (int y = 0; y < mRows; ++y)
        eraseCells(y, 0, mCols);
}

void VtScreen::moveTo(int x, int y)
{
    int top = mOriginMode ? mTop : 0;
    int bottom = mOriginMode ? mBottom : mRows - 1;
    mCursorX = std::clamp(x, 0, mCols - 1);
    mCursorY = std::clamp(y + top, top, bottom);
    mWrapPending = false;
}

void VtScreen::saveCursor()
{
    mSaved = {mCursorX, mCursorY, mPen, mOriginMode};
}

void VtScreen::restoreCursor()
{
    mCursorX = std::clamp(mSaved.x, 0, mCols - 1);
    mCursorY = std::clamp(mSaved.y, 0, mRows - 1);
    mPen = mSaved.pen;
    mOriginMode = mSaved.originMode;
    mWrapPending = false;
}

void VtScreen::enterAlt(bool clear, bool save)
{
    if (save)
        mSavedMain = {mCursorX, mCursorY, mPen, mOriginMode};
    if (mActive == &mAlt)
        return;
    mActive = &mAlt;
    if (clear)
        for (Line& line : mAlt)
            line.assign(mCols, blank());
}

void VtScreen::leaveAlt(bool restore)
{
    mActive = &mMain;
    if (restore)
    {
        mCursorX = std::clamp(mSavedMain.x, 0, mCols - 1);
        mCursorY = std::clamp(mSavedMain.y, 0, mRows - 1);
        mPen = mSavedMain.pen;
        mOriginMode = mSavedMain.originMode;
    }
    mWrapPending = false;
}

void VtScreen::setMode(bool on)
{
    for (size_t i = 0; i < std::max<size_t>(1, mParams.size()); ++i)
    {
        int m = param(i, 0);
        if (mPrivate == '?')
        {
            switch (m)
            {
            case 1:
                mAppCursorKeys = on;
                break;
            case 6:
                mOriginMode = on;
                moveTo(0, 0);
                break;
            case 7:
                mAutoWrap = on;
                break;
            case 9:
            case 1000:
            case 1002:
            case 1003:
            {
                MouseTracking t = m == 9      ? MouseTracking::X10
                                  : m == 1000 ? MouseTracking::Normal
                                  : m == 1002 ? MouseTracking::Button
                                              : MouseTracking::Any;
                if (on)
                    mMouseTracking = t;
                else if (mMouseTracking == t)
                    mMouseTracking = MouseTracking::Off;
                break;
            }
            case 25:
                mCursorVisible = on;
                break;
            case 47:
            case 1047:
                if (on)
                    enterAlt(m == 1047, false);
                else
                {
                    if (m == 1047 && mActive == &mAlt)
                        for (Line& line : mAlt)
                            line.assign(mCols, blank());
                    leaveAlt(false);
                }
                break;
            case 1004:
                mFocusReporting = on;
                break;
            case 1005:
            case 1006:
            case 1015:
            {
                MouseEncoding e = m == 1005 ? MouseEncoding::Utf8 : m == 1006 ? MouseEncoding::Sgr : MouseEncoding::Urxvt;
                if (on)
                    mMouseEncoding = e;
                else if (mMouseEncoding == e)
                    mMouseEncoding = MouseEncoding::Default;
                break;
            }
            case 1007:
                mAlternateScroll = on;
                break;
            case 1048:
                on ? saveCursor() : restoreCursor();
                break;
            case 1049:
                if (on)
                    enterAlt(true, true);
                else
                    leaveAlt(true);
                break;
            case 2004:
                mBracketedPaste = on;
                break;
            case 2026:
                mSyncOutput = on;
                break;
            default:
                break;
            }
        }
        else if (mPrivate == 0 && m == 4)
            mInsertMode = on;
    }
}

void VtScreen::sgr()
{
    if (mParams.empty())
    {
        mPen = Cell{};
        return;
    }
    auto extended = [&](size_t& i) -> Color {
        int mode = param(i + 1, -1);
        if (mode == 5)
        {
            int idx = param(i + 2, 0);
            i += 2;
            return std::clamp(idx, 0, 255);
        }
        if (mode == 2)
        {
            // 38;2;r;g;b, or the colon form with a color-space slot: 38:2::r:g:b.
            size_t first = i + 2;
            if (mColon && mParams.size() >= i + 6)
                first = i + 3;
            int r = param(first, 0), g = param(first + 1, 0), b = param(first + 2, 0);
            i = first + 2;
            return rgbColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
        }
        i += 1;
        return kDefaultColor;
    };

    for (size_t i = 0; i < mParams.size(); ++i)
    {
        int p = param(i, 0);
        switch (p)
        {
        case 0:
            mPen = Cell{};
            break;
        case 1:
            mPen.attrs |= Bold;
            break;
        case 2:
            mPen.attrs |= Dim;
            break;
        case 3:
            mPen.attrs |= Italic;
            break;
        case 4:
        case 21:
            mPen.attrs |= Underline;
            break;
        case 7:
            mPen.attrs |= Inverse;
            break;
        case 8:
            mPen.attrs |= Hidden;
            break;
        case 9:
            mPen.attrs |= Strike;
            break;
        case 22:
            mPen.attrs &= ~(Bold | Dim);
            break;
        case 23:
            mPen.attrs &= ~Italic;
            break;
        case 24:
            mPen.attrs &= ~Underline;
            break;
        case 27:
            mPen.attrs &= ~Inverse;
            break;
        case 28:
            mPen.attrs &= ~Hidden;
            break;
        case 29:
            mPen.attrs &= ~Strike;
            break;
        case 38:
            mPen.fg = extended(i);
            break;
        case 39:
            mPen.fg = kDefaultColor;
            break;
        case 48:
            mPen.bg = extended(i);
            break;
        case 49:
            mPen.bg = kDefaultColor;
            break;
        case 58:
            extended(i); // underline color: consumed, not shown
            break;
        default:
            if (p >= 30 && p <= 37)
                mPen.fg = p - 30;
            else if (p >= 40 && p <= 47)
                mPen.bg = p - 40;
            else if (p >= 90 && p <= 97)
                mPen.fg = p - 90 + 8;
            else if (p >= 100 && p <= 107)
                mPen.bg = p - 100 + 8;
            break;
        }
    }
}

void VtScreen::csi(char final)
{
    int n = std::max(1, param(0, 1));
    bool plain = mPrivate == 0 && mIntermediate == 0;
    Grid& g = *mActive;

    if (!plain)
    {
        if (mPrivate == '?' && (final == 'h' || final == 'l'))
            setMode(final == 'h');
        else if (mPrivate == '>' && final == 'c')
            mResponses += "\x1b[>0;10;1c";
        else if (mIntermediate == '!' && final == 'p')
        {
            // DECSTR soft reset.
            mCursorVisible = true;
            mOriginMode = mInsertMode = mAppCursorKeys = false;
            mAutoWrap = true;
            mTop = 0;
            mBottom = mRows - 1;
            mPen = Cell{};
            mSaved = {};
            mSyncOutput = false;
        }
        return;
    }

    switch (final)
    {
    case '@':
    {
        Line& line = g[mCursorY];
        n = std::min(n, mCols - mCursorX);
        line.insert(line.begin() + mCursorX, n, blank());
        line.resize(mCols);
        if (line.back().width == 2)
            line.back() = blank();
        break;
    }
    case 'A':
        mCursorY = std::max(mCursorY - n, mCursorY >= mTop ? mTop : 0);
        mWrapPending = false;
        break;
    case 'B':
    case 'e':
        mCursorY = std::min(mCursorY + n, mCursorY <= mBottom ? mBottom : mRows - 1);
        mWrapPending = false;
        break;
    case 'C':
    case 'a':
        mCursorX = std::min(mCursorX + n, mCols - 1);
        mWrapPending = false;
        break;
    case 'D':
        mCursorX = std::max(mCursorX - n, 0);
        mWrapPending = false;
        break;
    case 'E':
        mCursorY = std::min(mCursorY + n, mCursorY <= mBottom ? mBottom : mRows - 1);
        carriageReturn();
        break;
    case 'F':
        mCursorY = std::max(mCursorY - n, mCursorY >= mTop ? mTop : 0);
        carriageReturn();
        break;
    case 'G':
    case '`':
        mCursorX = std::clamp(n - 1, 0, mCols - 1);
        mWrapPending = false;
        break;
    case 'H':
    case 'f':
        moveTo(std::max(1, param(1, 1)) - 1, n - 1);
        break;
    case 'I':
        for (int i = 0; i < n; ++i)
            mCursorX = std::min(mCols - 1, (mCursorX / kTabWidth + 1) * kTabWidth);
        mWrapPending = false;
        break;
    case 'Z':
        for (int i = 0; i < n && mCursorX > 0; ++i)
            mCursorX = ((mCursorX - 1) / kTabWidth) * kTabWidth;
        mWrapPending = false;
        break;
    case 'J':
        switch (param(0, 0))
        {
        case 0:
            eraseCells(mCursorY, mCursorX, mCols);
            for (int y = mCursorY + 1; y < mRows; ++y)
                eraseCells(y, 0, mCols);
            break;
        case 1:
            eraseCells(mCursorY, 0, mCursorX + 1);
            for (int y = 0; y < mCursorY; ++y)
                eraseCells(y, 0, mCols);
            break;
        case 2:
            eraseDisplayToHistory();
            break;
        case 3:
            mDropped += int64_t(mScrollback.size());
            mScrollback.clear();
            break;
        default:
            break;
        }
        break;
    case 'K':
        switch (param(0, 0))
        {
        case 0:
            eraseCells(mCursorY, mCursorX, mCols);
            break;
        case 1:
            eraseCells(mCursorY, 0, mCursorX + 1);
            break;
        case 2:
            eraseCells(mCursorY, 0, mCols);
            break;
        default:
            break;
        }
        break;
    case 'L':
        if (mCursorY >= mTop && mCursorY <= mBottom)
        {
            scrollDown(mCursorY, mBottom, n);
            carriageReturn();
        }
        break;
    case 'M':
        if (mCursorY >= mTop && mCursorY <= mBottom)
        {
            // Deleted lines never go to the scrollback, even at the top of the screen.
            int limit = mScrollbackLimit;
            mScrollbackLimit = 0;
            scrollUp(mCursorY, mBottom, n);
            mScrollbackLimit = limit;
            carriageReturn();
        }
        break;
    case 'P':
    {
        Line& line = g[mCursorY];
        n = std::min(n, mCols - mCursorX);
        if (mCursorX > 0 && line[mCursorX].width == 0)
            line[mCursorX - 1] = blank();
        line.erase(line.begin() + mCursorX, line.begin() + mCursorX + n);
        line.resize(mCols, blank());
        if (line[mCursorX].width == 0)
            line[mCursorX] = blank();
        break;
    }
    case 'S':
        scrollUp(mTop, mBottom, n);
        break;
    case 'T':
        if (mParams.size() <= 1)
            scrollDown(mTop, mBottom, n);
        break;
    case 'X':
        eraseCells(mCursorY, mCursorX, mCursorX + n);
        break;
    case 'b':
        for (int i = 0; i < std::min(n, mCols * mRows); ++i)
            put(mLastChar);
        break;
    case 'c':
        if (param(0, 0) == 0)
            mResponses += "\x1b[?1;2c";
        break;
    case 'd':
        moveTo(mCursorX, n - 1);
        break;
    case 'h':
    case 'l':
        setMode(final == 'h');
        break;
    case 'm':
        sgr();
        break;
    case 'n':
        if (param(0, 0) == 5)
            mResponses += "\x1b[0n";
        else if (param(0, 0) == 6)
        {
            int y = mCursorY - (mOriginMode ? mTop : 0);
            mResponses += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(mCursorX + 1) + "R";
        }
        break;
    case 'r':
    {
        int top = std::max(1, param(0, 1)) - 1;
        int bottom = std::min(mRows, param(1, mRows) == 0 ? mRows : param(1, mRows)) - 1;
        if (top < bottom)
        {
            mTop = top;
            mBottom = bottom;
            moveTo(0, 0);
        }
        break;
    }
    case 's':
        saveCursor();
        break;
    case 'u':
        restoreCursor();
        break;
    default:
        break;
    }
}

void VtScreen::resize(int cols, int rows)
{
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (cols == mCols && rows == mRows)
        return;

    auto fitWidth = [cols](Line& line) {
        line.resize(cols);
        if (!line.empty() && line.back().width == 2)
            line.back() = Cell{};
    };

    // Keep the cursor row on screen: rows lost from the top of the main screen go to the scrollback.
    if (rows < mRows)
    {
        int push = std::max(0, mCursorY - (rows - 1));
        for (int i = 0; i < push && mScrollbackLimit > 0; ++i)
            mScrollback.push_back(mMain[i]);
        mMain.erase(mMain.begin(), mMain.begin() + push);
        mAlt.erase(mAlt.begin(), mAlt.begin() + push);
        trimScrollback();
        mMain.resize(rows);
        mAlt.resize(rows);
        mCursorY -= push;
    }
    else
    {
        mMain.resize(rows, Line(cols));
        mAlt.resize(rows, Line(cols));
    }
    for (Line& line : mMain)
        fitWidth(line);
    for (Line& line : mAlt)
        fitWidth(line);

    mCols = cols;
    mRows = rows;
    mTop = 0;
    mBottom = rows - 1;
    mCursorX = std::clamp(mCursorX, 0, cols - 1);
    mCursorY = std::clamp(mCursorY, 0, rows - 1);
    mSaved.x = std::clamp(mSaved.x, 0, cols - 1);
    mSaved.y = std::clamp(mSaved.y, 0, rows - 1);
    mWrapPending = false;
    ++mRevision;
}

std::string VtScreen::lineText(int index) const
{
    return text(index, 0, index, 1 << 30);
}

std::string VtScreen::text(int fromLine, int fromCol, int toLine, int toCol) const
{
    std::string out;
    fromLine = std::max(fromLine, -scrollbackSize());
    toLine = std::min(toLine, mRows - 1);
    for (int l = fromLine; l <= toLine; ++l)
    {
        const Line& line = historyLine(l);
        int a = l == fromLine ? std::max(0, fromCol) : 0;
        int b = l == toLine ? std::min(int(line.size()), toCol) : int(line.size());
        std::string row;
        for (int x = a; x < b; ++x)
            if (line[x].width != 0)
                appendUtf8(row, line[x].ch ? line[x].ch : U' ');
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        out += row;
        if (l != toLine)
            out += '\n';
    }
    return out;
}

std::optional<VtScreen::Match> VtScreen::find(std::string_view needle, int line, int col, bool backward, bool matchCase) const
{
    auto fold = [matchCase](char32_t c) { return !matchCase && c >= U'A' && c <= U'Z' ? char32_t(c + 32) : c; };
    std::u32string pattern;
    for (char32_t c : decodeUtf8(needle))
        if (charWidth(c) != 0)
            pattern += fold(c);
    if (pattern.empty())
        return std::nullopt;

    int first = altScreen() ? 0 : -scrollbackSize();
    int last = mRows - 1;
    std::u32string text;
    std::vector<int> columns; // text index -> cell column
    for (int l = std::clamp(line, first, last); l >= first && l <= last; l += backward ? -1 : 1)
    {
        const Line& cells = historyLine(l);
        text.clear();
        columns.clear();
        for (int x = 0; x < int(cells.size()); ++x)
            if (cells[x].width != 0)
            {
                text += fold(cells[x].ch ? cells[x].ch : U' ');
                columns.push_back(x);
            }
        std::optional<size_t> hit;
        if (backward)
        {
            for (size_t at = text.rfind(pattern); at != std::u32string::npos; at = at ? text.rfind(pattern, at - 1) : std::u32string::npos)
                if (l < line || columns[at] < col)
                {
                    hit = at;
                    break;
                }
        }
        else
        {
            for (size_t at = text.find(pattern); at != std::u32string::npos; at = text.find(pattern, at + 1))
                if (l > line || columns[at] > col)
                {
                    hit = at;
                    break;
                }
        }
        if (hit)
        {
            size_t end = *hit + pattern.size();
            int endCol = end < columns.size() ? columns[end] : int(cells.size());
            return Match{l, columns[*hit], endCol - columns[*hit]};
        }
    }
    return std::nullopt;
}
} // namespace lila::term
