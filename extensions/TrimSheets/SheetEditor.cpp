// The Trim Sheet Editor panel: the current sheet drawn over its bake, borders dragged to resize strips and rects (snapped
// to the mip grid, Shift for single pixels), trims split, deleted and retyped, and the selected trim's properties.

#include "TrimSheets.h"
#include "TrimBake.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Panels.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <filesystem>
#include <format>
#include <functional>
#include <set>

using namespace lila;
using namespace lila::ui;

namespace trims
{
namespace
{
using json = nlohmann::json;

const char* const kKindNames[] = {"stripU", "stripV", "rect", "radial", "unique", "flat"};

json edit(const json& call)
{
    try
    {
        return ops::execute("trimSheetEdit", call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
        return {};
    }
}

/** A trim border under the cursor: the line axis = at, grabbed at `along` on it. */
struct Border
{
    int axis = 1;
    int at = 0;
    int along = 0;
};

class SheetCanvas : public QWidget
{
public:
    std::function<void(int)> onSelect;
    /** A border moved or a trim split: those trims' rectangles changed. */
    std::function<void()> onLayout;

    explicit SheetCanvas(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumSize(200, 200);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { refresh(); });
        timer->start(1000);
        refresh();
    }

    const std::optional<Sheet>& sheet() const { return mSheet; }
    const std::string& path() const { return mPath; }
    int selected() const { return mSelected; }

    void select(int i)
    {
        mSelected = mSheet && i >= 0 && size_t(i) < mSheet->trims.size() ? i : -1;
        update();
        if (onSelect)
            onSelect(mSelected);
    }

    /** Reloads when the current sheet or its normal map changed on disk; force reloads anyway. */
    void refresh(bool force = false)
    {
        const std::string path = currentSheet();
        std::error_code ec;
        const auto stamp = [&](const std::filesystem::path& p) {
            return std::filesystem::exists(p, ec) ? std::filesystem::last_write_time(p, ec).time_since_epoch().count() : 0;
        };
        const long long sheetStamp = path.empty() ? 0 : stamp(path);
        const long long texStamp = mSheet ? stamp(mapFile(*mSheet, path, "normal")) : 0;
        if (!force && path == mPath && sheetStamp == mSheetStamp && texStamp == mTexStamp)
            return;
        const bool sameSheet = path == mPath;
        mPath = path;
        mSheetStamp = sheetStamp;
        mSheet.reset();
        mImage = QImage();
        if (!path.empty() && sheetStamp)
            try
            {
                mSheet = load(path);
                const std::filesystem::path normal = mapFile(*mSheet, path, "normal");
                mTexStamp = stamp(normal);
                if (mTexStamp)
                    mImage.load(QString::fromStdString(normal.generic_string()));
            }
            catch (const std::exception& e)
            {
                log::write(LogLevel::Warning, e.what());
            }
        select(sameSheet ? mSelected : -1);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        if (!mSheet)
        {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, "No trim sheet yet.\nMake one from the Trim Sheets section of the Level Toolkit.");
            return;
        }
        const QRectF area = sheetRect();
        if (!mImage.isNull())
            p.drawImage(area, mImage);
        else
            p.fillRect(area, palette().color(QPalette::Base));

        // The mip grid, while its lines are far enough apart to read.
        const double step = double(1 << std::clamp(mSheet->mipLevel, 0, 12)) * scale();
        if (step >= 6.0)
        {
            p.setPen(QColor(255, 255, 255, 24));
            for (double x = area.left(); x <= area.right(); x += step)
                p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            for (double y = area.top(); y <= area.bottom(); y += step)
                p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        }

        for (size_t i = 0; i < mSheet->trims.size(); ++i)
        {
            const QRectF r = trimRect(mSheet->trims[i]);
            const glm::vec3 c = trimColor(i);
            QColor col = QColor::fromRgbF(c.r, c.g, c.b);
            if (mImage.isNull())
                p.fillRect(r, col.darker(170));
            if (int(i) == mSelected)
            {
                QColor fill = col;
                fill.setAlphaF(0.3f);
                p.fillRect(r, fill);
            }
            col.setAlphaF(int(i) == mSelected ? 1.0f : 0.6f);
            p.setPen(QPen(col, int(i) == mSelected ? 2.0 : 1.0));
            p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
            if (r.height() >= 12 && r.width() >= 16)
            {
                p.setPen(Qt::white);
                const Trim& t = mSheet->trims[i];
                const QString label = r.width() > 120 ? QString::fromStdString(std::format("{}  {}  {}px", i, t.name, t.sizePx().y)) : QString::number(i);
                p.drawText(r.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
            }
        }

        if (mHover && !mDragging)
            drawBorder(p, *mHover, mHover->at, QColor(255, 255, 255, 160));
        if (mDragging)
        {
            drawBorder(p, mDrag, mDragTo, QColor(255, 210, 80));
            p.setPen(QColor(255, 210, 80));
            p.drawText(QRectF(area.left() + 4, area.bottom() - 18, area.width() - 8, 16), Qt::AlignRight,
                       QString::fromStdString(std::format("{} px{}", mDragTo, mDragTo % (1 << std::clamp(mSheet->mipLevel, 0, 12)) ? "  (off the mip grid)" : "")));
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!mSheet)
            return;
        const QPointF at = e->position();
        if (mDragging)
        {
            const glm::dvec2 px = toPx(at);
            const double v = mDrag.axis == 0 ? px.x : px.y;
            mDragTo = (e->modifiers() & Qt::ShiftModifier) ? int(std::lround(v)) : snapToGrid(*mSheet, v);
            update();
            return;
        }
        mHover = borderAt(at);
        setCursor(mHover ? (mHover->axis == 0 ? Qt::SplitHCursor : Qt::SplitVCursor) : Qt::ArrowCursor);
        update();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (!mSheet)
            return;
        if (e->button() == Qt::LeftButton)
        {
            if (const std::optional<Border> b = borderAt(e->position()))
            {
                mDrag = *b;
                mDragTo = b->at;
                mDragging = true;
                return;
            }
            select(trimAt(e->position()));
        }
        else if (e->button() == Qt::RightButton)
        {
            select(trimAt(e->position()));
            contextMenu(e->position(), e->globalPosition().toPoint());
        }
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (!mDragging || e->button() != Qt::LeftButton)
            return;
        mDragging = false;
        if (mDragTo != mDrag.at)
        {
            edit({{"moveBorder", {{"axis", mDrag.axis == 0 ? "x" : "y"}, {"at", mDrag.at}, {"along", mDrag.along}, {"to", mDragTo}}}});
            refresh(true);
            if (onLayout)
                onLayout();
        }
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (const int hit = trimAt(e->position()); hit >= 0 && e->button() == Qt::LeftButton)
            try
            {
                ops::execute("trimApply", {{"trim", hit}}, CallSource::UI);
            }
            catch (const std::exception& ex)
            {
                log::write(LogLevel::Warning, ex.what());
            }
    }

    void leaveEvent(QEvent*) override
    {
        mHover.reset();
        update();
    }

private:
    double scale() const
    {
        if (!mSheet)
            return 1.0;
        return std::min(double(width()) / std::max(1, mSheet->width), double(height()) / std::max(1, mSheet->height));
    }

    QRectF sheetRect() const
    {
        const double k = scale();
        const double w = mSheet->width * k, h = mSheet->height * k;
        return {(width() - w) / 2.0, (height() - h) / 2.0, w, h};
    }

    QRectF trimRect(const Trim& t) const
    {
        const QRectF a = sheetRect();
        const double k = scale();
        return {a.left() + t.min.x * k, a.top() + t.min.y * k, (t.max.x - t.min.x) * k, (t.max.y - t.min.y) * k};
    }

    glm::dvec2 toPx(QPointF at) const
    {
        const QRectF a = sheetRect();
        const double k = scale();
        return {(at.x() - a.left()) / k, (at.y() - a.top()) / k};
    }

    int trimAt(QPointF at) const
    {
        if (!mSheet)
            return -1;
        for (size_t i = 0; i < mSheet->trims.size(); ++i)
            if (trimRect(mSheet->trims[i]).contains(at))
                return int(i);
        return -1;
    }

    /** The inner trim border nearest the cursor, within a few screen pixels. */
    std::optional<Border> borderAt(QPointF at) const
    {
        if (!mSheet)
            return std::nullopt;
        const glm::dvec2 px = toPx(at);
        const double reach = 5.0 / scale();
        std::optional<Border> best;
        double bestDist = reach;
        const int extent[2] = {mSheet->width, mSheet->height};
        for (const Trim& t : mSheet->trims)
            for (int axis = 0; axis < 2; ++axis)
            {
                const int b = 1 - axis;
                if (px[b] < t.min[b] || px[b] > t.max[b])
                    continue;
                for (int line : {t.min[axis], t.max[axis]})
                {
                    if (line <= 0 || line >= extent[axis])
                        continue;
                    if (const double d = std::abs(px[axis] - line); d < bestDist)
                    {
                        bestDist = d;
                        best = Border{axis, line, int(px[b])};
                    }
                }
            }
        return best;
    }

    void drawBorder(QPainter& p, const Border& b, int to, QColor color) const
    {
        Sheet copy = *mSheet;
        const std::vector<size_t> run = moveBorder(copy, b.axis, b.at, b.along, b.at);
        const QRectF area = sheetRect();
        const double k = scale();
        p.setPen(QPen(color, 2.0));
        for (size_t i : run)
        {
            const Trim& t = mSheet->trims[i];
            if (b.axis == 0)
                p.drawLine(QPointF(area.left() + to * k, area.top() + t.min.y * k), QPointF(area.left() + to * k, area.top() + t.max.y * k));
            else
                p.drawLine(QPointF(area.left() + t.min.x * k, area.top() + to * k), QPointF(area.left() + t.max.x * k, area.top() + to * k));
        }
    }

    void contextMenu(QPointF at, QPoint global)
    {
        if (mSelected < 0)
            return;
        const int i = mSelected;
        const glm::dvec2 px = toPx(at);
        QMenu menu(this);
        menu.addAction("Map Selection Here", [i] {
            try
            {
                ops::execute("trimApply", {{"trim", i}}, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::write(LogLevel::Warning, e.what());
            }
        });
        menu.addSeparator();
        auto split = [this, i](int axis, int pos) {
            edit({{"split", {{"trim", i}, {"axis", axis == 0 ? "x" : "y"}, {"at", pos}}}});
            refresh(true);
            if (onLayout)
                onLayout();
        };
        const int sx = snapToGrid(*mSheet, px.x), sy = snapToGrid(*mSheet, px.y);
        menu.addAction(QString::fromStdString(std::format("Split Across at {} px", sy)), [split, sy] { split(1, sy); });
        menu.addAction(QString::fromStdString(std::format("Split Along at {} px", sx)), [split, sx] { split(0, sx); });
        menu.addAction("Delete Trim", [this, i] {
            edit({{"remove", i}});
            refresh(true);
            if (onLayout)
                onLayout();
        });
        menu.exec(global);
    }

    std::string mPath;
    long long mSheetStamp = 0;
    long long mTexStamp = 0;
    std::optional<Sheet> mSheet;
    QImage mImage;
    int mSelected = -1;
    std::optional<Border> mHover;
    bool mDragging = false;
    Border mDrag;
    int mDragTo = 0;
};

QWidget* makeEditor(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);

    auto* title = new QLabel(w);
    title->setWordWrap(true);
    v->addWidget(title);
    auto* canvas = new SheetCanvas(w);
    v->addWidget(canvas, 1);
    auto* warnings = new QLabel(w);
    warnings->setWordWrap(true);
    warnings->setStyleSheet("color: #e0a040");
    v->addWidget(warnings);

    // The selected trim.
    auto* props = new QWidget(w);
    auto* form = new QFormLayout(props);
    form->setContentsMargins(0, 0, 0, 0);
    auto* name = new QLineEdit(props);
    auto* kind = new QComboBox(props);
    for (const char* k : kKindNames)
        kind->addItem(k);
    auto* style = new QComboBox(props);
    for (const std::string& s : styleNames())
        style->addItem(QString::fromStdString(s));
    auto* bevel = new QDoubleSpinBox(props);
    bevel->setRange(0.0, 256.0);
    bevel->setSuffix(" px");
    auto* tags = new QLineEdit(props);
    tags->setPlaceholderText("wall, edge, floor...");
    auto* size = new QLabel(props);
    form->addRow("Name", name);
    form->addRow("Size", size);
    form->addRow("Kind", kind);
    form->addRow("Style", style);
    form->addRow("Bevel", bevel);
    form->addRow("Tags", tags);
    v->addWidget(props);

    auto* buttons = new QHBoxLayout();
    auto* bake = new QPushButton("Bake", w);
    auto* texture = new QPushButton("Texture", w);
    texture->setToolTip("Put the sheet's smart material on its texture set (trimSheetTexture).");
    auto* paint = new QPushButton("Paint", w);
    paint->setToolTip("Paint the sheet by hand on a flat preview plane.");
    buttons->addWidget(bake);
    buttons->addWidget(texture);
    buttons->addWidget(paint);
    v->addLayout(buttons);

    // Trims whose profile changed since the last bake rebake alone; a layout change rebakes everything.
    struct State
    {
        std::set<int> changed;
        bool layout = false;
        bool loading = false;
    };
    auto state = std::make_shared<State>();

    auto show = [=](int i) {
        state->loading = true;
        const std::optional<Sheet>& s = canvas->sheet();
        props->setEnabled(s && i >= 0);
        if (s)
        {
            title->setText(QString::fromStdString(std::format("<b>{}</b>  {} x {} px, {:g} px/m", s->name, s->width, s->height, s->pxPerM)));
            const std::vector<std::string> ws = validate(*s);
            std::string text;
            for (size_t k = 0; k < ws.size() && k < 3; ++k)
                text += (k ? "\n" : "") + ws[k];
            warnings->setText(QString::fromStdString(text));
            warnings->setVisible(!text.empty());
        }
        else
        {
            title->setText("No trim sheet");
            warnings->hide();
        }
        if (s && i >= 0)
        {
            const Trim& t = s->trims[size_t(i)];
            name->setText(QString::fromStdString(t.name));
            const glm::dvec2 world = s->worldSize(t);
            size->setText(QString::fromStdString(std::format("{} x {} px, {:.0f} x {:.0f} cm", t.sizePx().x, t.sizePx().y, world.x, world.y)));
            kind->setCurrentText(QString::fromStdString(t.toJson().value("kind", std::string("stripU"))));
            style->setCurrentText(QString::fromStdString(std::string(styleName(t.profile.style))));
            bevel->setValue(t.profile.bevelPx);
            std::string joined;
            for (const std::string& tag : t.tags)
                joined += (joined.empty() ? "" : ", ") + tag;
            tags->setText(QString::fromStdString(joined));
        }
        state->loading = false;
    };
    canvas->onSelect = show;
    canvas->onLayout = [=] {
        state->layout = true;
        show(canvas->selected());
    };

    auto apply = [=](const json& values) {
        const int i = canvas->selected();
        if (state->loading || i < 0)
            return;
        edit({{"trim", i}, {"values", values}});
        state->changed.insert(i);
        canvas->refresh(true);
    };
    QObject::connect(name, &QLineEdit::editingFinished, w, [=] { apply({{"name", name->text().toStdString()}}); });
    QObject::connect(kind, &QComboBox::currentTextChanged, w, [=](const QString& k) { apply({{"kind", k.toStdString()}}); });
    QObject::connect(style, &QComboBox::currentTextChanged, w, [=](const QString& st) { apply({{"profile", {{"style", st.toStdString()}}}}); });
    QObject::connect(bevel, &QDoubleSpinBox::editingFinished, w, [=] { apply({{"profile", {{"bevelPx", bevel->value()}}}}); });
    QObject::connect(tags, &QLineEdit::editingFinished, w, [=] {
        json list = json::array();
        for (const QString& t : tags->text().split(',', Qt::SkipEmptyParts))
            if (const QString tag = t.trimmed(); !tag.isEmpty())
                list.push_back(tag.toStdString());
        apply({{"tags", list}});
    });

    auto runUi = [](const char* command, const json& call) {
        try
        {
            ops::execute(command, call, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Warning, e.what());
        }
    };
    QObject::connect(bake, &QPushButton::clicked, w, [=] {
        json call = json::object();
        if (!state->layout && !state->changed.empty())
            call["trims"] = std::vector<int>(state->changed.begin(), state->changed.end());
        runUi("trimSheetBake", call);
        state->changed.clear();
        state->layout = false;
        canvas->refresh(true);
    });
    QObject::connect(texture, &QPushButton::clicked, w, [=] { runUi("trimSheetTexture", json::object()); });
    QObject::connect(paint, &QPushButton::clicked, w, [] { actions::run(json("trims.paint")); });
    show(-1);
    return w;
}
} // namespace

void registerEditor(ext::Context& ctx)
{
    panels::add(ctx, {.id = "trimSheetEditor",
                      .title = "Trim Sheet Editor",
                      .create = makeEditor,
                      .icon = "layout-rows",
                      .open = false,
                      .floatFirst = true,
                      .docs = "trim-sheets"});
}
} // namespace trims
