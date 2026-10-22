#pragma once

#include "ui/HotkeyChord.h"

#include <nlohmann/json.hpp>

#include <QObject>
#include <QPoint>
#include <QTimer>

#include <map>
#include <optional>
#include <string>

class QKeyEvent;
class QMouseEvent;
class QWidget;

namespace lila::ui
{
/**
 * App-wide hotkeys from the "hotkeys" prefs file:
 *   { "holdDelayMs": 180,
 *     "contexts": { "global": { "Ctrl+Z": "edit.undo",
 *                               "W": { "press": "tool.move", "LMB": { "markingMenu": "moveTool" } } },
 *                   "viewport": { ... } } }
 * Keys match by physical position (US layout names), so other layouts keep the same hand positions. A widget
 * opts into a context with the dynamic property "hotkeyContext"; lookup walks from the focus widget outwards, then
 * falls back to "global". A focused list or tree with the property "hotkeyPassThrough" set to a context name (see
 * passThrough) also gets that context's keys, Maya style, and swallows typed letters instead of keyboard-searching. Tap vs hold: with a hold target, releasing before holdDelayMs runs press. LMB/MMB/RMB run
 * when that button goes down while the key is held, and then the key's own targets don't run (see HeldKey).
 * Undo and redo repeat while held, on the /undo/repeatDelayMs and /undo/repeatIntervalMs "general" prefs.
 */
class Hotkeys : public QObject
{
    Q_OBJECT

public:
    static Hotkeys& get();
    void install();

    /** "Ctrl+Z" style label for the first key bound to target in any context, or "". */
    QString keyFor(const std::string& actionId) const;
    /** True when e is a key bound to actionId (as its press target) in any context. */
    bool isKeyFor(QKeyEvent* e, const std::string& actionId) const;
    /** Lets the viewport's hotkeys (Q/W/E/R, F, ...) through while view has focus, instead of keyboard search. */
    static void passThrough(QWidget* view, const char* context = "viewport");

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    struct Chord
    {
        int mods = 0;      // Qt::KeyboardModifiers
        int qtKey = 0;     // used when no scancode mapping exists
        uint32_t scan = 0; // physical key, 0 if unmapped
        bool operator<(const Chord& o) const { return std::tie(mods, qtKey, scan) < std::tie(o.mods, o.qtKey, o.scan); }
    };
    struct Binding
    {
        hotkeys::Slots targets;
        QString keyText;
    };
    struct Pending
    {
        Binding binding;
        uint32_t scan = 0;
        int qtKey = 0;
        hotkeys::HeldKey state;
    };

    Hotkeys();
    void reload();
    const Binding* lookup(QKeyEvent* e, QWidget* focus) const;
    static bool textInputFocused(QWidget* w);
    void onHoldTimeout();
    bool onMousePress(QObject* obj, QMouseEvent* e);
    void releaseAll();
    void startRepeat(QKeyEvent* e, const nlohmann::json& target);
    void stopRepeat();

    /** A held key whose action repeats on mRepeatTimer (see hotkeys::repeatsWhileHeld). */
    struct Repeat
    {
        uint32_t scan = 0;
        int qtKey = 0;
        nlohmann::json target;
    };

    std::map<std::string, std::vector<std::pair<Chord, Binding>>> mContexts;
    std::optional<Pending> mPending;
    std::vector<Pending> mHeldKeys;
    std::optional<Repeat> mRepeat;
    QTimer mHoldTimer;
    QTimer mRepeatTimer;
    int mSwallowRelease = 0; // Qt::MouseButtons whose release follows a press this filter ate
    int mHoldDelay = 180;
};
} // namespace lila::ui
