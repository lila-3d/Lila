#include "ui/HotkeyBindings.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/HotkeyChord.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <QWidget>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace lila::ui::hotkeys
{
using json = nlohmann::json;

namespace
{
constexpr int kModMask = Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;

json readDefaults()
{
    std::ifstream in(paths::resourcesDir() / "defaults" / "hotkeys.json", std::ios::binary);
    if (!in)
        return json::object();
    std::ostringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false, true);
    return j.is_object() ? j : json::object();
}

json defaultValue(const json& defaults, const std::string& context, const std::string& keyText)
{
    if (!defaults.contains("contexts") || !defaults["contexts"].contains(context))
        return json();
    const json& keys = defaults["contexts"][context];
    return keys.is_object() && keys.contains(keyText) ? keys[keyText] : json();
}

std::string escapePointer(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '~')
            out += "~0";
        else if (c == '/')
            out += "~1";
        else
            out += c;
    }
    return out;
}

std::string pointerFor(const std::string& context, const std::string& keyText)
{
    return "/contexts/" + escapePointer(context) + "/" + escapePointer(keyText);
}

Slots slotsOf(const Binding& b)
{
    return Slots{b.press, b.hold, b.release, b.buttons};
}

bool isEmpty(const Binding& b)
{
    return slotsOf(b).empty();
}

/** Windows set-1 scancodes of the remappable block, back to the US key at that position. */
int usKeyForScancode(uint32_t scan)
{
    static const std::map<uint32_t, int> table = {
        {0x10, Qt::Key_Q}, {0x11, Qt::Key_W}, {0x12, Qt::Key_E}, {0x13, Qt::Key_R}, {0x14, Qt::Key_T},
        {0x15, Qt::Key_Y}, {0x16, Qt::Key_U}, {0x17, Qt::Key_I}, {0x18, Qt::Key_O}, {0x19, Qt::Key_P},
        {0x1A, Qt::Key_BracketLeft}, {0x1B, Qt::Key_BracketRight}, {0x1E, Qt::Key_A}, {0x1F, Qt::Key_S},
        {0x20, Qt::Key_D}, {0x21, Qt::Key_F}, {0x22, Qt::Key_G}, {0x23, Qt::Key_H}, {0x24, Qt::Key_J},
        {0x25, Qt::Key_K}, {0x26, Qt::Key_L}, {0x27, Qt::Key_Semicolon}, {0x28, Qt::Key_Apostrophe},
        {0x29, Qt::Key_QuoteLeft}, {0x2B, Qt::Key_Backslash}, {0x2C, Qt::Key_Z}, {0x2D, Qt::Key_X},
        {0x2E, Qt::Key_C}, {0x2F, Qt::Key_V}, {0x30, Qt::Key_B}, {0x31, Qt::Key_N}, {0x32, Qt::Key_M},
        {0x33, Qt::Key_Comma}, {0x34, Qt::Key_Period}, {0x35, Qt::Key_Slash}, {0x02, Qt::Key_1},
        {0x03, Qt::Key_2}, {0x04, Qt::Key_3}, {0x05, Qt::Key_4}, {0x06, Qt::Key_5}, {0x07, Qt::Key_6},
        {0x08, Qt::Key_7}, {0x09, Qt::Key_8}, {0x0A, Qt::Key_9}, {0x0B, Qt::Key_0}, {0x0C, Qt::Key_Minus},
        {0x0D, Qt::Key_Equal},
    };
    auto it = table.find(scan);
    return it == table.end() ? 0 : it->second;
}

bool isModifierKey(int key)
{
    switch (key)
    {
    case Qt::Key_Control: case Qt::Key_Shift: case Qt::Key_Alt: case Qt::Key_Meta: case Qt::Key_AltGr:
    case Qt::Key_CapsLock: case Qt::Key_NumLock: case Qt::Key_ScrollLock: case Qt::Key_Super_L:
    case Qt::Key_Super_R: case Qt::Key_unknown: case 0:
        return true;
    default:
        return false;
    }
}
} // namespace

std::optional<QKeyCombination> parse(const std::string& keyText)
{
    QKeySequence seq = QKeySequence::fromString(QString::fromStdString(keyText), QKeySequence::PortableText);
    if (seq.isEmpty())
        return std::nullopt;
    QKeyCombination c = seq[0];
    auto [mods, key] = normalizeShifted(int(c.keyboardModifiers()) & kModMask, int(c.key()));
    return QKeyCombination(Qt::KeyboardModifiers(mods), Qt::Key(key));
}

std::string toText(QKeyCombination chord)
{
    return QKeySequence(chord).toString(QKeySequence::PortableText).toStdString();
}

QString displayText(QKeyCombination chord)
{
    return QKeySequence(chord).toString(QKeySequence::NativeText);
}

QString keyName(int qtKey)
{
    return QKeySequence(qtKey).toString(QKeySequence::NativeText);
}

bool keysByPosition()
{
    return PrefStore::get().value<std::string>("general", kKeysFollowPref, "labels") == "positions";
}

std::optional<QKeyCombination> chordFromEvent(QKeyEvent* e)
{
    int key = e->key();
    if (isModifierKey(key))
        return std::nullopt;
    int mods = int(e->modifiers()) & kModMask;
    if (key == Qt::Key_Backtab)
    {
        key = Qt::Key_Tab;
        mods |= Qt::ShiftModifier;
    }
#ifdef _WIN32
    if (const uint32_t scan = e->nativeScanCode(); matchesByPosition(scan, keysByPosition()))
        if (int us = usKeyForScancode(scan))
            key = us;
#endif
    std::tie(mods, key) = normalizeShifted(mods, key);
    return QKeyCombination(Qt::KeyboardModifiers(mods), Qt::Key(key));
}

std::vector<std::string> contextChain(QWidget* focus)
{
    std::vector<std::string> chain;
    for (QWidget* w = focus; w; w = w->parentWidget())
    {
        std::string ctx = w->property("hotkeyContext").toString().toStdString();
        if (!ctx.empty() && std::find(chain.begin(), chain.end(), ctx) == chain.end())
            chain.push_back(ctx);
    }
    if (std::find(chain.begin(), chain.end(), "global") == chain.end())
        chain.push_back("global");
    return chain;
}

Snapshot snapshot()
{
    Snapshot s;
    json merged = PrefStore::get().merged("hotkeys");
    json defaults = readDefaults();
    std::set<std::string> contextSet;
    auto collect = [&](const json& doc) {
        if (doc.contains("contexts") && doc["contexts"].is_object())
            for (auto& [ctx, keys] : doc["contexts"].items())
                contextSet.insert(ctx);
    };
    collect(merged);
    collect(defaults);
    contextSet.insert("global");
    s.contexts.push_back("global");
    for (const std::string& c : contextSet)
        if (c != "global")
            s.contexts.push_back(c);

    for (const std::string& ctx : s.contexts)
    {
        json keys = merged.contains("contexts") && merged["contexts"].contains(ctx) ? merged["contexts"][ctx] : json();
        if (keys.is_object())
            for (auto& [keyText, value] : keys.items())
            {
                auto chord = parse(keyText);
                if (!chord || value.is_null())
                    continue;
                Binding b;
                b.context = ctx;
                b.keyText = keyText;
                b.chord = *chord;
                split(value, b);
                if (isEmpty(b))
                    continue;
                json def = defaultValue(defaults, ctx, keyText);
                b.origin = def.is_null() ? Binding::Origin::User
                           : def == value ? Binding::Origin::Default
                                          : Binding::Origin::Modified;
                s.bindings.push_back(std::move(b));
            }
        json defKeys = defaults.contains("contexts") && defaults["contexts"].contains(ctx) ? defaults["contexts"][ctx] : json();
        if (defKeys.is_object())
            for (auto& [keyText, value] : defKeys.items())
                if (!value.is_null() && (!keys.is_object() || !keys.contains(keyText) || keys[keyText].is_null()))
                    s.unbound.push_back({ctx, keyText, value});
    }
    return s;
}

std::vector<const Binding*> active(const Snapshot& s, QWidget* focus)
{
    std::vector<const Binding*> out;
    std::vector<QKeyCombination> seen;
    for (const std::string& ctx : contextChain(focus))
        for (const Binding& b : s.bindings)
            if (b.context == ctx && std::find(seen.begin(), seen.end(), b.chord) == seen.end())
            {
                seen.push_back(b.chord);
                out.push_back(&b);
            }
    return out;
}

const Binding* find(const Snapshot& s, const std::string& context, QKeyCombination chord)
{
    for (const Binding& b : s.bindings)
        if (b.context == context && b.chord == chord)
            return &b;
    return nullptr;
}

void split(const json& value, Binding& b)
{
    Slots s = splitSlots(value);
    b.press = s.press;
    b.hold = s.hold;
    b.release = s.release;
    b.buttons = s.buttons;
}

const json& slotTarget(const Binding& b, Slot slot)
{
    switch (slot)
    {
    case Slot::Press: return b.press;
    case Slot::Hold: return b.hold;
    case Slot::Release: return b.release;
    case Slot::Lmb: return b.buttons[0];
    case Slot::Mmb: return b.buttons[1];
    default: return b.buttons[2];
    }
}

std::string describe(const Binding& b)
{
    std::string out;
    auto add = [&](const std::string& prefix, const json& t) {
        if (t.is_null())
            return;
        if (!out.empty())
            out += "  /  ";
        out += prefix + actions::labelFor(t);
    };
    int count = int(!b.press.is_null()) + int(!b.hold.is_null()) + int(!b.release.is_null());
    for (const json& t : b.buttons)
        count += int(!t.is_null());
    bool single = count == 1 && !slotsOf(b).hasButtons();
    bool deferred = !b.hold.is_null() || slotsOf(b).hasButtons();
    add(single || !deferred ? "" : "tap: ", b.press);
    add(single ? "" : "hold: ", b.hold);
    add(single ? "" : "release: ", b.release);
    for (int i = 0; i < 3; ++i)
        add(std::string("+") + kButtonKeys[i] + ": ", b.buttons[i]);
    return out;
}

std::string modeText(const Binding& b)
{
    std::string buttons;
    for (int i = 0; i < 3; ++i)
        if (!b.buttons[i].is_null())
            buttons += std::string(buttons.empty() ? "" : "/") + kButtonKeys[i];
    std::string base;
    if (!b.hold.is_null())
        base = b.press.is_null() ? "hold" : "tap / hold";
    else if (!b.release.is_null())
        base = b.press.is_null() ? "release" : "press / release";
    else if (!b.press.is_null())
        base = buttons.empty() ? "press" : "tap";
    if (buttons.empty())
        return base;
    return base.empty() ? "+" + buttons : base + " / +" + buttons;
}

std::vector<std::string> actionIds(const Binding& b)
{
    std::vector<std::string> out;
    for (const json* t : {&b.press, &b.hold, &b.release, &b.buttons[0], &b.buttons[1], &b.buttons[2]})
        if (t->is_string())
            out.push_back(t->get<std::string>());
    return out;
}

bool targets(const Binding& b, const std::string& actionId)
{
    auto ids = actionIds(b);
    return std::find(ids.begin(), ids.end(), actionId) != ids.end();
}

void assign(const std::string& context, QKeyCombination chord, Slot slot, const json& target)
{
    assignAll({{context, chord, slot, target}});
}

void assignAll(const std::vector<Assignment>& list)
{
    if (list.empty())
        return;
    Snapshot s = snapshot();
    json doc = PrefStore::get().userDoc("hotkeys");
    for (const Assignment& a : list)
    {
        std::string keyText = toText(a.chord);
        Binding* existing = nullptr;
        for (Binding& b : s.bindings)
            if (b.context == a.context && b.chord == a.chord)
                existing = &b;
        if (existing)
            keyText = existing->keyText;
        else
            // Reuse a nulled default's spelling so the new value replaces the null rather than sitting beside it.
            for (const Unbound& u : s.unbound)
                if (u.context == a.context && parse(u.keyText) == a.chord)
                    keyText = u.keyText;
        Binding next;
        if (existing)
            next = *existing;
        else
        {
            next.context = a.context;
            next.keyText = keyText;
            next.chord = a.chord;
        }
        const_cast<json&>(slotTarget(next, a.slot)) = a.target;
        doc[json::json_pointer(pointerFor(a.context, keyText))] = joinSlots(slotsOf(next));
        if (existing)
            *existing = next;
        else
            s.bindings.push_back(next);
    }
    PrefStore::get().setUserDoc("hotkeys", std::move(doc));
}

void unbind(const std::string& context, const std::string& keyText)
{
    if (defaultValue(readDefaults(), context, keyText).is_null())
        PrefStore::get().remove("hotkeys", pointerFor(context, keyText));
    else
        PrefStore::get().set("hotkeys", pointerFor(context, keyText), nullptr);
}

void resetToDefault(const std::string& context, const std::string& keyText)
{
    PrefStore::get().remove("hotkeys", pointerFor(context, keyText));
}
} // namespace lila::ui::hotkeys
