#include "ui/HotkeyCheatSheet.h"

#include "ui/Actions.h"
#include "ui/HotkeyBindings.h"
#include "ui/HotkeyChord.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
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
        return;
    QWidget* host = hostWindow();
    if (!host)
        return;
    gSheet = new HotkeyCheatSheet(host, QApplication::focusWidget());
    gSheet->show();
    qApp->installEventFilter(gSheet);
}

void HotkeyCheatSheet::dismiss()
{
    if (gSheet)
        gSheet->deleteLater();
    gSheet = nullptr;
}

HotkeyCheatSheet::HotkeyCheatSheet(QWidget* host, QWidget* focus)
    : QWidget(host, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus |
                        Qt::WindowTransparentForInput)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    if (host)
        setGeometry(QRect(host->mapToGlobal(QPoint(0, 0)), host->size()));

    hotkeys::Snapshot snap = hotkeys::snapshot();
    std::vector<std::string> chain = hotkeys::contextChain(focus);
    QStringList chainText;
    for (const std::string& c : chain)
        chainText << QString::fromStdString(c);
    mTitle = "Hotkeys   " + chainText.join(QString::fromUtf8("  ›  "));

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
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.label < b.second.label; });
        Group g{title, {}};
        for (auto& [chord, row] : rows)
            g.rows.push_back(row);
        mGroups.push_back(std::move(g));
    }
    // "Other" reads best last.
    std::stable_partition(mGroups.begin(), mGroups.end(), [](const Group& g) { return g.title != "Other"; });
}

bool HotkeyCheatSheet::eventFilter(QObject*, QEvent* e)
{
    switch (e->type())
    {
    case QEvent::MouseButtonPress:
    case QEvent::ApplicationDeactivate:
        dismiss();
        break;
    case QEvent::KeyPress:
        // The held F1 autorepeats; any other key dismisses.
        if (!static_cast<QKeyEvent*>(e)->isAutoRepeat())
            dismiss();
        break;
    default:
        break;
    }
    return false;
}

void HotkeyCheatSheet::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QRectF panel = QRectF(rect()).adjusted(28, 28, -28, -28);
    p.setPen(QPen(QColor(90, 90, 90, 220), 1));
    p.setBrush(QColor(14, 14, 14, 222));
    p.drawRoundedRect(panel, 8, 8);

    QRectF content = panel.adjusted(28, 22, -28, -22);
    QFont titleFont = font();
    titleFont.setPixelSize(17);
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(QColor(235, 235, 235));
    p.drawText(content, Qt::AlignLeft | Qt::AlignTop, mTitle);
    QFont hintFont = font();
    hintFont.setPixelSize(12);
    p.setFont(hintFont);
    p.setPen(QColor(130, 130, 130));
    p.drawText(content, Qt::AlignRight | Qt::AlignTop, "release F1 to close");
    content.setTop(content.top() + 38);

    int columns = content.width() > 1150 ? 3 : 2;
    double colGap = 32;
    double colWidth = (content.width() - colGap * (columns - 1)) / columns;

    // Shrink the line height until the tallest column fits.
    int totalRows = 0;
    for (const Group& g : mGroups)
        totalRows += int(g.rows.size()) + 2;
    double line = 22;
    std::vector<std::vector<const Group*>> cols;
    for (; line >= 11; line -= 1)
    {
        cols.assign(columns, {});
        double target = totalRows * line / columns;
        int col = 0;
        double used = 0, tallest = 0;
        for (const Group& g : mGroups)
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
    QFontMetricsF monoFm(mono);
    double keyWidth = 0;
    for (const Group& g : mGroups)
        for (const Row& r : g.rows)
            keyWidth = std::max(keyWidth, monoFm.horizontalAdvance(r.key) + 14);
    keyWidth = std::min(keyWidth, colWidth * 0.45);

    for (int c = 0; c < columns; ++c)
    {
        double x = content.left() + c * (colWidth + colGap);
        double y = content.top();
        for (const Group* g : cols[c])
        {
            p.setFont(header);
            p.setPen(QColor(150, 150, 150));
            p.drawText(QRectF(x, y, colWidth, line), Qt::AlignLeft | Qt::AlignVCenter, g->title);
            p.setPen(QPen(QColor(70, 70, 70), 1));
            p.drawLine(QPointF(x, y + line - 2), QPointF(x + colWidth, y + line - 2));
            y += line + 2;
            for (const Row& r : g->rows)
            {
                QRectF keyRect(x + keyWidth - monoFm.horizontalAdvance(r.key) - 12, y + 2, monoFm.horizontalAdvance(r.key) + 10, line - 4);
                p.setPen(QPen(QColor(105, 105, 105), 1));
                p.setBrush(QColor(44, 44, 44, 235));
                p.drawRoundedRect(keyRect, 3, 3);
                p.setFont(mono);
                p.setPen(QColor(230, 230, 230));
                p.drawText(keyRect, Qt::AlignCenter, r.key);

                QRectF textRect(x + keyWidth + 6, y, colWidth - keyWidth - 6, line);
                p.setFont(label);
                p.setPen(QColor(205, 205, 205));
                QFontMetricsF fm(label);
                QString suffix = r.context.isEmpty() ? QString() : "  " + r.context;
                double suffixW = fm.horizontalAdvance(suffix);
                QString text = fm.elidedText(r.label, Qt::ElideRight, textRect.width() - suffixW);
                p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
                if (!suffix.isEmpty())
                {
                    p.setPen(QColor(120, 120, 120));
                    p.drawText(textRect.adjusted(fm.horizontalAdvance(text), 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, suffix);
                }
                y += line;
            }
            y += line * 0.6;
        }
    }
}
} // namespace lila::ui
