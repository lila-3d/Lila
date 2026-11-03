#include "ui/ViewportToast.h"

#include "ops/Command.h"
#include "ui/Overlay.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace lila::ui::viewportToast
{
namespace
{
constexpr double kFadeMs = 400.0;
constexpr double kMargin = 12.0;
constexpr double kHeight = 26.0;
constexpr double kGap = 6.0;

struct Toast
{
    QString text;
    QPointer<QWidget> view;
    bool everyView = false;
    qint64 expires = 0;
    Kind kind = Kind::Info;
};

std::function<QWidget*()>& activeProvider()
{
    static std::function<QWidget*()> f;
    return f;
}

std::vector<Toast>& toasts()
{
    static std::vector<Toast> t;
    return t;
}

QElapsedTimer& clock()
{
    static QElapsedTimer c = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return c;
}

/** Redraws the viewports while a toast is up so it fades, then once more to clear the last one. */
QTimer& ticker()
{
    static QTimer* t = [] {
        auto* timer = new QTimer();
        timer->setInterval(33);
        QObject::connect(timer, &QTimer::timeout, [timer] {
            const qint64 now = clock().elapsed();
            std::erase_if(toasts(), [now](const Toast& t) { return t.expires <= now || (!t.everyView && !t.view); });
            if (toasts().empty())
                timer->stop();
            viewportTools::requestRedraw();
        });
        return timer;
    }();
    return *t;
}
} // namespace

void show(const QString& text, QWidget* view, int durationMs, Kind kind)
{
    const qint64 expires = clock().elapsed() + std::max(durationMs, 1);
    std::erase_if(toasts(), [&](const Toast& t) { return t.text == text && t.view == view && t.everyView == !view; });
    toasts().push_back({text, view, !view, expires, kind});
    if (!ticker().isActive())
        ticker().start();
    viewportTools::requestRedraw();
}

void showActive(const QString& text, int durationMs, Kind kind)
{
    show(text, activeProvider() ? activeProvider()() : nullptr, durationMs, kind);
}

void setActive(std::function<QWidget*()> active)
{
    activeProvider() = std::move(active);
}

void registerCommands()
{
    if (ops::find("viewportToast"))
        return;
    ops::registerCommand({
        .name = "viewportToast",
        .doc = "Pop up a short message in the bottom-right corner of the active viewport; it fades out after duration. "
               "Showing the same text again restarts it instead of adding another.",
        .positionalDoc = "text",
        .flags = {{.name = "all", .shortName = "a", .type = ArgType::Bool, .doc = "Show it in every viewport, not only the active one."},
                  {.name = "duration",
                   .shortName = "d",
                   .type = ArgType::Float,
                   .defaultValue = 2.0,
                   .doc = "Seconds before it is gone, including the fade.",
                   .min = 0.1,
                   .max = 60.0},
                  {.name = "error", .shortName = "e", .type = ArgType::Bool, .doc = "Mark it as a failure, with a red accent."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string() || a.positional[0].get<std::string>().empty())
                throw CommandError("viewportToast: text expected");
            const QString text = QString::fromStdString(a.positional[0].get<std::string>());
            const int ms = int(a.get<double>("duration") * 1000.0);
            const Kind kind = a.get<bool>("error") ? Kind::Error : Kind::Info;
            if (a.get<bool>("all"))
                show(text, nullptr, ms, kind);
            else
                showActive(text, ms, kind);
            return nullptr;
        },
        .examples = {"viewportToast(\"Exported 12 meshes\")", "viewportToast(\"Render done\", all=True, duration=4)"},
    });
}

void draw(OverlayPainter& p, const ViewInfo& info, const QWidget* view, const std::vector<QRectF>& occupied)
{
    const qint64 now = clock().elapsed();
    double bottom = info.height - kMargin;
    for (auto it = toasts().rbegin(); it != toasts().rend(); ++it)
    {
        if (it->expires <= now || (!it->everyView && it->view != view))
            continue;
        const float alpha = float(std::clamp((it->expires - now) / kFadeMs, 0.0, 1.0));
        const double w = OverlayPainter::textWidth(it->text) + 20.0;
        QRectF r(info.width - kMargin - w, bottom - kHeight, w, kHeight);
        for (bool moved = true; moved;)
        {
            moved = false;
            for (const QRectF& o : occupied)
                if (o.intersects(r))
                {
                    r.moveBottom(o.top() - kGap);
                    moved = true;
                }
        }
        p.rect(r, {0.08f, 0.08f, 0.08f, 0.82f * alpha});
        const bool error = it->kind == Kind::Error;
        p.rect(QRectF(r.left(), r.top(), 3.0, r.height()), error ? glm::vec4(1.0f, 0.35f, 0.3f, alpha) : glm::vec4(0.35f, 0.65f, 1.0f, alpha));
        p.text(QPointF(r.left() + 11.0, r.top() + 17.5), it->text, {0.95f, 0.95f, 0.95f, alpha});
        bottom = r.top() - kGap;
    }
}
} // namespace lila::ui::viewportToast
