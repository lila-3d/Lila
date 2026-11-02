#include "ui/AttrView.h"

#include "core/PrefStore.h"
#include "scene/Graph.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>
#include <fstream>

namespace lila::ui::attrView
{
namespace
{
constexpr const char* kFile = "attributes";
constexpr const char* kViewsFile = "attributeViews";

struct State
{
    scene::NodeId lead = scene::kNoNode;
    bool forced = false;
    bool held = false;
    bool used = false;
    std::map<int, std::function<void()>> listeners;
    int next = 1;
};
State& state()
{
    static State s;
    return s;
}

void notify()
{
    auto copy = state().listeners;
    for (auto& [id, fn] : copy)
        fn();
}

/** True when w or an ancestor is a panel that edits attributes (the property set by the AE and Channel Box). */
bool inPanel(QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (w->property("lilaMultiEditPanel").toBool())
            return true;
    return false;
}

class KeyWatch final : public QObject
{
protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() != Qt::Key_Alt || k->isAutoRepeat())
            return false;
        State& s = state();
        if (e->type() == QEvent::KeyPress)
        {
            s.used = inPanel(QApplication::widgetAt(QCursor::pos())) || inPanel(QApplication::focusWidget());
            if (!s.held)
            {
                s.held = true;
                notify();
            }
            return false;
        }
        const bool swallow = s.used;
        s.used = false;
        if (s.held)
        {
            s.held = false;
            notify();
        }
        // Eaten so Windows doesn't move focus to the menu bar after Alt was used to edit.
        return swallow;
    }
};

std::string hiddenKey(const std::string& type) { return type; }

const std::vector<View>& builtIns()
{
    static const std::vector<View> v = {
        {"Default", "lila", {}, {}},
        {"Lookdev", "lila", {}, {"Material", "Shad", "Light", "Render", "Display", "Texture", "Extra", "Parameters"}},
        {"Rigging", "lila", {}, {"Transform", "Joint", "Constraint", "Deform", "Skin", "Driver", "Extra", "Parameters", "Pose", "Ik"}},
    };
    return v;
}

View viewFromJson(const std::string& name, const json& j, const std::string& source)
{
    View v{name, source, {}, {}};
    if (j.is_object() && j.contains("hidden") && j["hidden"].is_object())
        for (const auto& [type, titles] : j["hidden"].items())
            for (const json& t : titles)
                if (t.is_string())
                    v.hidden[type].insert(t.get<std::string>());
    return v;
}

json viewToJson(const View& v)
{
    json hidden = json::object();
    for (const auto& [type, titles] : v.hidden)
        if (!titles.empty())
            hidden[type] = std::vector<std::string>(titles.begin(), titles.end());
    return {{"hidden", hidden}};
}

std::optional<View> findView(const std::string& name)
{
    for (View& v : views())
        if (v.name == name)
            return v;
    return std::nullopt;
}
} // namespace

NameMode nameMode()
{
    const std::string m = PrefStore::get().value<std::string>(kFile, "/nameMode", "nice");
    return m == "long" ? NameMode::Long : m == "short" ? NameMode::Short : NameMode::Nice;
}

void setNameMode(NameMode m)
{
    PrefStore::get().set(kFile, "/nameMode", nameModeName(m));
    notify();
}

const char* nameModeName(NameMode m)
{
    switch (m)
    {
    case NameMode::Long: return "long";
    case NameMode::Short: return "short";
    default: return "nice";
    }
}

QString niceName(const std::string& name)
{
    QString out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        if (i == 0)
            out += QChar(c).toUpper();
        else if (c == ':' || c == '_' || c == '.')
            out += ' ';
        else if (std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(name[i - 1])))
            out += QString(" ") + QChar(c);
        else if (name[i - 1] == ':' || name[i - 1] == '_' || name[i - 1] == '.')
            out += QChar(c).toUpper();
        else
            out += QChar(c);
    }
    return out;
}

QString label(const scene::AttrDef& def, int component)
{
    const bool child = component >= 0 && component < 3 && def.type == scene::PlugType::Vec3;
    const std::string longName = child ? def.childNames[size_t(component)] : def.name;
    switch (nameMode())
    {
    case NameMode::Long: return QString::fromStdString(longName);
    case NameMode::Short:
    {
        const std::string s = child ? def.childShort[size_t(component)] : def.shortName;
        return QString::fromStdString(s.empty() ? longName : s);
    }
    default:
        if (!child && !def.niceName.empty())
            return QString::fromStdString(def.niceName);
        return niceName(longName);
    }
}

scene::NodeId lead()
{
    scene::Graph& g = scene::Graph::current();
    const auto& sel = g.selection();
    if (sel.empty())
        return scene::kNoNode;
    if (state().lead != scene::kNoNode && std::find(sel.begin(), sel.end(), state().lead) != sel.end())
        return state().lead;
    return sel.back();
}

void setLead(scene::NodeId id)
{
    if (state().lead == id)
        return;
    state().lead = id;
    notify();
}

std::vector<scene::NodeId> selectionLeadFirst()
{
    std::vector<scene::NodeId> out;
    const scene::NodeId l = lead();
    if (l != scene::kNoNode)
        out.push_back(l);
    for (scene::NodeId id : scene::Graph::current().selection())
        if (id != l)
            out.push_back(id);
    return out;
}

bool multiEditHeld() { return state().forced || state().held || (QApplication::queryKeyboardModifiers() & Qt::AltModifier); }

void setMultiEditForced(bool on)
{
    if (state().forced == on)
        return;
    state().forced = on;
    notify();
}

void installKeyWatch()
{
    static KeyWatch* watch = nullptr;
    if (watch || !qApp)
        return;
    watch = new KeyWatch;
    watch->setParent(qApp);
    qApp->installEventFilter(watch);
}

void noteMultiEditUsed() { state().used = true; }

bool hintEnabled() { return PrefStore::get().value(kFile, "/multiEditHint", true); }

void setHintEnabled(bool on)
{
    PrefStore::get().set(kFile, "/multiEditHint", on);
    notify();
}

int precision() { return std::clamp(PrefStore::get().value(kFile, "/precision", 3), 0, 10); }

void setPrecision(int places)
{
    PrefStore::get().set(kFile, "/precision", std::clamp(places, 0, 10));
    notify();
}

bool showNamespaces() { return PrefStore::get().value(kFile, "/showNamespaces", false); }

void setShowNamespaces(bool on)
{
    PrefStore::get().set(kFile, "/showNamespaces", on);
    notify();
}

QString formatNumber(double v)
{
    if (std::abs(v) < 0.5 * std::pow(10.0, -precision()))
        return "0";
    QString s = QString::number(v, 'f', precision());
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

int addListener(std::function<void()> fn)
{
    const int id = state().next++;
    state().listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id) { state().listeners.erase(id); }

std::vector<View> views()
{
    std::vector<View> out = builtIns();
    const json user = PrefStore::get().userDoc(kViewsFile);
    const json merged = PrefStore::get().merged(kViewsFile);
    if (merged.is_object() && merged.contains("views") && merged["views"].is_object())
        for (const auto& [name, j] : merged["views"].items())
        {
            const bool mine = user.is_object() && user.contains("views") && user["views"].contains(name);
            View v = viewFromJson(name, j, mine ? "user" : "studio");
            auto same = std::find_if(out.begin(), out.end(), [&](const View& o) { return o.name == name; });
            if (same != out.end())
                *same = v;
            else
                out.push_back(std::move(v));
        }
    return out;
}

std::string currentView() { return PrefStore::get().value<std::string>(kFile, "/view", "Default"); }

void setCurrentView(const std::string& name)
{
    PrefStore::get().set(kFile, "/view", name);
    notify();
}

bool sectionShown(const std::string& typeName, const QString& title)
{
    std::optional<View> v = findView(currentView());
    if (!v)
        return true;
    if (auto it = v->hidden.find(hiddenKey(typeName)); it != v->hidden.end() && it->second.contains(title.toStdString()))
        return false;
    if (v->onlyContaining.empty())
        return true;
    for (const std::string& word : v->onlyContaining)
        if (title.contains(QString::fromStdString(word), Qt::CaseInsensitive))
            return true;
    return false;
}

void setSectionShown(const std::string& typeName, const QString& title, bool shown)
{
    std::optional<View> v = findView(currentView());
    View edit = v && v->source == "user" ? *v : View{"Custom", "user", v ? v->hidden : std::map<std::string, std::set<std::string>>{}, {}};
    if (shown)
        edit.hidden[hiddenKey(typeName)].erase(title.toStdString());
    else
        edit.hidden[hiddenKey(typeName)].insert(title.toStdString());
    PrefStore::get().set(kViewsFile, "/views/" + edit.name, viewToJson(edit));
    if (edit.name != currentView())
        PrefStore::get().set(kFile, "/view", edit.name);
    notify();
}

bool saveView(const std::string& name, bool studio)
{
    if (name.empty() || name.find('/') != std::string::npos || name.find('~') != std::string::npos)
        return false;
    std::optional<View> v = findView(currentView());
    View save = v ? *v : View{};
    save.name = name;
    if (!studio)
    {
        PrefStore::get().set(kViewsFile, "/views/" + name, viewToJson(save));
        PrefStore::get().set(kFile, "/view", name);
        notify();
        return true;
    }
    const std::vector<std::filesystem::path> dirs = PrefStore::get().studioDirs();
    if (dirs.empty())
        return false;
    const std::filesystem::path file = dirs.front() / (std::string(kViewsFile) + ".json");
    json doc = json::object();
    if (std::ifstream in(file); in)
        doc = json::parse(in, nullptr, false);
    if (!doc.is_object())
        doc = json::object();
    doc["views"][name] = viewToJson(save);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << doc.dump(2) << '\n';
    return bool(out);
}

Registry<std::string>& helpPages()
{
    static Registry<std::string> r;
    return r;
}

std::string helpPageFor(const scene::NodeType& type)
{
    for (const scene::NodeType* t = &type.registered(); t; t = t->base)
        if (const std::string* page = helpPages().find(t->name))
            return *page;
    // The Attributes page's heading for the type, when it has one; the page itself otherwise.
    return "attributes";
}
} // namespace lila::ui::attrView
