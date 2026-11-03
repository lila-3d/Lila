#include "ui/HotkeyCheatSheet.h"

#include "ui/AchievementRules.h"
#include "ui/Actions.h"
#include "ui/HotkeyBindings.h"
#include "ui/HotkeyChord.h"
#include "ui/Hotkeys.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <map>

namespace lila::ui
{
namespace
{
QPointer<HotkeyCheatSheet> gSheet;

/** A release sooner than this after the press is a tap, which leaves the sheet open. */
constexpr qint64 kTapMs = 300;
constexpr double kTitleHeight = 30;
constexpr double kFilterHeight = 26;
/** Row heights the layout tries, largest first; past the smallest the list scrolls rather than shrinking. */
constexpr double kLineMax = 26;
constexpr double kLineMin = 22;
constexpr double kMinColumnWidth = 300;
constexpr int kMaxColumns = 5;
constexpr double kColumnGap = 36;
/** A category shorter than this moves whole to the next column instead of splitting. */
constexpr int kMinSplitRows = 6;

double headerHeight(double line)
{
    return line + 8;
}

double groupGap(double line)
{
    return line * 0.8;
}

QFont keyFont(double line)
{
    QFont f = monoFont();
    f.setPixelSize(qRound(line * 0.54));
    return f;
}

QFont labelFont(const QFont& base, double line)
{
    QFont f = base;
    f.setPixelSize(qRound(line * 0.58));
    return f;
}

/** The main window that owns w, so tool windows (the floating shelf, floating panels) never host the sheet. */
QWidget* mainWindowOf(QWidget* w)
{
    for (; w; w = w->parentWidget())
    {
        w = w->window();
        if (qobject_cast<QMainWindow*>(w) && w->isVisible())
            return w;
    }
    return nullptr;
}

QWidget* hostWindow()
{
    if (QWidget* w = mainWindowOf(QApplication::widgetAt(QCursor::pos())))
        return w;
    if (QWidget* w = mainWindowOf(QApplication::activeWindow()))
        return w;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (qobject_cast<QMainWindow*>(w) && w->isVisible())
            return w;
    return nullptr;
}
} // namespace

HotkeyCheatSheet* HotkeyCheatSheet::active()
{
    return gSheet;
}

void HotkeyCheatSheet::open()
{
    if (gSheet)
    {
        if (gSheet->mPinned)
            dismiss();
        return;
    }
    QWidget* host = hostWindow();
    if (!host)
        return;
    gSheet = new HotkeyCheatSheet(host, QApplication::focusWidget());
    gSheet->show();
    qApp->installEventFilter(gSheet);
    // Only a key going down can be held; a menu, button or script opens it ready to filter.
    if (!Hotkeys::fromKey())
        gSheet->pin();
    achievementRules::cheatSheetShown();
}

void HotkeyCheatSheet::released()
{
    if (!gSheet || gSheet->mPinned)
        return;
    if (gSheet->mOpened.elapsed() < kTapMs)
        gSheet->pin();
    else
        dismiss();
}

void HotkeyCheatSheet::dismiss()
{
    if (!gSheet)
        return;
    if (gSheet->mPinned && gSheet->mFocus)
    {
        gSheet->mFocus->window()->activateWindow();
        gSheet->mFocus->setFocus();
    }
    qApp->removeEventFilter(gSheet);
    gSheet->hide();
    gSheet->deleteLater();
    gSheet = nullptr;
}

HotkeyCheatSheet::HotkeyCheatSheet(QWidget* host, QWidget* focus)
    : QWidget(host, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus |
                        Qt::WindowTransparentForInput),
      mFocus(focus)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    mOpened.start();

    mFilter = new QLineEdit(this);
    mFilter->setPlaceholderText("Filter: action, key or category");
    mFilter->setClearButtonEnabled(true);
    mFilter->setFocusPolicy(Qt::ClickFocus);
    connect(mFilter, &QLineEdit::textChanged, this, [this] {
        mScroll = 0;
        applyFilter();
        update();
    });
    if (host)
        setGeometry(QRect(host->mapToGlobal(QPoint(0, 0)), host->size()));

    hotkeys::Snapshot snap = hotkeys::snapshot();
    std::vector<std::string> chain = hotkeys::contextChain(focus);
    QStringList chainText;
    for (const std::string& c : chain)
        chainText << QString::fromStdString(c);
    mTitle = "Hotkeys   " + chainText.join(QString("  ") + QChar(0x203A) + "  ");

    std::map<QString, std::vector<std::pair<QKeyCombination, Row>>> byCategory;
    for (const hotkeys::Binding* b : hotkeys::active(snap, focus))
    {
        QString category = "Other";
        for (const std::string& id : hotkeys::actionIds(*b))
            if (const Action* a = actions::find(id))
            {
                category = QString::fromStdString(a->category);
                break;
            }
        // The primary target only; release halves of press/release pairs just undo the press.
        QString label = !b->press.is_null() ? QString::fromStdString(actions::labelFor(b->press))
                                            : QString::fromStdString(actions::labelFor(b->hold.is_null() ? b->release : b->hold));
        if (!b->press.is_null() && !b->hold.is_null())
            label += "  /  hold: " + QString::fromStdString(actions::labelFor(b->hold));
        for (int i = 0; i < 3; ++i)
            if (!b->buttons[i].is_null())
                label += QString("  /  +%1: ").arg(hotkeys::kButtonKeys[i]) + QString::fromStdString(actions::labelFor(b->buttons[i]));
        Row row{hotkeys::displayText(b->chord), label,
                b->context == "global" ? QString() : QString::fromStdString(b->context)};
        byCategory[category].emplace_back(b->chord, row);
    }
    for (auto& [title, rows] : byCategory)
    {
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.label.compare(b.second.label, Qt::CaseInsensitive) < 0; });
        Group g{title, {}};
        for (auto& [chord, row] : rows)
            g.rows.push_back(row);
        mGroups.push_back(std::move(g));
    }
    // "Other" reads best last.
    std::stable_partition(mGroups.begin(), mGroups.end(), [](const Group& g) { return g.title != "Other"; });
    applyFilter();
}

void HotkeyCheatSheet::pin()
{
    mPinned = true;
    // A held sheet lets clicks through to the window under it; a pinned one takes input for its filter.
    const Qt::WindowFlags input = Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput;
    // QWidget::setWindowFlags recreates a shown native window, which blinks; QWindow::setFlags restyles it in place.
    if (QWindow* w = windowHandle())
        w->setFlags(w->flags() & ~input);
    else
        setWindowFlags(windowFlags() & ~input);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    raise();
    activateWindow();
    mFilter->setFocus();
    update();
}

int HotkeyCheatSheet::shownRows() const
{
    int n = 0;
    for (const Group& g : mShown)
        n += int(g.rows.size());
    return n;
}

QStringList HotkeyCheatSheet::shownCategories() const
{
    QStringList out;
    for (const Group& g : mShown)
        out << g.title;
    return out;
}

void HotkeyCheatSheet::applyFilter()
{
    const QStringList words = mFilter->text().split(' ', Qt::SkipEmptyParts);
    mShown.clear();
    for (const Group& g : mGroups)
    {
        Group shown{g.title, {}};
        for (const Row& r : g.rows)
        {
            const QString hay = g.title + ' ' + r.label + ' ' + r.key + ' ' + r.context;
            if (std::all_of(words.begin(), words.end(), [&](const QString& w) { return hay.contains(w, Qt::CaseInsensitive); }))
                shown.rows.push_back(r);
        }
        if (!shown.rows.empty())
            mShown.push_back(std::move(shown));
    }
    relayout();
}

void HotkeyCheatSheet::relayout()
{
    const QRectF list = listRect();
    mLayout = {};
    if (mShown.empty() || list.width() <= 0)
        return;

    const int columns = std::clamp(int((list.width() + kColumnGap) / (kMinColumnWidth + kColumnGap)), 1, kMaxColumns);
    const double colWidth = (list.width() - kColumnGap * (columns - 1)) / columns;

    // Pours the categories down columns of height h; false when they need more columns than there are.
    auto pour = [&](double line, double h, std::vector<Placed>* out, double* used) {
        const double hdr = headerHeight(line);
        int col = 0;
        double y = 0, tallest = 0;
        auto nextColumn = [&] {
            tallest = std::max(tallest, y);
            y = 0;
            return ++col < columns;
        };
        auto place = [&](const Group& g, int row, bool cont) {
            if (out)
                out->push_back({&g, row, col, cont, QPointF(col * (colWidth + kColumnGap), y)});
            y += row < 0 ? hdr : line;
        };
        for (const Group& g : mShown)
        {
            const int n = int(g.rows.size());
            const double start = hdr + (n < kMinSplitRows ? n : 2) * line;
            if (y > 0 && y + start > h && !nextColumn())
                return false;
            place(g, -1, false);
            for (int i = 0; i < n; ++i)
            {
                if (y + line > h)
                {
                    if (!nextColumn())
                        return false;
                    place(g, -1, true);
                }
                place(g, i, false);
            }
            y += groupGap(line);
        }
        if (used)
            *used = std::max(tallest, y - groupGap(line));
        return true;
    };
    auto balancedHeight = [&](double line) {
        double total = 0, tallestStart = 0;
        for (const Group& g : mShown)
        {
            const int n = int(g.rows.size());
            total += headerHeight(line) + n * line + groupGap(line);
            tallestStart = std::max(tallestStart, headerHeight(line) + (n < kMinSplitRows ? n : 2) * line);
        }
        double lo = std::max(total / columns, tallestStart), hi = std::max(total, tallestStart);
        for (int i = 0; i < 24 && hi - lo > 1; ++i)
        {
            const double mid = (lo + hi) / 2;
            (pour(line, mid, nullptr, nullptr) ? hi : lo) = mid;
        }
        return hi;
    };

    double line = kLineMax, h = balancedHeight(line);
    while (h > list.height() && line > kLineMin)
        h = balancedHeight(--line);

    mLayout.line = line;
    mLayout.columns = columns;
    mLayout.colWidth = colWidth;
    pour(line, h, &mLayout.items, &mLayout.height);

    QFontMetricsF keyFm(keyFont(line));
    mLayout.keyWidth.assign(columns, 0.0);
    for (const Placed& item : mLayout.items)
        if (item.row >= 0)
        {
            double& w = mLayout.keyWidth[item.column];
            w = std::max(w, keyFm.horizontalAdvance(item.group->rows[item.row].key) + 16);
        }
    for (double& w : mLayout.keyWidth)
        w = std::min(w, colWidth * 0.45);
    mScroll = std::clamp(mScroll, 0.0, maxScroll());
}

double HotkeyCheatSheet::maxScroll() const
{
    return std::max(0.0, mLayout.height - listRect().height());
}

void HotkeyCheatSheet::scrollTo(double y)
{
    const double clamped = std::clamp(y, 0.0, maxScroll());
    if (clamped == mScroll)
        return;
    mScroll = clamped;
    update();
}

void HotkeyCheatSheet::wheelEvent(QWheelEvent* e)
{
    scrollTo(mScroll - e->angleDelta().y() / 120.0 * mLayout.line * 3);
    e->accept();
}

QRectF HotkeyCheatSheet::panelRect() const
{
    return QRectF(rect()).adjusted(28, 28, -28, -28);
}

QRectF HotkeyCheatSheet::listRect() const
{
    QRectF list = panelRect().adjusted(28, 22, -28, -22);
    list.setTop(list.top() + kTitleHeight + kFilterHeight + 16);
    return list;
}

void HotkeyCheatSheet::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    const QRectF content = panelRect().adjusted(28, 22, -28, -22);
    mFilter->setGeometry(QRectF(content.left(), content.top() + kTitleHeight, std::min(360.0, content.width()), kFilterHeight).toRect());
    relayout();
}

bool HotkeyCheatSheet::eventFilter(QObject* obj, QEvent* e)
{
    switch (e->type())
    {
    case QEvent::MouseButtonPress:
        if (mPinned)
            if (auto* w = qobject_cast<QWidget*>(obj); w && (w == mFilter || mFilter->isAncestorOf(w) ||
                                                         (w == this && panelRect().contains(static_cast<QMouseEvent*>(e)->position()))))
                return false;
        dismiss();
        break;
    case QEvent::ApplicationDeactivate: dismiss(); break;
    case QEvent::KeyPress:
    {
        auto* k = static_cast<QKeyEvent*>(e);
        if (!mPinned)
        {
            // The held F1 autorepeats; any other key dismisses.
            if (!k->isAutoRepeat())
                dismiss();
            break;
        }
        if (k->key() == Qt::Key_Escape || (k->key() == Qt::Key_F1 && k->modifiers() == Qt::NoModifier && !k->isAutoRepeat()))
        {
            dismiss();
            return true;
        }
        const double page = listRect().height() - mLayout.line * 2;
        switch (k->key())
        {
        case Qt::Key_Down: scrollTo(mScroll + mLayout.line * 3); return true;
        case Qt::Key_Up: scrollTo(mScroll - mLayout.line * 3); return true;
        case Qt::Key_PageDown: scrollTo(mScroll + page); return true;
        case Qt::Key_PageUp: scrollTo(mScroll - page); return true;
        default: break;
        }
        break;
    }
    default: break;
    }
    return false;
}

void HotkeyCheatSheet::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QRectF panel = panelRect();
    p.setPen(QPen(themeShade(90, 220), 1));
    p.setBrush(themeShade(14, 222));
    p.drawRoundedRect(panel, 8, 8);

    QRectF content = panel.adjusted(28, 22, -28, -22);
    QFont titleFont = font();
    titleFont.setPixelSize(17);
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(themeShade(235));
    p.drawText(content, Qt::AlignLeft | Qt::AlignTop, mTitle);
    const bool overflows = maxScroll() > 0;
    QFont hintFont = font();
    hintFont.setPixelSize(13);
    p.setFont(hintFont);
    p.setPen(themeShade(150));
    QString hint = mPinned ? (overflows ? "scroll for more   F1 or Esc to close" : "F1 or Esc to close")
                           : (overflows ? "release F1 to close, tap to scroll and search" : "release F1 to close, tap to search");
    p.drawText(content, Qt::AlignRight | Qt::AlignTop, hint);

    const QRectF list = listRect();
    if (mShown.empty())
    {
        QFont empty = font();
        empty.setPixelSize(15);
        p.setFont(empty);
        p.setPen(themeShade(150));
        p.drawText(list.adjusted(0, 40, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                   mGroups.empty() ? QString("No hotkeys here") : QString("No hotkeys match \"%1\"").arg(mFilter->text()));
        return;
    }

    const double line = mLayout.line;
    const double colWidth = mLayout.colWidth;
    const QFont key = keyFont(line);
    const QFont label = labelFont(font(), line);
    QFont header = label;
    header.setPixelSize(qRound(line * 0.5));
    header.setBold(true);
    header.setCapitalization(QFont::AllUppercase);
    header.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    QFont count = label;
    count.setPixelSize(qRound(line * 0.5));
    const QFontMetricsF keyFm(key), labelFm(label);
    const QColor accent = themeColor("accent");

    p.save();
    p.setClipRect(list);
    p.translate(list.left(), list.top() - mScroll);
    for (const Placed& item : mLayout.items)
    {
        const double x = item.pos.x(), y = item.pos.y();
        if (y + line < mScroll || y > mScroll + list.height())
            continue;
        if (item.row < 0)
        {
            QRectF band(x, y, colWidth, line + 2);
            p.setPen(Qt::NoPen);
            p.setBrush(themeShade(34, 235));
            p.drawRoundedRect(band, 3, 3);
            p.setBrush(accent);
            p.drawRoundedRect(QRectF(band.left(), band.top() + 4, 3, band.height() - 8), 1.5, 1.5);
            p.setFont(header);
            p.setPen(themeShade(230));
            p.drawText(band.adjusted(12, 0, -10, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       item.cont ? item.group->title + "  (cont.)" : item.group->title);
            p.setFont(count);
            p.setPen(themeShade(155));
            p.drawText(band.adjusted(12, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter, QString::number(item.group->rows.size()));
            continue;
        }

        const Row& r = item.group->rows[item.row];
        const double keyWidth = mLayout.keyWidth[item.column];
        const double keyW = keyFm.horizontalAdvance(r.key);
        QRectF keyRect(x + keyWidth - keyW - 14, y + 3, keyW + 12, line - 6);
        p.setPen(QPen(themeShade(105), 1));
        p.setBrush(themeShade(44, 235));
        p.drawRoundedRect(keyRect, 3, 3);
        p.setFont(key);
        p.setPen(themeShade(235));
        p.drawText(keyRect, Qt::AlignCenter, r.key);

        QRectF textRect(x + keyWidth + 6, y, colWidth - keyWidth - 6, line);
        p.setFont(label);
        p.setPen(themeShade(215));
        QString suffix = r.context.isEmpty() ? QString() : "  " + r.context;
        double suffixW = labelFm.horizontalAdvance(suffix);
        // The context tag gives way before the label is cut.
        if (labelFm.horizontalAdvance(r.label) + suffixW > textRect.width())
        {
            suffix.clear();
            suffixW = 0;
        }
        QString text = labelFm.elidedText(r.label, Qt::ElideRight, textRect.width() - suffixW);
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
        if (!suffix.isEmpty())
        {
            p.setPen(themeShade(150));
            p.drawText(textRect.adjusted(labelFm.horizontalAdvance(text), 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, suffix);
        }
    }
    p.restore();

    if (!overflows)
        return;
    // Fade the cut edges into the panel and draw a thumb in the right margin.
    const QColor bg = themeShade(14, 222);
    const double fade = line * 1.5;
    auto edge = [&](QRectF r, bool bottom) {
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        QColor clear = bg;
        clear.setAlpha(0);
        g.setColorAt(bottom ? 0 : 1, clear);
        g.setColorAt(bottom ? 1 : 0, bg);
        p.fillRect(r, g);
    };
    if (mScroll > 0)
        edge(QRectF(list.left(), list.top(), list.width(), fade), false);
    if (mScroll < maxScroll())
        edge(QRectF(list.left(), list.bottom() - fade, list.width(), fade), true);
    const double trackX = panel.right() - 14;
    const double thumbH = std::max(24.0, list.height() * list.height() / mLayout.height);
    const double thumbY = list.top() + (list.height() - thumbH) * (mScroll / maxScroll());
    p.setPen(Qt::NoPen);
    p.setBrush(themeShade(60, 200));
    p.drawRoundedRect(QRectF(trackX, list.top(), 4, list.height()), 2, 2);
    p.setBrush(themeShade(150, 230));
    p.drawRoundedRect(QRectF(trackX, thumbY, 4, thumbH), 2, 2);
}
} // namespace lila::ui
