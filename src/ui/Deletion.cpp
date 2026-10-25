#include "ui/Deletion.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

namespace lila::ui::deletion
{
namespace
{
constexpr const char* kProperty = "lilaDeleteClaim";

/** Lives as a child of the claiming widget, so the claim goes with it. */
class Claim : public QObject
{
public:
    Claim(QWidget* owner, std::function<void()> remove, bool scene) : QObject(owner), remove(std::move(remove)), scene(scene) {}
    std::function<void()> remove;
    bool scene = false;
};

Claim* claimOf(QWidget* w)
{
    return static_cast<Claim*>(w->property(kProperty).value<QObject*>());
}

void setClaim(QWidget* w, std::function<void()> remove, bool scene)
{
    if (!w)
        return;
    delete claimOf(w);
    w->setProperty(kProperty, QVariant::fromValue<QObject*>(new Claim(w, std::move(remove), scene)));
}

Route walk(QWidget* from)
{
    for (QWidget* w = from; w; w = w->parentWidget())
        if (Claim* c = claimOf(w))
            return {c->scene ? Target::Scene : (c->remove ? Target::Panel : Target::Nothing), w};
    return {};
}

bool textInput(QWidget* w)
{
    return qobject_cast<QLineEdit*>(w) || qobject_cast<QTextEdit*>(w) || qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) ||
           w->property("hotkeysOff").toBool();
}
} // namespace

void claim(QWidget* w, std::function<void()> remove)
{
    setClaim(w, std::move(remove), false);
}

void claimScene(QWidget* w)
{
    setClaim(w, nullptr, true);
}

Route route(QWidget* focus, QWidget* hovered)
{
    if (focus && textInput(focus))
        return {Target::Nothing, focus};
    if (Route r = walk(focus); r.owner)
        return r;
    // A list nobody claimed has a selection of its own, which Delete must not read as the scene's.
    if (qobject_cast<QAbstractItemView*>(focus))
        return {Target::Nothing, focus};
    if (hovered && textInput(hovered))
        return {Target::Nothing, hovered};
    return walk(hovered);
}

Route run(QWidget* focus, QWidget* hovered, const std::function<void()>& scene)
{
    Route r = route(focus, hovered);
    if (r.target == Target::Scene && scene)
        scene();
    else if (r.target == Target::Panel)
        if (Claim* c = claimOf(r.owner); c && c->remove)
            c->remove();
    return r;
}
} // namespace lila::ui::deletion
