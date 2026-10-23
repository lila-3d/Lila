#include "ui/TimeSlider.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/Ghosting.h"
#include "ui/Icons.h"
#include "ui/ViewportDisplay.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
namespace anim = scene::anim;
using scene::Graph;

QColor gray(int v, int a = 255) { return QColor(v, v, v, a); }

void runUi(const char* command, const json& call = json::object())
{
    try
    {
        cmds::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

int frameOf(double seconds) { return int(std::lround(anim::frameOf(seconds))); }

/** A step that keeps labelled ticks at least minPx apart: 1, 2, 5, 10, 20, 50... frames. */
int niceStep(double pxPerFrame, double minPx)
{
    for (int base = 1;; base *= 10)
        for (int m : {1, 2, 5})
            if (base * m * pxPerFrame >= minPx || base > 1000000)
                return base * m;
}

enum class Glyph
{
    ToStart,
    StepBack,
    PrevKey,
    PlayBack,
    Play,
    Pause,
    StepForward,
    NextKey,
    ToEnd,
};

QIcon glyphIcon(Glyph g)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(gray(205));
    auto tri = [&](double x0, double x1) {
        QPainterPath path;
        path.moveTo(x0, 8);
        path.lineTo(x0, 24);
        path.lineTo(x1, 16);
        path.closeSubpath();
        p.drawPath(path);
    };
    auto bar = [&](double x) { p.drawRect(QRectF(x, 8, 3, 16)); };
    auto key = [&](double cx) { p.drawPolygon(QPolygonF({{cx, 10}, {cx + 6, 16}, {cx, 22}, {cx - 6, 16}})); };
    switch (g)
    {
    case Glyph::ToStart: bar(7); tri(24, 11); break;
    case Glyph::StepBack: tri(20, 7); bar(22); break;
    case Glyph::PlayBack: tri(23, 9); break;
    case Glyph::Play: tri(9, 23); break;
    case Glyph::Pause: bar(10); bar(19); break;
    case Glyph::StepForward: bar(7); tri(12, 25); break;
    case Glyph::PrevKey: key(10); tri(27, 17); break;
    case Glyph::NextKey: tri(5, 15); key(22); break;
    case Glyph::ToEnd: tri(8, 21); bar(22); break;
    }
    return QIcon(pm);
}

QToolButton* toolButton(QWidget* parent, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setIconSize(QSize(14, 14));
    b->setFixedSize(22, 22);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QSpinBox* frameField(QWidget* parent, const QString& tip)
{
    auto* s = new QSpinBox(parent);
    s->setRange(-1000000, 1000000);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setAlignment(Qt::AlignCenter);
    s->setFixedWidth(52);
    s->setToolTip(tip);
    s->setKeyboardTracking(false);
    return s;
}
} // namespace

/** Frame ruler over the playback range: scrub with LMB, key ticks for the selection, right-click for keys. */
class TimeRuler : public QWidget
{
public:
    explicit TimeRuler(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(28);
        setMouseTracking(false);
        setFocusPolicy(Qt::ClickFocus);
        mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
            if (file == "general")
                update();
        });
    }
    ~TimeRuler() override { PrefStore::get().removeOnChanged(mPrefListener); }

    void invalidateKeys()
    {
        mKeysValid = false;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        LILA_ZONE();

        QPainter p(this);
        p.fillRect(rect(), gray(40));
        const anim::Playback& pb = anim::playback();
        int first = frameOf(pb.minTime), last = frameOf(pb.maxTime);
        double ppf = pxPerFrame();
        int h = height();

        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        p.setFont(f);
        int labelStep = niceStep(ppf, 46);
        int tickStep = std::max(1, niceStep(ppf, 6));
        for (int fr = first - (first % tickStep); fr <= last; fr += tickStep)
        {
            if (fr < first)
                continue;
            double x = xOf(fr);
            bool major = fr % labelStep == 0;
            p.setPen(gray(major ? 120 : 78));
            p.drawLine(QPointF(x, h - (major ? 10 : 5)), QPointF(x, h));
            QString label = QString::number(fr);
            if (major && x + 3 + p.fontMetrics().horizontalAdvance(label) < width())
            {
                p.setPen(gray(140));
                p.drawText(QPointF(x + 3, h - 12), label);
            }
        }

        if (!mKeysValid)
        {
            Graph& g = Graph::current();
            mKeys = anim::keyTimes(g, g.selection());
            mLoopKeys = anim::loopKeyTimes(g, g.selection());
            mKeysValid = true;
        }
        double tickW = std::max(2.0, ppf * 0.35);
        QColor tick = gray(175);
        if (viewportDisplay::accentColors(viewportDisplay::kTimeKeyColors))
            tick = QColor::fromRgbF(viewportDisplay::kKeyTickColor.r, viewportDisplay::kKeyTickColor.g, viewportDisplay::kKeyTickColor.b);
        for (double t : mKeys)
        {
            double fr = anim::frameOf(t);
            if (fr < first - 0.5 || fr > last + 0.5)
                continue;
            p.fillRect(QRectF(xOf(fr) - tickW / 2, 2, tickW, h - 4), tick);
        }
        // Loop keys: the tick with caps top and bottom, bracketing the cycle.
        for (double t : mLoopKeys)
        {
            double fr = anim::frameOf(t);
            if (fr < first - 0.5 || fr > last + 0.5)
                continue;
            double capW = tickW * 3.0;
            p.fillRect(QRectF(xOf(fr) - capW, 1, capW, 2), tick);
            p.fillRect(QRectF(xOf(fr) - capW, h - 3, capW, 2), tick);
        }

        int cur = frameOf(Graph::current().time());
        double cx = xOf(cur);
        double cw = std::max(3.0, ppf);
        p.fillRect(QRectF(cx - cw / 2, 0, cw, h), gray(26, 220));
        QString label = QString::number(cur);
        QRect tr = p.fontMetrics().boundingRect(label).adjusted(-4, -1, 4, 1);
        tr.moveTopLeft(QPoint(int(cx + cw / 2 + 2), 2));
        if (tr.right() > width())
            tr.moveRight(int(cx - cw / 2 - 2));
        p.fillRect(tr, gray(26));
        p.setPen(gray(225));
        p.drawText(tr, Qt::AlignCenter, label);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton)
        {
            mScrubbing = true;
            scrubTo(e->position().x());
        }
        else if (e->button() == Qt::RightButton)
            showMenu(e->globalPosition().toPoint());
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (mScrubbing)
            scrubTo(e->position().x());
    }

    void mouseReleaseEvent(QMouseEvent*) override { mScrubbing = false; }

private:
    double pxPerFrame() const
    {
        const anim::Playback& pb = anim::playback();
        int span = std::max(1, frameOf(pb.maxTime) - frameOf(pb.minTime));
        return (width() - 2.0 * kMargin) / span;
    }
    double xOf(double frame) const { return kMargin + (frame - frameOf(anim::playback().minTime)) * pxPerFrame(); }

    void scrubTo(double x)
    {
        const anim::Playback& pb = anim::playback();
        int fr = int(std::lround((x - kMargin) / pxPerFrame())) + frameOf(pb.minTime);
        fr = std::clamp(fr, frameOf(pb.minTime), frameOf(pb.maxTime));
        double t = anim::secondsOf(fr);
        if (std::abs(t - Graph::current().time()) < 1e-9)
            return;
        cmds::execute("currentTime", {{"args", {t}}});
        notifyTimeChanged();
    }

    void showMenu(QPoint at)
    {
        double now = Graph::current().time();
        QMenu menu(this);
        menu.addAction("Set Key", [] { actions::run(json("anim.setKey")); });
        menu.addAction("Delete Key", [now] { runUi("cutKey", {{"time", now}, {"clear", true}}); });
        menu.addSeparator();
        menu.addAction("Copy", [now] { runUi("copyKey", {{"time", now}}); });
        menu.addAction("Cut", [now] { runUi("cutKey", {{"time", now}}); });
        menu.addAction("Paste", [] { runUi("pasteKey"); });
        menu.addSeparator();
        QMenu* speed = menu.addMenu("Playback Speed");
        for (anim::PlaybackSpeed s : anim::kPlaybackSpeeds)
        {
            std::string id = "anim.speed." + std::string(anim::speedName(s));
            QString label = s == anim::PlaybackSpeed::Custom ? QString("Custom (%1x)...").arg(anim::playback().customSpeed)
                                                              : QString::fromUtf8(anim::speedLabel(s).data());
            QAction* item = speed->addAction(label, [id] { actions::run(json(id)); });
            item->setCheckable(true);
            item->setChecked(anim::playback().speed == s);
        }
        menu.exec(at);
    }

    static constexpr double kMargin = 10.0;
    std::vector<double> mKeys;
    std::vector<double> mLoopKeys;
    bool mKeysValid = false;
    bool mScrubbing = false;
    int mPrefListener = 0;
};

/** Outer animation range with the playback range as a draggable bar inside it. */
class RangeBar : public QWidget
{
public:
    explicit RangeBar(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(18);
        setMinimumWidth(80);
        setCursor(Qt::SizeHorCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF track = QRectF(rect()).adjusted(1, 3, -1, -3);
        p.setPen(Qt::NoPen);
        p.setBrush(gray(34));
        p.drawRoundedRect(track, 3, 3);
        QRectF bar = barRect();
        p.setBrush(gray(mDrag == Drag::None ? 72 : 88));
        p.drawRoundedRect(bar, 3, 3);
        p.setBrush(gray(124));
        p.drawRect(QRectF(bar.left(), bar.top(), 3, bar.height()));
        p.drawRect(QRectF(bar.right() - 3, bar.top(), 3, bar.height()));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        QRectF bar = barRect();
        double x = e->position().x();
        const anim::Playback& pb = anim::playback();
        if (std::abs(x - bar.left()) < 6)
            mDrag = Drag::Start;
        else if (std::abs(x - bar.right()) < 6)
            mDrag = Drag::End;
        else if (x > bar.left() && x < bar.right())
            mDrag = Drag::Move;
        else
            return;
        mPressFrame = frameAt(x);
        mStartMin = frameOf(pb.minTime);
        mStartMax = frameOf(pb.maxTime);
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (mDrag == Drag::None)
            return;
        const anim::Playback& pb = anim::playback();
        int lo = frameOf(pb.animStart), hi = frameOf(pb.animEnd);
        int d = frameAt(e->position().x()) - mPressFrame;
        int mn = mStartMin, mx = mStartMax;
        if (mDrag == Drag::Move)
        {
            d = std::clamp(d, lo - mStartMin, hi - mStartMax);
            mn += d;
            mx += d;
        }
        else if (mDrag == Drag::Start)
            mn = std::clamp(mStartMin + d, lo, mStartMax);
        else
            mx = std::clamp(mStartMax + d, mStartMin, hi);
        if (mn != frameOf(pb.minTime) || mx != frameOf(pb.maxTime))
            cmds::execute("playbackOptions", {{"minTime", anim::secondsOf(mn)}, {"maxTime", anim::secondsOf(mx)}});
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        mDrag = Drag::None;
        update();
    }

private:
    enum class Drag
    {
        None,
        Start,
        End,
        Move,
    };

    double pxPerFrame() const
    {
        const anim::Playback& pb = anim::playback();
        int span = std::max(1, frameOf(pb.animEnd) - frameOf(pb.animStart));
        return (width() - 2.0) / span;
    }
    int frameAt(double x) const { return int(std::lround((x - 1.0) / pxPerFrame())) + frameOf(anim::playback().animStart); }
    QRectF barRect() const
    {
        const anim::Playback& pb = anim::playback();
        double x0 = 1.0 + (frameOf(pb.minTime) - frameOf(pb.animStart)) * pxPerFrame();
        double x1 = 1.0 + (frameOf(pb.maxTime) - frameOf(pb.animStart)) * pxPerFrame();
        return QRectF(x0, 3, std::max(8.0, x1 - x0), height() - 6);
    }

    Drag mDrag = Drag::None;
    int mPressFrame = 0;
    int mStartMin = 0;
    int mStartMax = 0;
};

TimeSlider::TimeSlider(QWidget* parent) : QWidget(parent)
{
    setProperty("hotkeyContext", "timeSlider");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 2, 4, 2);
    outer->setSpacing(2);

    auto* top = new QHBoxLayout();
    top->setSpacing(2);
    mRuler = new TimeRuler(this);
    top->addWidget(mRuler, 1);
    mFrame = frameField(this, "Current frame");
    top->addWidget(mFrame);
    auto addButton = [&](Glyph g, const QString& tip, const char* action) {
        QToolButton* b = toolButton(this, tip);
        b->setIcon(glyphIcon(g));
        connect(b, &QToolButton::clicked, this, [action] { actions::run(json(action)); });
        top->addWidget(b);
        return b;
    };
    addButton(Glyph::ToStart, "Go to start", "anim.firstFrame");
    addButton(Glyph::PrevKey, "Previous key (,)", "anim.prevKey");
    addButton(Glyph::StepBack, "Step back one frame (Alt+,)", "anim.prevFrame");
    mPlayBack = toolButton(this, "Play backwards");
    top->addWidget(mPlayBack);
    connect(mPlayBack, &QToolButton::clicked, this, [] {
        if (anim::playDirection() < 0)
            actions::run(json("anim.stop"));
        else
            actions::run(json("anim.playBackward"));
    });
    mPlay = toolButton(this, "Play forwards (Alt+V)");
    top->addWidget(mPlay);
    connect(mPlay, &QToolButton::clicked, this, [] {
        if (anim::playDirection() > 0)
            actions::run(json("anim.stop"));
        else
            runUi("play");
    });
    addButton(Glyph::StepForward, "Step forward one frame (Alt+.)", "anim.nextFrame");
    addButton(Glyph::NextKey, "Next key (.)", "anim.nextKey");
    addButton(Glyph::ToEnd, "Go to end", "anim.lastFrame");
    outer->addLayout(top);

    auto* bottom = new QHBoxLayout();
    bottom->setSpacing(2);
    mAnimStart = frameField(this, "Animation start");
    mMin = frameField(this, "Playback start");
    mRange = new RangeBar(this);
    mMax = frameField(this, "Playback end");
    mAnimEnd = frameField(this, "Animation end");
    bottom->addWidget(mAnimStart);
    bottom->addWidget(mMin);
    bottom->addWidget(mRange, 1);
    bottom->addWidget(mMax);
    bottom->addWidget(mAnimEnd);

    mFps = new QComboBox(this);
    for (double fps : {15.0, 24.0, 25.0, 30.0, 48.0, 50.0, 60.0, 120.0})
        mFps->addItem(QString("%1 fps").arg(fps), fps);
    mFps->setToolTip("Frames per second");
    mFps->setFocusPolicy(Qt::NoFocus);
    bottom->addWidget(mFps);
    mLoop = toolButton(this, "Loop playback");
    mLoop->setIcon(icon("repeat"));
    mLoop->setCheckable(true);
    bottom->addWidget(mLoop);
    mAutoKey = toolButton(this, "Auto Key: key animated channels whenever they change");
    mAutoKey->setIcon(icon("key"));
    mAutoKey->setCheckable(true);
    bottom->addWidget(mAutoKey);
    {
        QToolButton* ghost = toolButton(this, QString::fromStdString(actions::tooltip("anim.toggleGhosts")) + "\nRight-click for ghost options");
        ghost->setIcon(icon("ghost"));
        ghost->setCheckable(true);
        ghost->setChecked(ghostActionState("anim.toggleGhosts").value_or(false));
        ghost->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ghost, &QToolButton::clicked, this, [] { actions::run(json("anim.toggleGhosts")); });
        connect(ghost, &QToolButton::customContextMenuRequested, this, [ghost](const QPoint& at) {
            QMenu menu;
            for (const char* id : {"anim.ghostMode.keys", "anim.ghostMode.frames", "", "anim.ghostShow.mesh", "anim.ghostShow.skeleton",
                                   "anim.ghostShow.both", "", "anim.refreshGhosts"})
            {
                if (!*id)
                {
                    menu.addSeparator();
                    continue;
                }
                QAction* qa = menu.addAction(QString::fromStdString(actions::displayLabel(id)), [id] { actions::run(json(id)); });
                if (std::optional<bool> on = ghostActionState(id))
                {
                    qa->setCheckable(true);
                    qa->setChecked(*on);
                }
            }
            menu.exec(ghost->mapToGlobal(at));
        });
        int listener = PrefStore::get().onChanged([ghost](const std::string& file) {
            if (file != "general")
                return;
            QMetaObject::invokeMethod(ghost, [ghost] {
                QSignalBlocker block(ghost);
                ghost->setChecked(ghostActionState("anim.toggleGhosts").value_or(false));
            });
        });
        connect(ghost, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
        bottom->addWidget(ghost);
    }
    outer->addLayout(bottom);

    // Checked toggles read as lit, which grayscale needs to be unmistakable.
    setStyleSheet("QToolButton:checked { background: #686868; border: 1px solid #b4b4b4; border-radius: 2px; }");

    connect(mFrame, &QSpinBox::valueChanged, this, [](int f) {
        cmds::execute("currentTime", {{"args", {anim::secondsOf(f)}}});
        notifyTimeChanged();
    });
    auto rangeField = [this](QSpinBox* s, const char* flag) {
        connect(s, &QSpinBox::valueChanged, this, [flag](int f) { runUi("playbackOptions", {{flag, anim::secondsOf(f)}}); });
    };
    rangeField(mAnimStart, "animationStartTime");
    rangeField(mMin, "minTime");
    rangeField(mMax, "maxTime");
    rangeField(mAnimEnd, "animationEndTime");
    connect(mFps, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i >= 0)
            runUi("playbackOptions", {{"fps", mFps->itemData(i).toDouble()}});
    });
    connect(mLoop, &QToolButton::toggled, this, [](bool on) { runUi("playbackOptions", {{"loop", on}}); });
    connect(mAutoKey, &QToolButton::toggled, this, [](bool on) {
        if (on != anim::autoKey())
            runUi("autoKeyframe", {{"state", on}});
    });

    mGraphListener = Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c != scene::Change::Renamed && c != scene::Change::Reparented)
            mRuler->invalidateKeys();
    });
    mPlaybackListener = anim::addPlaybackListener([this] { refreshOptions(); });
    onTimeChanged(this, [this] { refreshTime(); });
    refreshOptions();
}

TimeSlider::~TimeSlider()
{
    Graph::current().removeListener(mGraphListener);
    anim::removePlaybackListener(mPlaybackListener);
}

QSize TimeSlider::sizeHint() const
{
    return {800, 56};
}

void TimeSlider::refreshOptions()
{
    const anim::Playback& pb = anim::playback();
    auto set = [](QSpinBox* s, double seconds) {
        QSignalBlocker block(s);
        s->setValue(frameOf(seconds));
    };
    set(mAnimStart, pb.animStart);
    set(mMin, pb.minTime);
    set(mMax, pb.maxTime);
    set(mAnimEnd, pb.animEnd);
    {
        QSignalBlocker block(mFps);
        int i = mFps->findData(pb.fps);
        if (i < 0)
        {
            mFps->addItem(QString("%1 fps").arg(pb.fps), pb.fps);
            i = mFps->count() - 1;
        }
        mFps->setCurrentIndex(i);
    }
    {
        QSignalBlocker block(mLoop);
        mLoop->setChecked(pb.loop);
    }
    {
        QSignalBlocker block(mAutoKey);
        mAutoKey->setChecked(anim::autoKey());
    }
    int dir = anim::playDirection();
    mPlay->setIcon(glyphIcon(dir > 0 ? Glyph::Pause : Glyph::Play));
    mPlayBack->setIcon(glyphIcon(dir < 0 ? Glyph::Pause : Glyph::PlayBack));
    mRuler->update();
    mRange->update();
    refreshTime();
}

void TimeSlider::refreshTime()
{
    if (!mFrame->hasFocus())
    {
        QSignalBlocker block(mFrame);
        mFrame->setValue(frameOf(Graph::current().time()));
    }
    mRuler->update();
}
} // namespace lila::ui
