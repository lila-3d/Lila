#include "ui/AchievementBanner.h"

#include "core/Paths.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QConicalGradient>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>

namespace lila::ui::achievementBanner
{
namespace
{
constexpr int kWidth = 380;
constexpr int kHeight = 92;
constexpr int kMargin = 18;
constexpr int kShownMs = 5000;
constexpr int kFadeMs = 250;
constexpr int kGlow = 12;         // room around the box for an unlock's glow
constexpr double kSwirlDegPerSec = 140;

QWidget* mainWindow()
{
    if (QWidget* w = QApplication::activeWindow(); w && qobject_cast<QMainWindow*>(w->window()))
        return w->window();
    for (QWidget* w : QApplication::topLevelWidgets())
        if (qobject_cast<QMainWindow*>(w) && w->isVisible())
            return w;
    return nullptr;
}

class Banner : public QWidget
{
public:
    Banner(QWidget* host, const achievements::Shown& s)
        : QWidget(host, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus |
                            Qt::WindowTransparentForInput),
          mShown(s)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::NoFocus);
        QString icon = QString::fromStdWString((paths::resourcesDir() / "achievements" / (s.api + ".png")).wstring());
        mIcon.load(icon);
        resize(kWidth + 2 * kGlow, kHeight + 2 * kGlow);
        QPoint corner = host->mapToGlobal(QPoint(host->width(), host->height()));
        move(corner - QPoint(kWidth + kMargin + kGlow, kHeight + kMargin + kGlow));
        mSwirl.setInterval(16);
        connect(&mSwirl, &QTimer::timeout, this, [this] {
            mAngle = std::fmod(mAngle + kSwirlDegPerSec * 0.016, 360.0);
            update();
        });
        swirlIfUnlocked();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        bool unlocked = mShown.goal == 0;
        QRectF box = QRectF(rect()).adjusted(kGlow + 0.5, kGlow + 0.5, -kGlow - 0.5, -kGlow - 0.5);
        QPainterPath path;
        path.addRoundedRect(box, 8, 8);
        if (unlocked)
        {
            // Gold with two bright highlights chasing round the edge, and a soft glow outside it.
            for (auto [width, alpha] : {std::pair{16.0, 28}, {10.0, 50}, {5.0, 110}})
            {
                p.setPen(QPen(QBrush(gold(box.center(), alpha)), width));
                p.drawPath(path);
            }
        }
        p.fillPath(path, themeColor("base"));
        p.setPen(unlocked ? QPen(QBrush(gold(box.center(), 255)), 2.0) : QPen(themeColor("highlight"), 1.5));
        p.drawPath(path);
        p.translate(kGlow, kGlow);

        const int pad = 12;
        const int iconSize = kHeight - 2 * pad;
        QRect iconRect(pad, pad, iconSize, iconSize);
        if (!mIcon.isNull())
            p.drawPixmap(iconRect, mIcon);

        const int x = iconRect.right() + pad;
        const int w = kWidth - x - pad;
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        p.setFont(small);
        p.setPen(themeColor("dimText"));
        p.drawText(QRect(x, pad, w, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   unlocked ? QStringLiteral("Achievement unlocked") : QString("%1 / %2").arg(mShown.progress).arg(mShown.goal));

        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(themeColor("text"));
        p.drawText(QRect(x, pad + 18, w, 22), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(QString::fromStdString(mShown.name), Qt::ElideRight, w));

        p.setFont(small);
        p.setPen(themeColor("dimText"));
        QRect desc(x, pad + 40, w, kHeight - pad - (pad + 40));
        if (unlocked)
            p.drawText(desc, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, QString::fromStdString(mShown.description));
        else
        {
            QRectF bar(x, desc.top() + 6, w, 5);
            p.setPen(Qt::NoPen);
            p.setBrush(themeColor("altBase"));
            p.drawRoundedRect(bar, 2.5, 2.5);
            p.setBrush(themeColor("highlight"));
            p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * mShown.progress / std::max(1, mShown.goal), bar.height()),
                              2.5, 2.5);
        }
    }

public:
    const std::string& api() const { return mShown.api; }
    bool leaving() const { return mLeaving; }
    void setLeaving() { mLeaving = true; }

    /** Shows a newer step (or the unlock) of the achievement on screen, and holds it there again. */
    void refresh(const achievements::Shown& s)
    {
        mShown = s;
        swirlIfUnlocked();
        update();
        mHide.start(kShownMs);
    }

    QTimer mHide;

private:
    void swirlIfUnlocked()
    {
        if (mShown.goal == 0 && !mSwirl.isActive())
            mSwirl.start();
    }

    QConicalGradient gold(QPointF centre, int alpha) const
    {
        QConicalGradient g(centre, mAngle);
        QColor dark(138, 106, 31, alpha), mid(232, 184, 74, alpha), bright(255, 242, 196, alpha);
        g.setColorAt(0.0, dark);
        g.setColorAt(0.10, bright);
        g.setColorAt(0.22, mid);
        g.setColorAt(0.5, dark);
        g.setColorAt(0.60, bright);
        g.setColorAt(0.72, mid);
        g.setColorAt(1.0, dark);
        return g;
    }

    achievements::Shown mShown;
    QPixmap mIcon;
    QTimer mSwirl;
    double mAngle = 0;
    bool mLeaving = false;
};

std::deque<achievements::Shown> gQueue;
QPointer<Banner> gCurrent;

void fade(QWidget* w, double from, double to, std::function<void()> done = {})
{
    auto* a = new QPropertyAnimation(w, "windowOpacity", w);
    a->setDuration(kFadeMs);
    a->setStartValue(from);
    a->setEndValue(to);
    if (done)
        QObject::connect(a, &QPropertyAnimation::finished, w, std::move(done));
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void next()
{
    if (gCurrent || gQueue.empty())
        return;
    QWidget* host = mainWindow();
    if (!host)
    {
        gQueue.clear();
        return;
    }
    gCurrent = new Banner(host, gQueue.front());
    gQueue.pop_front();
    Banner* b = gCurrent;
    // Closed, or gone with its window: either way the next one may show.
    QObject::connect(b, &QObject::destroyed, [] { QTimer::singleShot(0, [] { next(); }); });
    b->setWindowOpacity(0.0);
    b->show();
    fade(b, 0.0, 1.0);
    b->mHide.setSingleShot(true);
    QObject::connect(&b->mHide, &QTimer::timeout, b, [b] {
        b->setLeaving();
        fade(b, 1.0, 0.0, [b] { b->close(); });
    });
    b->mHide.start(kShownMs);
}
} // namespace

void show(const achievements::Shown& shown)
{
    if (gCurrent && !gCurrent->leaving() && gCurrent->api() == shown.api)
        return gCurrent->refresh(shown);
    // A newer step of progress replaces one still waiting for the same achievement.
    std::erase_if(gQueue, [&](const achievements::Shown& q) { return q.api == shown.api; });
    gQueue.push_back(shown);
    next();
}
} // namespace lila::ui::achievementBanner
