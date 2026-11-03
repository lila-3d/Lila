#include "ui/CreditsDialog.h"

#include "ui/Credits.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QBasicTimer>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QElapsedTimer>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimerEvent>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lila::ui
{
namespace
{
using credits::Credits;
using credits::Tier;

constexpr int kColumnWidth = 560;
constexpr double kRowSpeed = 26.0; // pixels per second
constexpr int kNameGap = 30;
constexpr int kMaxNameRows = 3;
constexpr int kFade = 36;

/** Windows' "Show animations" setting, off for people who need motion kept down. LILA_REDUCED_MOTION=0 or 1 overrides it. */
bool reducedMotion()
{
    if (const char* v = std::getenv("LILA_REDUCED_MOTION"); v && *v)
        return *v != '0';
#ifdef _WIN32
    BOOL animate = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0))
        return !animate;
#endif
    return false;
}

bool matches(const QString& name, const QString& query)
{
    return !query.isEmpty() && name.contains(query, Qt::CaseInsensitive);
}

void repolish(QWidget* w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
}

/**
 * A long list of names as slowly scrolling rows, two or three of them in alternating directions. Stops while hovered or
 * focused and while a search has a match in it, and lies still as wrapped lines when the names fit or when Windows has
 * animations turned off.
 */
class NameRows : public QWidget
{
public:
    struct Name
    {
        QString text;
        bool mark = false;
    };

    NameRows(std::vector<Name> names, const QString& markTip, QWidget* parent)
        : QWidget(parent), mNames(std::move(names)), mMarkTip(markTip), mStill(reducedMotion())
    {
        setObjectName("creditsRows");
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        mMark = icon("seedling").pixmap(QSize(12, 12), devicePixelRatioF());
        QStringList all;
        for (const Name& n : mNames)
            all << n.text;
        setAccessibleDescription(all.join(", "));
        mClock.start();
    }

    /** Highlights the first name containing query and stops its row there; an empty query clears it. */
    bool highlight(const QString& query)
    {
        mQuery = query;
        mMatch = -1;
        for (size_t i = 0; i < mNames.size() && mMatch < 0; ++i)
            if (matches(mNames[i].text, query))
                mMatch = int(i);
        if (mMatch >= 0 && moving())
            for (Row& row : mRows)
                if (auto it = std::ranges::find(row.items, mMatch); it != row.items.end())
                {
                    const double x = itemX(row, int(it - row.items.begin()));
                    row.offset = std::fmod(x + itemWidth(mMatch) / 2.0 - width() / 2.0 + row.width, row.width);
                }
        updateTimer();
        update();
        return mMatch >= 0;
    }

    bool hasHeightForWidth() const override
    {
        return true;
    }

    int heightForWidth(int w) const override
    {
        if (!mStill && totalWidth() > w)
            return rowCount(w) * rowHeight();
        return std::max(1, int(flowLines(w).size())) * rowHeight();
    }

    QSize sizeHint() const override
    {
        return {kColumnWidth, heightForWidth(kColumnWidth)};
    }

protected:
    void resizeEvent(QResizeEvent*) override
    {
        layoutRows();
        updateTimer();
    }

    void showEvent(QShowEvent*) override
    {
        layoutRows();
        updateTimer();
    }

    void hideEvent(QHideEvent*) override
    {
        mTimer.stop();
    }

    void enterEvent(QEnterEvent*) override
    {
        updateTimer();
    }

    void leaveEvent(QEvent*) override
    {
        updateTimer();
    }

    void focusInEvent(QFocusEvent*) override
    {
        updateTimer();
        update();
    }

    void focusOutEvent(QFocusEvent*) override
    {
        updateTimer();
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int i = itemAt(e->position().toPoint());
        setToolTip(i >= 0 && mNames[size_t(i)].mark ? mMarkTip : QString());
    }

    void timerEvent(QTimerEvent* e) override
    {
        if (e->timerId() != mTimer.timerId())
            return QWidget::timerEvent(e);
        const double dt = mClock.restart() / 1000.0;
        for (size_t r = 0; r < mRows.size(); ++r)
        {
            Row& row = mRows[r];
            if (row.width <= width())
                continue;
            const double dir = r % 2 ? -1.0 : 1.0;
            row.offset = std::fmod(row.offset + dir * kRowSpeed * dt + row.width, row.width);
        }
        update();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(font());
        if (moving())
        {
            for (size_t r = 0; r < mRows.size(); ++r)
            {
                const Row& row = mRows[r];
                const int y = int(r) * rowHeight();
                if (row.width <= width())
                {
                    paintLine(p, row.items, (width() - lineWidth(row.items)) / 2.0, y);
                    continue;
                }
                // Drawn twice over so the row wraps without a seam.
                for (double start = -row.offset; start < width(); start += row.width)
                    paintLine(p, row.items, start, y, true);
            }
            const QColor bg = palette().color(QPalette::Window);
            QColor clear = bg;
            clear.setAlpha(0);
            QLinearGradient left(0, 0, kFade, 0);
            left.setColorAt(0, bg);
            left.setColorAt(1, clear);
            p.fillRect(QRect(0, 0, kFade, height()), left);
            QLinearGradient right(width() - kFade, 0, width(), 0);
            right.setColorAt(0, clear);
            right.setColorAt(1, bg);
            p.fillRect(QRect(width() - kFade, 0, kFade, height()), right);
        }
        else
        {
            int y = 0;
            for (const std::vector<int>& line : flowLines(width()))
            {
                paintLine(p, line, (width() - lineWidth(line)) / 2.0, y);
                y += rowHeight();
            }
        }
        if (hasFocus())
        {
            p.setPen(QPen(themeColor("accent"), 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        }
    }

private:
    struct Row
    {
        std::vector<int> items;
        double width = 0;
        double offset = 0;
    };

    int rowHeight() const
    {
        return fontMetrics().height() + 14;
    }

    int itemWidth(int i) const
    {
        const Name& n = mNames[size_t(i)];
        return fontMetrics().horizontalAdvance(n.text) + (n.mark ? 16 : 0);
    }

    double lineWidth(const std::vector<int>& items) const
    {
        double w = 0;
        for (int i : items)
            w += itemWidth(i) + kNameGap;
        return std::max(0.0, w - kNameGap);
    }

    double itemX(const Row& row, int index) const
    {
        double x = 0;
        for (int k = 0; k < index; ++k)
            x += itemWidth(row.items[size_t(k)]) + kNameGap;
        return x;
    }

    int totalWidth() const
    {
        int w = 0;
        for (size_t i = 0; i < mNames.size(); ++i)
            w += itemWidth(int(i)) + kNameGap;
        return std::max(0, w - kNameGap);
    }

    int rowCount(int w) const
    {
        const int rows = int(std::ceil(totalWidth() / std::max(1.0, w * 1.5)));
        return std::clamp(rows, 1, std::min(kMaxNameRows, int(mNames.size())));
    }

    bool moving() const
    {
        return !mStill && totalWidth() > width();
    }

    void layoutRows()
    {
        mRows.assign(size_t(rowCount(width())), Row{});
        // Each name goes to the shortest row so far, keeping the rows close in length.
        for (size_t i = 0; i < mNames.size(); ++i)
        {
            Row& row = *std::ranges::min_element(mRows, {}, &Row::width);
            row.items.push_back(int(i));
            row.width += itemWidth(int(i)) + kNameGap;
        }
        for (size_t r = 0; r < mRows.size(); ++r)
            mRows[r].offset = mRows[r].width * 0.37 * double(r);
        if (!mQuery.isEmpty())
            highlight(mQuery);
    }

    std::vector<std::vector<int>> flowLines(int w) const
    {
        std::vector<std::vector<int>> lines(1);
        double x = 0;
        for (size_t i = 0; i < mNames.size(); ++i)
        {
            const int iw = itemWidth(int(i));
            if (!lines.back().empty() && x + kNameGap + iw > w)
            {
                lines.emplace_back();
                x = 0;
            }
            x += (lines.back().empty() ? 0 : kNameGap) + iw;
            lines.back().push_back(int(i));
        }
        return lines;
    }

    int itemAt(const QPoint& pos) const
    {
        auto hit = [&](const std::vector<int>& items, double start) {
            double x = start;
            for (int i : items)
            {
                if (pos.x() >= x && pos.x() < x + itemWidth(i))
                    return i;
                x += itemWidth(i) + kNameGap;
            }
            return -1;
        };
        const int line = pos.y() / rowHeight();
        if (moving())
        {
            if (line < 0 || line >= int(mRows.size()))
                return -1;
            const Row& row = mRows[size_t(line)];
            if (row.width <= width())
                return hit(row.items, (width() - lineWidth(row.items)) / 2.0);
            for (double start = -row.offset; start < width(); start += row.width)
                if (int i = hit(row.items, start); i >= 0)
                    return i;
            return -1;
        }
        const auto lines = flowLines(width());
        if (line < 0 || line >= int(lines.size()))
            return -1;
        return hit(lines[size_t(line)], (width() - lineWidth(lines[size_t(line)])) / 2.0);
    }

    /** looped: a scrolling row, which also needs the separator between its last name and its first. */
    void paintLine(QPainter& p, const std::vector<int>& items, double x, int y, bool looped = false)
    {
        const QFontMetrics fm = fontMetrics();
        const int h = rowHeight();
        for (size_t k = 0; k < items.size(); ++k)
        {
            const int i = items[k];
            const Name& n = mNames[size_t(i)];
            const int w = itemWidth(i);
            if (x + w >= 0 && x <= width())
            {
                const bool hit = i == mMatch;
                if (hit)
                {
                    QColor fill = themeColor("highlight");
                    fill.setAlpha(110);
                    p.setPen(Qt::NoPen);
                    p.setBrush(fill);
                    p.drawRoundedRect(QRectF(x - 6, y + 4, w + 12, h - 8), 4, 4);
                }
                p.setPen(themeColor(hit ? "brightText" : "text"));
                p.drawText(QPointF(x, y + (h + fm.ascent() - fm.descent()) / 2.0), n.text);
                if (n.mark)
                    p.drawPixmap(QPointF(x + fm.horizontalAdvance(n.text) + 4, y + (h - 12) / 2.0), mMark);
            }
            x += w;
            if (k + 1 < items.size() || looped)
            {
                p.setPen(Qt::NoPen);
                p.setBrush(themeColor("dimText"));
                p.drawEllipse(QPointF(x + kNameGap / 2.0, y + h / 2.0), 1.5, 1.5);
                x += kNameGap;
            }
        }
    }

    void updateTimer()
    {
        const bool run = isVisible() && moving() && !underMouse() && !hasFocus() && mMatch < 0;
        if (run && !mTimer.isActive())
        {
            mClock.restart();
            mTimer.start(16, Qt::PreciseTimer, this);
        }
        else if (!run)
            mTimer.stop();
    }

    std::vector<Name> mNames;
    QString mMarkTip;
    QPixmap mMark;
    std::vector<Row> mRows;
    QString mQuery;
    int mMatch = -1;
    bool mStill = false;
    QBasicTimer mTimer;
    QElapsedTimer mClock;
};

/**
 * A credited name. With a link it opens the browser on click, names the host it opens in its tooltip, and offers to
 * report the link on right-click.
 */
class NameLabel : public QLabel
{
public:
    NameLabel(const QString& text, std::string url, const char* name, std::function<void(const QString&)> status, QWidget* parent)
        : QLabel(text, parent), mUrl(std::move(url)), mStatus(std::move(status))
    {
        setObjectName(name);
        setAlignment(Qt::AlignCenter);
        if (!mUrl.empty())
        {
            setProperty("linked", true);
            setCursor(Qt::PointingHandCursor);
            setToolTip(QString("Opens %1\nRight-click to report this link").arg(QString::fromStdString(credits::linkHost(mUrl))));
        }
    }

    void setMatch(bool hit)
    {
        if (property("match").toBool() != hit)
        {
            setProperty("match", hit);
            repolish(this);
        }
    }

protected:
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && !mUrl.empty() && rect().contains(e->position().toPoint()))
            QDesktopServices::openUrl(QUrl(QString::fromStdString(mUrl)));
        QLabel::mouseReleaseEvent(e);
    }

    void contextMenuEvent(QContextMenuEvent* e) override
    {
        if (mUrl.empty())
            return;
        QMenu menu(this);
        const QString host = QString::fromStdString(credits::linkHost(mUrl));
        menu.addAction(icon("external-link"), "Open " + host, [this] { QDesktopServices::openUrl(QUrl(QString::fromStdString(mUrl))); });
        menu.addAction(icon("flag"), "Report This Link", [this] {
            QPointer<NameLabel> self(this);
            credits::report(text().toStdString(), mUrl, [self](bool sent) {
                if (self && self->mStatus)
                    self->mStatus(sent ? "Thanks. The link was reported and will be checked."
                                       : "Couldn't send the report. Check your connection and try again.");
            });
        });
        menu.exec(e->globalPos());
    }

private:
    std::string mUrl;
    std::function<void(const QString&)> mStatus;
};

class CreditsDialog : public QDialog
{
public:
    explicit CreditsDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle("Credits");
        setObjectName("creditsDialog");
        setAttribute(Qt::WA_DeleteOnClose);
        setStyleSheet(creditsStyle());

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 12, 0, 0);
        layout->setSpacing(0);

        mSearch = new QLineEdit(this);
        mSearch->setPlaceholderText("Find a name or skill");
        mSearch->setToolTip("Search names, and what contributors worked on (try OpenUSD)");
        mSearch->setClearButtonEnabled(true);
        mSearch->addAction(icon("search"), QLineEdit::LeadingPosition);
        mSearch->setMaximumWidth(320);
        connect(mSearch, &QLineEdit::textChanged, this, [this] { applySearch(true); });
        auto* top = new QHBoxLayout;
        top->setContentsMargins(16, 0, 16, 8);
        top->addStretch(1);
        top->addWidget(mSearch, 2);
        top->addStretch(1);
        layout->addLayout(top);

        mScroll = new QScrollArea(this);
        mScroll->setWidgetResizable(true);
        mScroll->setFrameShape(QFrame::NoFrame);
        mScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        layout->addWidget(mScroll, 1);

        mStatus = new QLabel(this);
        mStatus->setObjectName("creditsStatus");
        mStatus->setAlignment(Qt::AlignCenter);
        mStatus->setWordWrap(true);
        mStatus->hide();
        layout->addWidget(mStatus);

        build(credits::current());
        QPointer<CreditsDialog> self(this);
        credits::refreshIfStale([self](const Credits& c) {
            if (self)
                self->build(c);
        });
        resize(680, 780);
    }

private:
    QString creditsStyle() const
    {
        auto c = [](const char* token) { return themeColor(token).name(QColor::HexArgb); };
        QColor match = themeColor("highlight");
        match.setAlpha(110);
        return QString("QLabel#creditsTitle { color: %1; font-size: 22pt; font-weight: 600; }"
                       "QLabel#creditsBy, QLabel#creditsDim, QLabel#creditsRole { color: %2; }"
                       "QLabel#creditsHeading { color: %2; font-size: 10pt; font-weight: 600; }"
                       "QLabel#creditsEternal { color: %1; font-size: 18pt; font-weight: 600; }"
                       "QLabel#creditsMythic { color: %1; font-size: 15pt; font-weight: 600; }"
                       "QLabel#creditsLegendary { color: %1; font-size: 12pt; }"
                       "QLabel#creditsPatron { color: %3; font-size: 11pt; }"
                       "QLabel#creditsContributor { color: %1; font-size: 12pt; font-weight: 600; }"
                       "QLabel#creditsSponsor { color: %1; font-size: 12pt; font-weight: 600; padding: 10px; }"
                       "QLabel[linked=\"true\"]:hover { text-decoration: underline; }"
                       "QLabel[match=\"true\"] { background: %4; border-radius: 4px; padding: 0 6px; }"
                       "QFrame#creditsPatronPanel { background: %5; border: 1px solid %6; border-radius: 6px; }"
                       "QLabel#creditsStatus { color: %3; padding: 8px 16px; border-top: 1px solid %6; }")
            .arg(c("brightText"), c("dimText"), c("text"), match.name(QColor::HexArgb), c("panel"), c("border"));
    }

    void build(const Credits& shown)
    {
        mNames.clear();
        mRows.clear();
        auto* page = new QWidget;
        auto* outer = new QHBoxLayout(page);
        outer->setContentsMargins(16, 8, 16, 32);
        auto* column = new QWidget(page);
        column->setMaximumWidth(kColumnWidth);
        outer->addStretch(1);
        outer->addWidget(column, 100);
        outer->addStretch(1);
        mColumn = new QVBoxLayout(column);
        mColumn->setContentsMargins(0, 0, 0, 0);
        mColumn->setSpacing(0);

        addHeader(column);

        if (!shown.corporate.empty())
        {
            addHeading(column, "Corporate sponsors");
            auto* grid = new QGridLayout;
            grid->setSpacing(12);
            for (size_t i = 0; i < shown.corporate.size(); ++i)
            {
                const credits::Sponsor& s = shown.corporate[i];
                auto* tile = addName(column, QString::fromStdString(s.name), s.url, "creditsSponsor");
                if (const QString logo = credits::logoFile(s); !logo.isEmpty())
                {
                    QPixmap pm(logo);
                    if (!pm.isNull())
                    {
                        pm = pm.scaledToHeight(int(48 * devicePixelRatioF()), Qt::SmoothTransformation);
                        pm.setDevicePixelRatio(devicePixelRatioF());
                        tile->setPixmap(pm);
                        tile->setAccessibleName(QString::fromStdString(s.name));
                        tile->setProperty("plain", QString::fromStdString(s.name));
                    }
                }
                grid->addWidget(tile, int(i / 3), int(i % 3), Qt::AlignCenter);
            }
            mColumn->addLayout(grid);
        }

        for (Tier t : {Tier::Eternal, Tier::Mythic, Tier::Legendary, Tier::Patron, Tier::Supporter})
        {
            const std::vector<credits::Supporter> list = shown.tier(t);
            if (list.empty())
                continue;
            addHeading(column, credits::tierTitle(t));
            if (t == Tier::Supporter)
            {
                std::vector<NameRows::Name> names;
                for (const credits::Supporter& s : list)
                    names.push_back({QString::fromStdString(s.name), s.founder});
                addRows(column, std::move(names), "Founder: supported Lila before 1.0", credits::tierTitle(t));
                continue;
            }
            const int across = t == Tier::Patron ? 3 : t == Tier::Legendary ? 2 : 1;
            const char* style = t == Tier::Eternal ? "creditsEternal" : t == Tier::Mythic ? "creditsMythic"
                              : t == Tier::Legendary ? "creditsLegendary" : "creditsPatron";
            QWidget* host = column;
            auto* grid = new QGridLayout;
            grid->setHorizontalSpacing(16);
            grid->setVerticalSpacing(t >= Tier::Mythic ? 10 : 6);
            if (t == Tier::Patron)
            {
                auto* panel = new QFrame(column);
                panel->setObjectName("creditsPatronPanel");
                panel->setLayout(grid);
                grid->setContentsMargins(16, 14, 16, 14);
                host = panel;
                mColumn->addWidget(panel);
            }
            else
                mColumn->addLayout(grid);
            for (size_t i = 0; i < list.size(); ++i)
            {
                const credits::Supporter& s = list[i];
                QString text = QString::fromStdString(s.name);
                if (t == Tier::Legendary && s.number > 0)
                    text = QString("%1  <span style=\"color: %2;\">#%3</span>").arg(text.toHtmlEscaped(), themeColor("dimText").name()).arg(s.number);
                NameLabel* label = addName(host, text, s.url, style);
                if (t == Tier::Legendary)
                {
                    label->setTextFormat(Qt::RichText);
                    label->setProperty("plain", QString::fromStdString(s.name));
                }
                if (s.founder)
                    label->setToolTip((label->toolTip().isEmpty() ? QString() : label->toolTip() + "\n") + "Founder: supported Lila before 1.0");
                grid->addWidget(label, int(i / size_t(across)), int(i % size_t(across)), Qt::AlignCenter);
            }
        }

        if (!shown.contributors.empty())
        {
            addHeading(column, "Contributors");
            for (const credits::Contributor& ct : shown.contributors)
            {
                NameLabel* name = addName(column, QString::fromStdString(ct.name), ct.url, "creditsContributor");
                mColumn->addWidget(name, 0, Qt::AlignHCenter);
                QStringList detail;
                if (!ct.role.empty())
                    detail << QString::fromStdString(ct.role);
                QStringList areas;
                for (const std::string& a : ct.areas)
                    areas << QString::fromStdString(a);
                if (!areas.isEmpty())
                    detail << areas.join("  ·  ");
                name->setProperty("keywords", detail.join(" "));
                auto* role = new QLabel(detail.join("\n"), column);
                role->setObjectName("creditsRole");
                role->setAlignment(Qt::AlignCenter);
                role->setWordWrap(true);
                mColumn->addWidget(role);
                mColumn->addSpacing(14);
            }
        }

        if (!shown.testers.empty())
        {
            addHeading(column, "Testers");
            std::vector<NameRows::Name> names;
            for (const credits::Tester& t : shown.testers)
                names.push_back({QString::fromStdString(t.name), t.founding});
            addRows(column, std::move(names), "Founding Tester: tested Lila before 1.0", "Testers");
        }

        if (!shown.pastSponsors.empty())
        {
            addHeading(column, "Past sponsors");
            QStringList names;
            for (const credits::Sponsor& s : shown.pastSponsors)
                names << QString::fromStdString(s.name);
            auto* past = new QLabel(names.join("  ·  "), column);
            past->setObjectName("creditsDim");
            past->setAlignment(Qt::AlignCenter);
            past->setWordWrap(true);
            mColumn->addWidget(past);
        }

        addFooter(column);
        mColumn->addStretch(1);

        const int scroll = mScroll->verticalScrollBar()->value();
        delete mScroll->takeWidget();
        mScroll->setWidget(page);
        mScroll->verticalScrollBar()->setValue(scroll);
        applySearch(false);
    }

    void addHeader(QWidget* column)
    {
        auto* logo = new QLabel(column);
        logo->setPixmap(qApp->windowIcon().pixmap(QSize(56, 56), devicePixelRatioF()));
        logo->setAlignment(Qt::AlignCenter);
        mColumn->addWidget(logo);
        mColumn->addSpacing(6);
        auto* title = new QLabel("Lila", column);
        title->setObjectName("creditsTitle");
        title->setAlignment(Qt::AlignCenter);
        mColumn->addWidget(title);
        auto* by = new QLabel("by Jared Taylor, with thanks to everyone below", column);
        by->setObjectName("creditsBy");
        by->setAlignment(Qt::AlignCenter);
        mColumn->addWidget(by);
    }

    void addHeading(QWidget* column, const QString& text)
    {
        mColumn->addSpacing(36);
        auto* heading = new QLabel(text, column);
        heading->setObjectName("creditsHeading");
        heading->setAlignment(Qt::AlignCenter);
        mColumn->addWidget(heading);
        mColumn->addSpacing(12);
    }

    NameLabel* addName(QWidget* host, const QString& text, const std::string& url, const char* style)
    {
        auto* label = new NameLabel(text, url, style, [this](const QString& s) { showStatus(s); }, host);
        mNames.push_back(label);
        return label;
    }

    void addRows(QWidget* column, std::vector<NameRows::Name> names, const QString& markTip, const QString& title)
    {
        auto* rows = new NameRows(std::move(names), markTip, column);
        rows->setAccessibleName(title);
        QFont f = rows->font();
        f.setPointSizeF(f.pointSizeF() * 1.08);
        rows->setFont(f);
        mRows.push_back(rows);
        mColumn->addWidget(rows);
    }

    void addFooter(QWidget* column)
    {
        mColumn->addSpacing(44);
        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        row->addStretch(1);
        auto* pitch = new QLabel("Lila is free. Chip in:", column);
        pitch->setObjectName("creditsDim");
        row->addWidget(pitch);
        auto link = [&](const char* iconName, const QString& label, const QString& url) {
            auto* b = new QPushButton(icon(iconName), label, column);
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(url);
            connect(b, &QPushButton::clicked, b, [url] { QDesktopServices::openUrl(QUrl(url)); });
            row->addWidget(b);
        };
        link("coffee", "Ko-fi", "https://ko-fi.com/jaredtaylor");
        link("brand-github", "Sponsors", "https://github.com/sponsors/Vaei/");
        row->addStretch(1);
        mColumn->addLayout(row);

        if (const std::filesystem::path notices = credits::licenceNotices(); !notices.empty())
        {
            mColumn->addSpacing(16);
            auto* licences = new QPushButton("Open-Source Licences", column);
            licences->setFlat(true);
            licences->setCursor(Qt::PointingHandCursor);
            licences->setToolTip("The third-party components in Lila and their licences");
            const QUrl url = QUrl::fromLocalFile(QString::fromStdWString(notices.wstring()));
            connect(licences, &QPushButton::clicked, licences, [url] { QDesktopServices::openUrl(url); });
            auto* centre = new QHBoxLayout;
            centre->addStretch(1);
            centre->addWidget(licences);
            centre->addStretch(1);
            mColumn->addLayout(centre);
        }
    }

    void applySearch(bool scroll)
    {
        const QString query = mSearch->text().trimmed().size() >= 2 ? mSearch->text().trimmed() : QString();
        QWidget* first = nullptr;
        for (NameLabel* label : mNames)
        {
            const QString plain = label->property("plain").toString();
            const bool hit = matches(plain.isEmpty() ? label->text() : plain, query) || matches(label->property("keywords").toString(), query);
            label->setMatch(hit);
            if (hit && !first)
                first = label;
        }
        for (NameRows* rows : mRows)
            if (rows->highlight(query) && !first)
                first = rows;

        if (query.isEmpty())
            mStatus->hide();
        else if (!first)
            showStatus(QString("No one matching \"%1\" is listed.").arg(query));
        else
            mStatus->hide();
        if (scroll && first)
            mScroll->ensureWidgetVisible(first, 0, 80);
    }

    void showStatus(const QString& text)
    {
        mStatus->setText(text);
        mStatus->show();
    }

    QLineEdit* mSearch = nullptr;
    QScrollArea* mScroll = nullptr;
    QLabel* mStatus = nullptr;
    QVBoxLayout* mColumn = nullptr;
    std::vector<NameLabel*> mNames;
    std::vector<NameRows*> mRows;
};
} // namespace

void showCredits(QWidget* parent)
{
    static QPointer<CreditsDialog> open;
    if (!open)
    {
        open = new CreditsDialog(parent);
        open->show();
    }
    open->raise();
    open->activateWindow();
}
} // namespace lila::ui
