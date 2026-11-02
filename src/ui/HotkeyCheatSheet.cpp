#include "ui/HotkeyCheatSheet.h"

#include "ui/AchievementRules.h"
#include "ui/Actions.h"
#include "ui/HotkeyBindings.h"
#include "ui/HotkeyChord.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>

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
    const bool shown = isVisible();
    setWindowFlags(windowFlags() & ~(Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput));
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    if (shown)
        show();
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
}

QRectF HotkeyCheatSheet::panelRect() const
{
    return QRectF(rect()).adjusted(28, 28, -28, -28);
}

void HotkeyCheatSheet::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    const QRectF content = panelRect().adjusted(28, 22, -28, -22);
    mFilter->setGeometry(QRectF(content.left(), content.top() + kTitleHeight, std::min(360.0, content.width()), kFilterHeight).toRect());
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
    QFont hintFont = font();
    hintFont.setPixelSize(12);
    p.setFont(hintFont);
    p.setPen(themeShade(130));
    p.drawText(content, Qt::AlignRight | Qt::AlignTop, mPinned ? "F1 or Esc to close" : "release F1 to close, tap to search");
    content.setTop(content.top() + kTitleHeight + kFilterHeight + 16);

    if (mShown.empty())
    {
        QFont empty = font();
        empty.setPixelSize(14);
        p.setFont(empty);
        p.setPen(themeShade(140));
        p.drawText(content.adjusted(0, 40, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                   mGroups.empty() ? QString("No hotkeys here") : QString("No hotkeys match \"%1\"").arg(mFilter->text()));
        return;
    }

    int columns = content.width() > 1150 ? 3 : 2;
    double colGap = 32;
    double colWidth = (content.width() - colGap * (columns - 1)) / columns;

    // Shrink the line height until the tallest column fits.
    int totalRows = 0;
    for (const Group& g : mShown)
        totalRows += int(g.rows.size()) + 2;
    double line = 22;
    std::vector<std::vector<const Group*>> cols;
    for (; line >= 11; line -= 1)
    {
        cols.assign(columns, {});
        double target = totalRows * line / columns;
        int col = 0;
        double used = 0, tallest = 0;
        for (const Group& g : mShown)
        {
            double h = (g.rows.size() + 2) * line;
            if (used > 0 && used + h > target + line * 2 && col + 1 < columns)
            {
                tallest = std::max(tallest, used);
                ++col;
                used = 0;
            }
            cols[col].push_back(&g);
            used += h;
        }
        tallest = std::max(tallest, used);
        if (tallest <= content.height())
            break;
    }

    QFont mono = monoFont();
    mono.setPixelSize(int(line * 0.6));
    QFont label = font();
    label.setPixelSize(int(line * 0.62));
    QFont header = label;
    header.setBold(true);
    header.setCapitalization(QFont::AllUppercase);
    header.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    QFont count = label;
    count.setPixelSize(int(line * 0.55));
    QFontMetricsF monoFm(mono);
    double keyWidth = 0;
    for (const Group& g : mShown)
        for (const Row& r : g.rows)
            keyWidth = std::max(keyWidth, monoFm.horizontalAdvance(r.key) + 14);
    keyWidth = std::min(keyWidth, colWidth * 0.45);
    const QColor accent = themeColor("accent");

    for (int c = 0; c < columns; ++c)
    {
        double x = content.left() + c * (colWidth + colGap);
        double y = content.top();
        for (const Group* g : cols[c])
        {
            QRectF band(x, y, colWidth, line);
            p.setPen(Qt::NoPen);
            p.setBrush(themeShade(34, 235));
            p.drawRoundedRect(band, 3, 3);
            p.setBrush(accent);
            p.drawRoundedRect(QRectF(band.left(), band.top() + 3, 3, band.height() - 6), 1.5, 1.5);
            p.setFont(header);
            p.setPen(themeShade(225));
            p.drawText(band.adjusted(10, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, g->title);
            p.setFont(count);
            p.setPen(themeShade(140));
            p.drawText(band.adjusted(10, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, QString::number(g->rows.size()));
            y += line + 4;
            for (const Row& r : g->rows)
            {
                QRectF keyRect(x + keyWidth - monoFm.horizontalAdvance(r.key) - 12, y + 2, monoFm.horizontalAdvance(r.key) + 10, line - 4);
                p.setPen(QPen(themeShade(105), 1));
                p.setBrush(themeShade(44, 235));
                p.drawRoundedRect(keyRect, 3, 3);
                p.setFont(mono);
                p.setPen(themeShade(230));
                p.drawText(keyRect, Qt::AlignCenter, r.key);

                QRectF textRect(x + keyWidth + 6, y, colWidth - keyWidth - 6, line);
                p.setFont(label);
                p.setPen(themeShade(205));
                QFontMetricsF fm(label);
                QString suffix = r.context.isEmpty() ? QString() : "  " + r.context;
                double suffixW = fm.horizontalAdvance(suffix);
                QString text = fm.elidedText(r.label, Qt::ElideRight, textRect.width() - suffixW);
                p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
                if (!suffix.isEmpty())
                {
                    p.setPen(themeShade(120));
                    p.drawText(textRect.adjusted(fm.horizontalAdvance(text), 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, suffix);
                }
                y += line;
            }
            y += line * 0.6;
        }
    }
}
} // namespace lila::ui
