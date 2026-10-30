#include "ui/AchievementRules.h"
#include "ui/Presenter.h"

#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/Tools.h"
#include "ui/ViewportTool.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>

namespace lila::ui::presenter
{
namespace
{
using nlohmann::json;

constexpr const char* kPresenterPrefs = "general";

const QColor kPresenterBack(18, 18, 18, 215);
const QColor kPresenterText(242, 242, 242);
const QColor kPresenterDim(160, 160, 160);
const QColor kPresenterCap(58, 58, 58);
const QColor kPresenterCapLit(230, 230, 230);

std::string presenterKey(const std::string& key)
{
    return "/presenter/" + key;
}

bool presenterShows(const std::string& element, bool fallback = true)
{
    return PrefStore::get().value<bool>(kPresenterPrefs, presenterKey("show/" + element), fallback);
}

double presenterDuration()
{
    return std::clamp(PrefStore::get().value<double>(kPresenterPrefs, presenterKey("duration"), 3.5), 0.5, 30.0);
}

/** One element of the overlay and where it sits: anchor (x, y) as a fraction of the main window, f the point of the block put there. */
struct BlockDef
{
    const char* id;
    const char* title;
    double x, y, fx, fy;
    const char* element;
};

constexpr BlockDef kPresenterBlocks[] = {
    {"keys", "Keys", 0.5, 0.95, 0.5, 1.0, "keys"},
    {"actions", "Actions", 0.5, 0.87, 0.5, 1.0, "actions"},
    {"context", "Tool and mode", 0.98, 0.08, 1.0, 0.0, "context"},
    {"caption", "Caption", 0.5, 0.12, 0.5, 0.0, "caption"},
};

struct Card
{
    QString title, detail, key, tag;
    int count = 1;
    qint64 at = 0;
};

struct Chord
{
    QStringList caps;
    QString label;
    qint64 at = 0;
};

class Block;
class CursorRing;
class LayoutCard;

struct State
{
    QPointer<QWidget> main;
    std::vector<Block*> blocks;
    QPointer<CursorRing> ring;
    QPointer<LayoutCard> card;
    QTimer* tick = nullptr;
    QObject* filter = nullptr;
    QElapsedTimer clock;
    bool editing = false;
    std::deque<Card> cards;
    Chord chord;
    int heldMods = 0;
    int heldButtons = 0;
    QString nav;
    QString caption;
    qint64 captionAt = 0;
    QString context;
    int actionDepth = 0;
    int runListener = 0;
    int executeListener = 0;
    ulong lastKeyTime = 0;
    ulong lastMouseTime = 0;
};

State& state()
{
    static State s;
    return s;
}

qint64 now()
{
    State& s = state();
    if (!s.clock.isValid())
        s.clock.start();
    return s.clock.elapsed();
}

/** 1 while fresh, easing to 0 over the last 600 ms of its time. */
double fadeFor(qint64 at, double seconds)
{
    const double left = seconds * 1000.0 - double(now() - at);
    return std::clamp(left / 600.0, 0.0, 1.0);
}

QString capitalised(QString s)
{
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

/** "moveComponents" reads "Move Components". */
QString humanised(const std::string& name)
{
    QString out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        if (i > 0 && std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(name[i - 1])))
            out += ' ';
        out += QChar(c);
    }
    return capitalised(out);
}

QString compactValue(const json& v)
{
    if (v.is_number_float())
    {
        QString s = QString::number(v.get<double>(), 'f', 2);
        while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
            s.chop(1);
        return s == "-0" ? "0" : s;
    }
    if (v.is_number())
        return QString::number(v.get<long long>());
    if (v.is_boolean())
        return v.get<bool>() ? "on" : "off";
    if (v.is_string())
        return QString::fromStdString(v.get<std::string>());
    if (v.is_array())
    {
        QStringList parts;
        for (const json& e : v)
        {
            if (parts.size() == 4)
            {
                parts << "...";
                break;
            }
            parts << compactValue(e);
        }
        return parts.join(", ");
    }
    return "...";
}

/** The objects a call names, then its flags: "pCube1  by 12, 0, 0". */
QString callSummary(const json& call)
{
    QStringList parts;
    if (const auto it = call.find("args"); it != call.end() && it->is_array() && !it->empty())
        parts << (it->size() == 1 ? compactValue((*it)[0]) : QString("%1 objects").arg(it->size()));
    if (call.is_object())
        for (const auto& [k, v] : call.items())
            if (k != "args" && parts.size() < 4)
                parts << QString::fromStdString(k) + " " + compactValue(v);
    QString s = parts.join("   ");
    return s.size() > 80 ? s.left(77) + "..." : s;
}

QStringList capsOf(const QString& keyText)
{
    if (keyText.isEmpty())
        return {};
    QStringList caps = keyText.split('+', Qt::SkipEmptyParts);
    if (keyText.endsWith("++"))
        caps << "+";
    return caps;
}

QString modifierName(int mod)
{
    switch (mod)
    {
    case Qt::ControlModifier: return "Ctrl";
    case Qt::ShiftModifier: return "Shift";
    case Qt::AltModifier: return "Alt";
    case Qt::MetaModifier: return "Win";
    default: return {};
    }
}

QStringList modifierCaps(int mods)
{
    QStringList caps;
    for (int m : {int(Qt::ControlModifier), int(Qt::ShiftModifier), int(Qt::AltModifier), int(Qt::MetaModifier)})
        if (mods & m)
            caps << modifierName(m);
    return caps;
}

QStringList buttonCaps(int buttons)
{
    QStringList caps;
    if (buttons & Qt::LeftButton)
        caps << "LMB";
    if (buttons & Qt::MiddleButton)
        caps << "MMB";
    if (buttons & Qt::RightButton)
        caps << "RMB";
    return caps;
}

/** The key's name by position for letters, digits and the backtick, so Shift+2 reads as Shift 2, not @. */
QString keyName(const QKeyEvent* e)
{
    const quint32 vk = e->nativeVirtualKey();
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return QString(QChar(char(vk)));
    if (e->nativeScanCode() == 0x29)
        return "`";
    switch (e->key())
    {
    case Qt::Key_Return:
    case Qt::Key_Enter: return "Enter";
    case Qt::Key_Escape: return "Esc";
    case Qt::Key_Backspace: return "Backspace";
    default: break;
    }
    return QKeySequence(e->key()).toString(QKeySequence::NativeText);
}

bool isModifierKey(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
}

/** Typing into a field stays private; only chords with Ctrl, Alt or Win show from there. */
bool typingInField(int mods)
{
    if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;
    QWidget* f = QApplication::focusWidget();
    if (!f)
        return false;
    if (qobject_cast<QLineEdit*>(f) || qobject_cast<QTextEdit*>(f) || qobject_cast<QPlainTextEdit*>(f) || qobject_cast<QAbstractSpinBox*>(f))
        return true;
    if (auto* c = qobject_cast<QComboBox*>(f); c && c->isEditable())
        return true;
    return QString(f->metaObject()->className()).contains("WebEngine") || QString(f->metaObject()->className()).contains("RenderWidgetHost");
}

bool overViewport(QObject* receiver)
{
    for (auto* w = qobject_cast<QWidget*>(receiver); w; w = w->parentWidget())
        if (w->inherits("lila::ui::ViewportWidget"))
            return true;
    return false;
}

QRect mainRect()
{
    QWidget* m = state().main;
    return m ? QRect(m->mapToGlobal(QPoint(0, 0)), m->size()) : QRect();
}

bool mainShowing()
{
    QWidget* m = state().main;
    return m && m->isVisible() && !m->isMinimized();
}

QFont presenterFont(double points, bool bold = false)
{
    QFont f = QApplication::font();
    f.setPointSizeF(points);
    f.setBold(bold);
    return f;
}

/** A row of keycaps; the width it took. */
double drawCaps(QPainter& p, QPointF at, const QStringList& caps, double s, bool lit, double opacity, bool measureOnly = false)
{
    const QFont f = presenterFont(10.5 * s, true);
    const QFontMetricsF fm(f);
    const double h = 24.0 * s, pad = 8.0 * s, gap = 5.0 * s;
    double x = 0.0;
    for (const QString& cap : caps)
    {
        const double w = std::max(h, fm.horizontalAdvance(cap) + pad * 2.0);
        if (!measureOnly)
        {
            QRectF r(at.x() + x, at.y(), w, h);
            p.setOpacity(opacity);
            p.setPen(Qt::NoPen);
            p.setBrush(lit ? kPresenterCapLit : kPresenterCap);
            p.drawRoundedRect(r, 5.0 * s, 5.0 * s);
            p.setPen(lit ? QColor(20, 20, 20) : kPresenterText);
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, cap);
        }
        x += w + gap;
    }
    return caps.isEmpty() ? 0.0 : x - gap;
}

void relayoutAll();
void refreshBlocks();

/**
 * One movable piece of the overlay, its own small window over the main one so the rest of Lila keeps the mouse. While
 * the layout is edited it takes the mouse: drag to move, scroll to scale, right-click for its menu.
 */
class Block : public QWidget
{
public:
    Block(QWidget* owner, const BlockDef& def)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus |
                             Qt::WindowTransparentForInput),
          mDef(def)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
    }

    const BlockDef& def() const { return mDef; }

    double scale() const
    {
        const double global = PrefStore::get().value<double>(kPresenterPrefs, presenterKey("scale"), 1.0);
        return std::clamp(global * pref("scale", 1.0), 0.3, 4.0);
    }

    void setEditable(bool on)
    {
        if (windowFlags().testFlag(Qt::WindowTransparentForInput) == !on)
            return;
        const bool shown = isVisible();
        setWindowFlag(Qt::WindowTransparentForInput, !on);
        setAttribute(Qt::WA_TransparentForMouseEvents, !on);
        if (shown)
            show();
    }

    /** Sizes and places the block from its content, and shows it when it has any. */
    void relayout()
    {
        State& st = state();
        const bool on = enabled() && presenterShows(mDef.element) && mainShowing();
        const bool content = hasContent();
        if (!on || (!content && !st.editing))
        {
            hide();
            return;
        }
        const double s = scale();
        QSize size = contentSize(s, !content).grownBy(QMargins(0, editBand(), 0, 0));
        size = size.expandedTo(QSize(40, 20));
        const QRect m = mainRect();
        const double ax = pref("x", mDef.x), ay = pref("y", mDef.y), fx = pref("fx", mDef.fx), fy = pref("fy", mDef.fy);
        QPoint pos(int(std::lround(m.x() + ax * m.width() - fx * size.width())), int(std::lround(m.y() + ay * m.height() - fy * size.height())));
        if (geometry() != QRect(pos, size))
            setGeometry(QRect(pos, size));
        if (!isVisible())
            show();
        update();
    }

protected:
    virtual bool hasContent() const = 0;
    virtual QSize contentSize(double s, bool placeholder) const = 0;
    virtual void paintContent(QPainter& p, double s, bool placeholder) = 0;

    double pref(const char* key, double fallback) const
    {
        return PrefStore::get().value<double>(kPresenterPrefs, presenterKey(std::string("blocks/") + mDef.id + "/" + key), fallback);
    }
    void setPref(const char* key, double v) const
    {
        PrefStore::get().set(kPresenterPrefs, presenterKey(std::string("blocks/") + mDef.id + "/" + key), v);
    }

    int editBand() const { return state().editing ? 18 : 0; }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool placeholder = !hasContent();
        if (state().editing)
        {
            p.setPen(QPen(kPresenterCapLit, 1.0, Qt::DashLine));
            p.setBrush(QColor(0, 0, 0, 60));
            p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            p.setPen(kPresenterText);
            p.setFont(presenterFont(8.5));
            p.drawText(QRectF(6, 1, width() - 12, editBand()), Qt::AlignVCenter | Qt::AlignLeft, mDef.title);
        }
        p.translate(0, editBand());
        paintContent(p, scale(), placeholder);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton)
            mDragOffset = e->globalPosition().toPoint() - pos();
        else if (e->button() == Qt::RightButton)
            showMenu(e->globalPosition().toPoint());
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || mDragOffset.isNull())
            return;
        move(e->globalPosition().toPoint() - mDragOffset);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || mDragOffset.isNull())
            return;
        mDragOffset = {};
        storePlace();
    }

    void wheelEvent(QWheelEvent* e) override
    {
        const double factor = std::pow(1.1, e->angleDelta().y() / 120.0);
        setPref("scale", std::clamp(pref("scale", 1.0) * factor, 0.4, 3.0));
        relayout();
    }

private:
    /** The block grows away from the nearest edge, so a block at the bottom keeps its bottom where it was put. */
    void storePlace()
    {
        const QRect m = mainRect();
        if (m.isEmpty())
            return;
        const QPointF c = QRectF(geometry()).center();
        auto side = [](double t) { return t < 0.34 ? 0.0 : t > 0.66 ? 1.0 : 0.5; };
        const double fx = side((c.x() - m.x()) / m.width()), fy = side((c.y() - m.y()) / m.height());
        setPref("fx", fx);
        setPref("fy", fy);
        setPref("x", (x() + fx * width() - m.x()) / double(m.width()));
        setPref("y", (y() + fy * height() - m.y()) / double(m.height()));
    }

    void showMenu(QPoint at)
    {
        QMenu menu;
        menu.addAction(QString("Hide %1").arg(mDef.title), [this] {
            PrefStore::get().set(kPresenterPrefs, presenterKey(std::string("show/") + mDef.element), false);
        });
        menu.addAction("Larger", [this] { setPref("scale", std::min(3.0, pref("scale", 1.0) * 1.2)); relayout(); });
        menu.addAction("Smaller", [this] { setPref("scale", std::max(0.4, pref("scale", 1.0) / 1.2)); relayout(); });
        menu.addAction("Reset Place and Size", [this] {
            for (const char* k : {"x", "y", "fx", "fy", "scale"})
                PrefStore::get().set(kPresenterPrefs, presenterKey(std::string("blocks/") + mDef.id + "/" + k), nullptr);
            relayout();
        });
        menu.exec(at);
    }

    BlockDef mDef;
    QPoint mDragOffset;
};

class KeysBlock : public Block
{
public:
    using Block::Block;

protected:
    struct Row
    {
        QStringList caps;
        QString label;
        bool lit = false;
        double opacity = 1.0;
    };

    Row row(bool placeholder) const
    {
        State& st = state();
        if (placeholder)
            return {{"Ctrl", "B"}, {}, false, 1.0};
        if (!st.nav.isEmpty() && presenterShows("navigation"))
            return {modifierCaps(st.heldMods) + buttonCaps(st.heldButtons), st.nav, true, 1.0};
        if (st.heldButtons)
            return {modifierCaps(st.heldMods) + buttonCaps(st.heldButtons), {}, true, 1.0};
        const double fade = fadeFor(st.chord.at, presenterDuration());
        if (st.heldMods && (fade <= 0.0 || !st.chord.caps.startsWith(modifierCaps(st.heldMods).value(0))))
            return {modifierCaps(st.heldMods), {}, true, 1.0};
        if (fade > 0.0 && !st.chord.caps.isEmpty())
            return {st.chord.caps, st.chord.label, false, fade};
        return {};
    }

    bool hasContent() const override { return !row(false).caps.isEmpty(); }

    QSize contentSize(double s, bool placeholder) const override
    {
        const Row r = row(placeholder);
        QImage scratch(1, 1, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&scratch);
        double w = drawCaps(p, {}, r.caps, s, r.lit, 1.0, true);
        if (!r.label.isEmpty())
            w += 10.0 * s + QFontMetricsF(presenterFont(11.0 * s, true)).horizontalAdvance(r.label);
        return QSize(int(std::ceil(w + 20.0 * s)), int(std::ceil(36.0 * s)));
    }

    void paintContent(QPainter& p, double s, bool placeholder) override
    {
        const Row r = row(placeholder);
        const QSizeF box(width(), 36.0 * s);
        p.setOpacity(r.opacity);
        p.setPen(Qt::NoPen);
        p.setBrush(kPresenterBack);
        p.drawRoundedRect(QRectF(QPointF(), box), 8.0 * s, 8.0 * s);
        const double w = drawCaps(p, QPointF(10.0 * s, 6.0 * s), r.caps, s, r.lit, r.opacity);
        if (!r.label.isEmpty())
        {
            p.setOpacity(r.opacity);
            p.setPen(kPresenterText);
            p.setFont(presenterFont(11.0 * s, true));
            p.drawText(QRectF(10.0 * s + w + 10.0 * s, 0, box.width(), box.height()), Qt::AlignVCenter | Qt::AlignLeft, r.label);
        }
        p.setOpacity(1.0);
    }
};

class ActionsBlock : public Block
{
public:
    using Block::Block;

protected:
    static constexpr double kCardH = 46.0, kCardGap = 6.0, kCardW = 340.0;

    std::vector<Card> shown(bool placeholder) const
    {
        if (placeholder)
            return {Card{"Bevel", "Edit Mesh > Bevel", "Ctrl+B", {}, 1, now()}};
        return {state().cards.begin(), state().cards.end()};
    }

    bool hasContent() const override { return !state().cards.empty(); }

    double cardWidth(const Card& c, double s) const
    {
        QImage scratch(1, 1, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&scratch);
        const QString title = c.count > 1 ? c.title + QString("  x%1").arg(c.count) : c.title;
        const double text = std::max(QFontMetricsF(presenterFont(12.5 * s, true)).horizontalAdvance(title + "    " + c.tag),
                                     QFontMetricsF(presenterFont(9.5 * s)).horizontalAdvance(c.detail));
        return std::max(kCardW * 0.6 * s, 24.0 * s + text + 14.0 * s + drawCaps(p, {}, capsOf(c.key), s, false, 1.0, true));
    }

    QSize contentSize(double s, bool placeholder) const override
    {
        const std::vector<Card> cs = shown(placeholder);
        double w = 0.0;
        for (const Card& c : cs)
            w = std::max(w, cardWidth(c, s));
        const double h = cs.size() * (kCardH + kCardGap) * s - kCardGap * s;
        return QSize(int(std::ceil(w)), int(std::ceil(std::max(h, 1.0))));
    }

    void paintContent(QPainter& p, double s, bool placeholder) override
    {
        const std::vector<Card> cs = shown(placeholder);
        const double w = width();
        double y = 0.0;
        for (const Card& c : cs)
        {
            const double fade = placeholder ? 1.0 : fadeFor(c.at, presenterDuration());
            const QRectF r(0, y, w, kCardH * s);
            p.setOpacity(fade);
            p.setPen(Qt::NoPen);
            p.setBrush(kPresenterBack);
            p.drawRoundedRect(r, 8.0 * s, 8.0 * s);
            const QStringList caps = capsOf(c.key);
            QImage scratch(1, 1, QImage::Format_ARGB32_Premultiplied);
            QPainter measure(&scratch);
            const double capsW = drawCaps(measure, {}, caps, s, false, 1.0, true);
            drawCaps(p, QPointF(r.right() - 12.0 * s - capsW, r.center().y() - 12.0 * s), caps, s, false, fade);
            p.setOpacity(fade);
            const QRectF text = r.adjusted(12.0 * s, 5.0 * s, -(capsW + 24.0 * s), -5.0 * s);
            QString title = c.title;
            if (c.count > 1)
                title += QString("  x%1").arg(c.count);
            p.setFont(presenterFont(12.5 * s, true));
            p.setPen(kPresenterText);
            const QRectF titleRect(text.left(), text.top(), text.width(), text.height() * (c.detail.isEmpty() ? 1.0 : 0.55));
            p.drawText(titleRect, Qt::AlignLeft | (c.detail.isEmpty() ? Qt::AlignVCenter : Qt::AlignBottom), title);
            if (!c.tag.isEmpty())
            {
                const double tw = QFontMetricsF(p.font()).horizontalAdvance(title + "  ");
                p.setFont(presenterFont(8.5 * s, true));
                p.setPen(kPresenterDim);
                p.drawText(titleRect.adjusted(tw, 0, 0, 0), Qt::AlignLeft | (c.detail.isEmpty() ? Qt::AlignVCenter : Qt::AlignBottom),
                           c.tag.toUpper());
            }
            if (!c.detail.isEmpty())
            {
                p.setFont(presenterFont(9.5 * s));
                p.setPen(kPresenterDim);
                p.drawText(QRectF(text.left(), titleRect.bottom() + 2.0 * s, text.width(), text.height() * 0.45), Qt::AlignLeft | Qt::AlignTop,
                           QFontMetricsF(p.font()).elidedText(c.detail, Qt::ElideRight, text.width()));
            }
            y += (kCardH + kCardGap) * s;
        }
        p.setOpacity(1.0);
    }
};

class ContextBlock : public Block
{
public:
    using Block::Block;

protected:
    bool hasContent() const override { return !state().context.isEmpty(); }

    QSize contentSize(double s, bool) const override
    {
        const double w = QFontMetricsF(presenterFont(10.5 * s, true)).horizontalAdvance(state().context);
        return QSize(int(std::ceil(w + 24.0 * s)), int(std::ceil(30.0 * s)));
    }

    void paintContent(QPainter& p, double s, bool) override
    {
        const QRectF r(0, 0, width(), 30.0 * s);
        p.setPen(Qt::NoPen);
        p.setBrush(kPresenterBack);
        p.drawRoundedRect(r, 7.0 * s, 7.0 * s);
        p.setPen(kPresenterText);
        p.setFont(presenterFont(10.5 * s, true));
        p.drawText(r, Qt::AlignCenter, state().context);
    }
};

class CaptionBlock : public Block
{
public:
    using Block::Block;

protected:
    double fade() const { return fadeFor(state().captionAt, std::max(6.0, presenterDuration() * 2.0)); }
    QString text(bool placeholder) const { return placeholder ? QString("Caption") : state().caption; }

    bool hasContent() const override { return !state().caption.isEmpty() && fade() > 0.0; }

    QSize contentSize(double s, bool placeholder) const override
    {
        const QFontMetricsF fm(presenterFont(20.0 * s, true));
        const double maxW = std::max(200.0, mainRect().width() * 0.6);
        const QRectF b = fm.boundingRect(QRectF(0, 0, maxW, 1000), Qt::TextWordWrap | Qt::AlignCenter, text(placeholder));
        return QSize(int(std::ceil(b.width() + 40.0 * s)), int(std::ceil(b.height() + 22.0 * s)));
    }

    void paintContent(QPainter& p, double s, bool placeholder) override
    {
        p.setOpacity(placeholder ? 1.0 : fade());
        const QRectF r(0, 0, width(), height() - editBand());
        p.setPen(Qt::NoPen);
        p.setBrush(kPresenterBack);
        p.drawRoundedRect(r, 10.0 * s, 10.0 * s);
        p.setPen(kPresenterText);
        p.setFont(presenterFont(20.0 * s, true));
        p.drawText(r.adjusted(20.0 * s, 11.0 * s, -20.0 * s, -11.0 * s), Qt::TextWordWrap | Qt::AlignCenter, text(placeholder));
        p.setOpacity(1.0);
    }
};

/** A ring that follows the pointer, filled while a button is down, with a ripple on each click. */
class CursorRing : public QWidget
{
public:
    explicit CursorRing(QWidget* owner)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus |
                             Qt::WindowTransparentForInput | Qt::WindowStaysOnTopHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        resize(kSize, kSize);
    }

    void click() { mClickAt = now(); }

    void follow()
    {
        const bool on = enabled() && presenterShows("cursor") && QApplication::activeWindow() && !state().editing;
        if (!on)
        {
            hide();
            return;
        }
        const QPoint c = QCursor::pos() - QPoint(kSize / 2, kSize / 2);
        if (pos() != c)
            move(c);
        if (!isVisible())
            show();
        const int buttons = state().heldButtons;
        const bool rippling = now() - mClickAt < kRippleMs;
        if (buttons != mPainted || rippling || mWasRippling)
            update();
        mWasRippling = rippling;
    }

protected:
    static constexpr int kSize = 96;
    static constexpr qint64 kRippleMs = 450;

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c(kSize / 2.0, kSize / 2.0);
        mPainted = state().heldButtons;
        const double t = double(now() - mClickAt) / kRippleMs;
        if (t >= 0.0 && t < 1.0)
        {
            p.setPen(QPen(QColor(255, 255, 255, int(200 * (1.0 - t))), 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 16.0 + 28.0 * t, 16.0 + 28.0 * t);
        }
        p.setPen(QPen(QColor(0, 0, 0, 120), 4.0));
        p.setBrush(mPainted ? QColor(255, 255, 255, 70) : QColor(0, 0, 0, 0));
        p.drawEllipse(c, 16.0, 16.0);
        p.setPen(QPen(QColor(255, 255, 255, 220), 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 16.0, 16.0);
    }

private:
    qint64 mClickAt = -100000;
    int mPainted = 0;
    bool mWasRippling = false;
};

/** The settings shown while the layout is edited: which elements show, how long, and a reset. */
class LayoutCard : public QFrame
{
public:
    explicit LayoutCard(QWidget* owner) : QFrame(owner, Qt::Tool | Qt::FramelessWindowHint)
    {
        setObjectName("presenterLayoutCard");
        setFrameShape(QFrame::StyledPanel);
        setStyleSheet("#presenterLayoutCard { border: 1px solid palette(mid); border-radius: 6px; }");
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 10, 12, 10);
        auto* title = new QLabel("<b>Presenter Layout</b>", this);
        v->addWidget(title);
        auto* hint = new QLabel("Drag a block to move it and scroll over it to scale it. Right-click a block for more.", this);
        hint->setWordWrap(true);
        v->addWidget(hint);
        const std::pair<const char*, const char*> toggles[] = {
            {"keys", "Keys and mouse buttons"},
            {"actions", "Actions, with their menu path and hotkey"},
            {"commands", "Edits from drags and panels"},
            {"agent", "Commands an agent runs"},
            {"context", "Tool and mode"},
            {"caption", "Captions"},
            {"cursor", "Cursor highlight"},
            {"navigation", "Camera navigation"},
            {"selection", "Selection clicks"},
        };
        for (auto [key, label] : toggles)
        {
            auto* box = new QCheckBox(label, this);
            box->setChecked(presenterShows(key, std::string(key) != "selection"));
            connect(box, &QCheckBox::toggled, this, [key](bool on) { PrefStore::get().set(kPresenterPrefs, presenterKey(std::string("show/") + key), on); });
            mBoxes.emplace_back(box, key);
            v->addWidget(box);
        }
        auto* timeRow = new QHBoxLayout;
        timeRow->addWidget(new QLabel("Seconds shown", this));
        auto* seconds = new QSpinBox(this);
        seconds->setRange(1, 30);
        seconds->setValue(int(std::lround(presenterDuration())));
        connect(seconds, &QSpinBox::valueChanged, this, [](int v) { PrefStore::get().set(kPresenterPrefs, presenterKey("duration"), double(v)); });
        timeRow->addWidget(seconds);
        timeRow->addSpacing(12);
        timeRow->addWidget(new QLabel("Cards", this));
        auto* count = new QSpinBox(this);
        count->setRange(1, 10);
        count->setValue(PrefStore::get().value<int>(kPresenterPrefs, presenterKey("maxCards"), 4));
        connect(count, &QSpinBox::valueChanged, this, [](int v) { PrefStore::get().set(kPresenterPrefs, presenterKey("maxCards"), v); });
        timeRow->addWidget(count);
        timeRow->addStretch();
        v->addLayout(timeRow);
        auto* buttons = new QHBoxLayout;
        auto* reset = new QPushButton("Reset Layout", this);
        connect(reset, &QPushButton::clicked, this, [] {
            for (const BlockDef& d : kPresenterBlocks)
                for (const char* k : {"x", "y", "fx", "fy", "scale"})
                    PrefStore::get().set(kPresenterPrefs, presenterKey(std::string("blocks/") + d.id + "/" + k), nullptr);
            relayoutAll();
        });
        auto* done = new QPushButton("Done", this);
        connect(done, &QPushButton::clicked, this, [] { setEditing(false); });
        buttons->addWidget(reset);
        buttons->addStretch();
        buttons->addWidget(done);
        v->addLayout(buttons);
        adjustSize();
    }

    void sync()
    {
        for (auto& [box, key] : mBoxes)
        {
            QSignalBlocker block(box);
            box->setChecked(presenterShows(key, std::string(key) != "selection"));
        }
    }

protected:
    void mousePressEvent(QMouseEvent* e) override { mDrag = e->globalPosition().toPoint() - pos(); }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
            move(e->globalPosition().toPoint() - mDrag);
    }

private:
    std::vector<std::pair<QCheckBox*, const char*>> mBoxes;
    QPoint mDrag;
};

void relayoutAll()
{
    for (Block* b : state().blocks)
        b->relayout();
}

void refreshBlocks()
{
    relayoutAll();
}

QString contextText()
{
    QString tool;
    if (const std::string& id = viewportTools::activeId(); !id.empty())
    {
        if (const viewportTools::ToolDef* d = viewportTools::registry().find(id))
            tool = d->label;
        else
            tool = humanised(id);
    }
    else
        tool = capitalised(QString::fromUtf8(tools::name(tools::current()))) + " Tool";
    QStringList parts{tool};
    const scene::ComponentMode mode = scene::components::mode();
    if (mode != scene::ComponentMode::Object)
        parts << capitalised(QString::fromUtf8(scene::components::modeName(mode)));
    if (PrefStore::get().value<bool>("general", "/softSelect/enabled", false))
        parts << "Soft Select";
    QStringList snaps;
    const unsigned held = tools::snaps();
    if (held & SnapGrid)
        snaps << "grid";
    if (held & SnapEdge)
        snaps << "edge";
    if (held & SnapVertex)
        snaps << "vertex";
    if (held & SnapSurface)
        snaps << "surface";
    if (tools::current() != Tool::Select && tools::incrementSnap(tools::current()).enabled)
        snaps << "increment";
    if (!snaps.isEmpty())
        parts << "Snap " + snaps.join(", ");
    return parts.join("   ");
}

void pushCard(Card c)
{
    State& st = state();
    if (!presenterShows("actions"))
        return;
    c.at = now();
    if (!st.cards.empty() && st.cards.back().title == c.title && st.cards.back().tag == c.tag && fadeFor(st.cards.back().at, presenterDuration()) > 0.0)
    {
        Card& last = st.cards.back();
        last.count += 1;
        last.at = c.at;
        last.detail = c.detail;
        last.key = c.key;
    }
    else
        st.cards.push_back(std::move(c));
    const size_t limit = size_t(std::clamp(PrefStore::get().value<int>(kPresenterPrefs, presenterKey("maxCards"), 4), 1, 10));
    while (st.cards.size() > limit)
        st.cards.pop_front();
    refreshBlocks();
}

QString viaTag(const actions::Trigger& t)
{
    switch (t.via)
    {
    case actions::Via::MarkingMenu: return "marking menu";
    case actions::Via::Palette: return "palette";
    case actions::Via::Shelf: return "shelf";
    default: return {};
    }
}

bool ownAction(const std::string& id)
{
    return id.rfind("view.presenter", 0) == 0 || id.rfind("hotbox.", 0) == 0;
}

void onRun(const actions::RunEvent& e)
{
    State& st = state();
    if (!e.done)
    {
        ++st.actionDepth;
        if (!enabled())
            return;
        Card c;
        c.tag = ops::agentCalling() ? QString("agent") : viaTag(e.trigger);
        if (e.target.is_string())
        {
            const std::string id = e.target.get<std::string>();
            if (ownAction(id))
                return;
            c.title = QString::fromStdString(actions::labelFor(e.target));
            c.detail = QString::fromStdString(e.trigger.path);
            c.key = Hotkeys::get().keyFor(id);
            if (e.trigger.via == actions::Via::Key && fadeFor(st.chord.at, 1.0) > 0.0)
                c.key = st.chord.caps.join('+');
            if (id == "edit.undo")
                c.detail = QString::fromStdString(UndoStack::get().undoLabel());
            else if (id == "edit.redo")
                c.detail = QString::fromStdString(UndoStack::get().redoLabel());
        }
        else if (e.target.is_object() && e.target.contains("python"))
        {
            c.title = "Python";
            c.detail = QString::fromStdString(e.target["python"].get<std::string>()).section('\n', 0, 0).trimmed();
        }
        else if (e.target.is_object() && e.target.contains("command"))
        {
            c.title = humanised(e.target["command"].get<std::string>());
            c.detail = callSummary(e.target.value("args", json::object()));
        }
        else if (e.target.is_object() && e.target.contains("optionBox"))
            c.title = QString::fromStdString(actions::labelFor(e.target["optionBox"])) + " Options";
        else
            return;
        if (!c.title.isEmpty())
            pushCard(std::move(c));
        return;
    }
    st.actionDepth = std::max(0, st.actionDepth - 1);
}

bool selectionCommand(std::string_view name)
{
    return name.starts_with("select") || name.starts_with("pickWalk") || name.starts_with("hilite") || name == "deselect";
}

void onExecute(const ExecuteEvent& e)
{
    State& st = state();
    if (!e.done || !e.ok || st.actionDepth > 0 || !enabled())
        return;
    const bool agent = ops::agentCalling();
    if (e.source != CallSource::UI && !agent)
        return;
    if (!presenterShows(agent ? "agent" : "commands"))
        return;
    if (const CommandDef* d = ops::find(e.name); d && !d->undoable)
        return;
    if (selectionCommand(e.name) && !presenterShows("selection", false))
        return;
    Card c;
    c.title = humanised(std::string(e.name));
    c.detail = callSummary(e.call);
    if (agent)
        c.tag = "agent";
    pushCard(std::move(c));
}

/** Watches input app-wide without taking any of it. */
class InputWatch : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        State& st = state();
        switch (e->type())
        {
        case QEvent::KeyPress:
        {
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->isAutoRepeat() || k->timestamp() == st.lastKeyTime || isModifierKey(k->key()) || !presenterShows("keys"))
                break;
            st.lastKeyTime = k->timestamp();
            const int mods = int(k->modifiers()) & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
            if (typingInField(mods))
                break;
            st.chord = {modifierCaps(mods) << keyName(k), {}, now()};
            refreshBlocks();
            break;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            if (!obj->isWidgetType() || m->timestamp() == st.lastMouseTime)
                break;
            st.lastMouseTime = m->timestamp();
            if (st.ring)
                st.ring->click();
            if ((m->modifiers() & Qt::AltModifier) && overViewport(obj))
                st.nav = m->button() == Qt::LeftButton ? "Orbit" : m->button() == Qt::MiddleButton ? "Pan" : m->button() == Qt::RightButton ? "Dolly" : QString();
            break;
        }
        case QEvent::Wheel:
        {
            auto* w = static_cast<QWheelEvent*>(e);
            if (obj->isWidgetType() && overViewport(obj) && presenterShows("navigation") && w->timestamp() != st.lastMouseTime)
            {
                st.lastMouseTime = w->timestamp();
                st.chord = {{"Scroll"}, "Zoom", now()};
                refreshBlocks();
            }
            break;
        }
        default: break;
        }
        return false;
    }
};

/** Keeps held keys and buttons, fades and the cursor ring current; runs only while presenting. */
void onTick()
{
    State& st = state();
    const bool active = QApplication::activeWindow() != nullptr;
    const int mods = active ? int(QGuiApplication::queryKeyboardModifiers()) & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier) : 0;
    const int buttons = active ? int(QGuiApplication::mouseButtons()) : 0;
    bool changed = mods != st.heldMods || buttons != st.heldButtons;
    st.heldMods = mods;
    st.heldButtons = buttons;
    if (!buttons && !st.nav.isEmpty())
    {
        st.nav.clear();
        changed = true;
    }
    const QString context = presenterShows("context") ? contextText() : QString();
    changed |= context != st.context;
    st.context = context;
    const double seconds = presenterDuration();
    while (!st.cards.empty() && fadeFor(st.cards.front().at, seconds) <= 0.0)
    {
        st.cards.pop_front();
        changed = true;
    }
    const bool fading = std::ranges::any_of(st.cards, [&](const Card& c) { return fadeFor(c.at, seconds) < 1.0; }) ||
                        (fadeFor(st.chord.at, seconds) > 0.0 && fadeFor(st.chord.at, seconds) < 1.0) ||
                        (!st.caption.isEmpty() && fadeFor(st.captionAt, std::max(6.0, seconds * 2.0)) < 1.0);
    if (!st.caption.isEmpty() && fadeFor(st.captionAt, std::max(6.0, seconds * 2.0)) <= 0.0)
    {
        st.caption.clear();
        changed = true;
    }
    if (changed || fading)
        relayoutAll();
    if (st.ring)
        st.ring->follow();
}

void applyEnabled()
{
    State& st = state();
    const bool on = enabled() && st.main;
    if (on && !st.filter)
    {
        st.filter = new InputWatch(qApp);
        qApp->installEventFilter(st.filter);
    }
    else if (!on && st.filter)
    {
        qApp->removeEventFilter(st.filter);
        delete st.filter;
        st.filter = nullptr;
    }
    if (on && !st.tick->isActive())
        st.tick->start();
    else if (!on)
    {
        st.tick->stop();
        st.cards.clear();
        st.caption.clear();
        if (st.ring)
            st.ring->hide();
    }
    if (!on && st.editing)
        setEditing(false);
    relayoutAll();
}

/** A one-line box near the caption's place; Enter shows the text (empty clears it), Esc or clicking away closes it. */
void askCaption()
{
    State& st = state();
    if (!st.main)
        return;
    auto* popup = new QFrame(st.main, Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QHBoxLayout(popup);
    layout->setContentsMargins(6, 6, 6, 6);
    auto* edit = new QLineEdit(popup);
    edit->setPlaceholderText("Caption to show, then Enter");
    edit->setText(st.caption);
    edit->selectAll();
    edit->setMinimumWidth(420);
    layout->addWidget(edit);
    QObject::connect(edit, &QLineEdit::returnPressed, popup, [popup, edit] {
        showCaption(edit->text());
        popup->close();
    });
    popup->adjustSize();
    const QRect m = mainRect();
    popup->move(m.x() + (m.width() - popup->width()) / 2, m.y() + int(m.height() * 0.12));
    popup->show();
    edit->setFocus();
}

/** Keeps the blocks on the main window as it moves, resizes, hides and comes back. */
class MainWatch : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::WindowStateChange: QTimer::singleShot(0, [] { relayoutAll(); }); break;
        default: break;
        }
        return false;
    }
};

Block* makeBlock(QWidget* main, const BlockDef& d)
{
    const std::string id = d.id;
    if (id == "keys")
        return new KeysBlock(main, d);
    if (id == "actions")
        return new ActionsBlock(main, d);
    if (id == "context")
        return new ContextBlock(main, d);
    return new CaptionBlock(main, d);
}
} // namespace

void install(QWidget* main)
{
    State& st = state();
    if (st.main)
        return;
    st.main = main;
    now();
    for (const BlockDef& d : kPresenterBlocks)
        st.blocks.push_back(makeBlock(main, d));
    st.ring = new CursorRing(main);
    st.tick = new QTimer(main);
    st.tick->setInterval(16);
    QObject::connect(st.tick, &QTimer::timeout, main, [] { onTick(); });
    main->installEventFilter(new MainWatch(main));
    st.runListener = actions::addRunListener(onRun);
    st.executeListener = ops::addExecuteListener(onExecute);
    PrefStore::get().onChanged([last = enabled()](const std::string& file) mutable {
        if (file != kPresenterPrefs || !state().main)
            return;
        if (std::exchange(last, enabled()) != last)
            applyEnabled();
        if (state().card)
            state().card->sync();
        relayoutAll();
    });

    Action toggle{"view.presenter", "Presenter Mode", "View", [] { setEnabled(!enabled()); }, nullptr,
                  "Shows the keys you press, the actions you run with their menu path and hotkey, the tool and mode, captions and "
                  "a cursor highlight over Lila's window, for streams and tutorials. It ignores the mouse."};
    toggle.checked = [] { return enabled(); };
    actions::add(std::move(toggle));
    Action layout{"view.presenterLayout", "Edit Presenter Layout", "View", [] { setEditing(!editing()); }, [] { return enabled(); },
                  "While on, the presenter's blocks take the mouse: drag to move them, scroll to scale them, right-click for "
                  "more, and pick which elements show."};
    layout.checked = [] { return editing(); };
    actions::add(std::move(layout));
    actions::add({"view.presenterCaption", "Presenter Caption...", "View", [] { askCaption(); }, [] { return enabled(); },
                  "Types a caption that the presenter shows large for a few seconds, such as what you are about to do. Enter on an "
                  "empty box clears it."});
    applyEnabled();
}

bool enabled()
{
    return PrefStore::get().value<bool>(kPresenterPrefs, presenterKey("enabled"), false);
}

void setEnabled(bool on)
{
    PrefStore::get().set(kPresenterPrefs, presenterKey("enabled"), on);
    applyEnabled();
    if (on)
        achievementRules::presenterModeOn();
}

bool editing()
{
    return state().editing;
}

void setEditing(bool on)
{
    State& st = state();
    on = on && enabled();
    if (st.editing == on)
        return;
    st.editing = on;
    for (Block* b : st.blocks)
        b->setEditable(on);
    if (on && st.main)
    {
        if (!st.card)
            st.card = new LayoutCard(st.main);
        st.card->sync();
        const QRect m = mainRect();
        st.card->move(m.center() - QPoint(st.card->width() / 2, st.card->height() / 2));
        st.card->show();
        st.card->raise();
    }
    else if (st.card)
        st.card->hide();
    relayoutAll();
    if (st.ring)
        st.ring->follow();
}

void showCaption(const QString& text)
{
    State& st = state();
    st.caption = text.trimmed();
    st.captionAt = now();
    relayoutAll();
}

std::vector<QString> cards()
{
    std::vector<QString> out;
    for (const Card& c : state().cards)
        out.push_back(c.title + "|" + c.detail + "|" + c.key);
    return out;
}
} // namespace lila::ui::presenter
