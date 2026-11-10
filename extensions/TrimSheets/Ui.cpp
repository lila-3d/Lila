// Level > Trim Sheets and the Level Toolkit's Trim Sheets section: the current sheet drawn as a palette (click a trim to
// map the selection onto it), new sheets from presets, bake, auto trim with cycling, shifting, edge trims and sending.

#include "TrimSheets.h"
#include "TrimBake.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Panels.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <filesystem>
#include <format>

using namespace lila;
using namespace lila::ui;

namespace trims
{
using json = nlohmann::json;

namespace
{
void run(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
}

/**
 * The selected meshes (and the meshes under selected groups) to bake as the current sheet's detail, leaving out the sheet's
 * own generated high and low.
 */
std::vector<std::string> selectedDetail()
{
    scene::Graph& g = scene::Graph::current();
    std::string sheet;
    try
    {
        sheet = load(currentSheet()).name;
    }
    catch (const std::exception&)
    {
    }
    // Meshes mapped onto a sheet are its users, not detail modelled on its high.
    auto mapped = [&](scene::NodeId id) {
        for (scene::NodeId at : {id, g.parent(id)})
            if (scene::Node* n = g.node(at))
                try
                {
                    const scene::PlugValue v = g.value(g.plug(n->path() + "." + kNodeSheet));
                    if (const std::string* p = std::get_if<std::string>(&v); p && !p->empty())
                        return true;
                }
                catch (const std::exception&)
                {
                }
        return false;
    };
    std::vector<std::string> out;
    auto add = [&](scene::NodeId id) {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || n->name() == sheet + "_High" || n->name() == sheet + "_Low" || mapped(id))
            return;
        if (std::ranges::find(out, n->path()) == out.end())
            out.push_back(n->path());
    };
    for (scene::NodeId id : g.selection())
    {
        add(id);
        for (scene::NodeId d : g.descendants(id))
            add(d);
    }
    return out;
}

/** How many times Cycle Trim has run since the last Auto Trim: each press takes the next-best match. */
int gCycle = 0;

/** The painted preview the Trim Sheet Editor keeps when the sheet is textured, else the baked normal map. */
std::filesystem::path shownFile(const Sheet& s, const std::filesystem::path& f)
{
    const std::filesystem::path painted = textureFolder(s, f) / "Preview" / ("TS_" + materialName(s).substr(2) + "_baseColor.png");
    return std::filesystem::exists(painted) ? painted : mapFile(s, f, "normal");
}

/** The current sheet as a picture with its trims outlined; clicking a trim maps the selected faces onto it. */
class SheetView : public QWidget
{
public:
    explicit SheetView(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumHeight(160);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { refresh(); });
        timer->start(1000);
        refresh();
    }

    QSize sizeHint() const override { return {240, 240}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return mSheet ? int(double(w) * mSheet->height / std::max(1, mSheet->width)) : w; }

    /** Reloads when the current sheet or its textures changed on disk. */
    void refresh()
    {
        const std::string path = currentSheet();
        std::error_code ec;
        const auto stamp = [&](const std::filesystem::path& p) {
            return std::filesystem::exists(p, ec) ? std::filesystem::last_write_time(p, ec).time_since_epoch().count() : 0;
        };
        const long long sheetStamp = path.empty() ? 0 : stamp(path);
        long long texStamp = 0;
        if (mSheet)
            texStamp = stamp(shownFile(*mSheet, path));
        if (path == mPath && sheetStamp == mSheetStamp && texStamp == mTexStamp)
            return;
        mPath = path;
        mSheetStamp = sheetStamp;
        mSheet.reset();
        mImage = QImage();
        if (!path.empty() && sheetStamp)
        {
            try
            {
                mSheet = load(path);
                const std::filesystem::path shown = shownFile(*mSheet, path);
                mTexStamp = stamp(shown);
                if (mTexStamp)
                    mImage.load(QString::fromStdString(shown.generic_string()));
            }
            catch (const std::exception& e)
            {
                log::write(LogLevel::Warning, e.what());
            }
        }
        updateGeometry();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QRect area = sheetRect();
        if (!mSheet)
        {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, "No trim sheet yet.\nPick a preset and press New.");
            return;
        }
        if (!mImage.isNull())
            p.drawImage(area, mImage);
        else
            p.fillRect(area, palette().color(QPalette::Base));
        for (size_t i = 0; i < mSheet->trims.size(); ++i)
        {
            const QRect r = trimRect(i);
            const glm::vec3 c = trimColor(i);
            QColor col = QColor::fromRgbF(c.r, c.g, c.b);
            if (mImage.isNull())
                p.fillRect(r, col.darker(160));
            col.setAlphaF(int(i) == mHover ? 1.0f : 0.55f);
            p.setPen(QPen(col, int(i) == mHover ? 2 : 1));
            p.drawRect(r.adjusted(0, 0, -1, -1));
            if (r.height() >= 12 && r.width() >= 16)
            {
                p.setPen(Qt::white);
                p.drawText(r.adjusted(3, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, QString::number(i));
            }
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int hit = trimAt(e->position().toPoint());
        if (hit != mHover)
        {
            mHover = hit;
            update();
        }
        if (hit >= 0 && mSheet)
        {
            const Trim& t = mSheet->trims[size_t(hit)];
            const glm::dvec2 w = mSheet->worldSize(t);
            QToolTip::showText(e->globalPosition().toPoint(),
                               QString::fromStdString(std::format("{}  {}: {} x {} px, {:.0f} x {:.0f} cm\nClick to map the selection onto it.", hit,
                                                                  t.name, t.sizePx().x, t.sizePx().y, w.x, w.y)),
                               this);
        }
    }

    void leaveEvent(QEvent*) override
    {
        mHover = -1;
        update();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const int hit = trimAt(e->position().toPoint());
        if (hit >= 0 && e->button() == Qt::LeftButton)
            run("trimApply", {{"trim", hit}});
    }

private:
    QRect sheetRect() const
    {
        if (!mSheet)
            return rect();
        const double aspect = double(mSheet->width) / std::max(1, mSheet->height);
        int w = width(), h = int(w / aspect);
        if (h > height())
        {
            h = height();
            w = int(h * aspect);
        }
        return {(width() - w) / 2, 0, w, h};
    }

    QRect trimRect(size_t i) const
    {
        const QRect a = sheetRect();
        const Trim& t = mSheet->trims[i];
        const double sx = double(a.width()) / mSheet->width, sy = double(a.height()) / mSheet->height;
        return QRect(QPoint(a.x() + int(t.min.x * sx), a.y() + int(t.min.y * sy)), QPoint(a.x() + int(t.max.x * sx) - 1, a.y() + int(t.max.y * sy) - 1));
    }

    int trimAt(QPoint at) const
    {
        if (!mSheet)
            return -1;
        for (size_t i = 0; i < mSheet->trims.size(); ++i)
            if (trimRect(i).contains(at))
                return int(i);
        return -1;
    }

    std::string mPath;
    long long mSheetStamp = 0;
    long long mTexStamp = 0;
    std::optional<Sheet> mSheet;
    QImage mImage;
    int mHover = -1;
};

QWidget* sectionWidget(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);

    auto* row = new QHBoxLayout();
    auto* presetBox = new QComboBox(w);
    for (const PresetInfo& p : presets())
    {
        presetBox->addItem(QString::fromStdString(p.label), QString::fromStdString(p.id));
        presetBox->setItemData(presetBox->count() - 1, QString::fromStdString(p.doc), Qt::ToolTipRole);
    }
    auto* make = new QPushButton("New", w);
    make->setToolTip("Make and bake a trim sheet from this preset (kept and reused if it already exists).");
    QObject::connect(make, &QPushButton::clicked, w, [presetBox] { run("trimSheetCreate", {{"preset", presetBox->currentData().toString().toStdString()}}); });
    row->addWidget(presetBox, 1);
    row->addWidget(make);
    v->addLayout(row);

    v->addWidget(new SheetView(w));
    return w;
}
} // namespace

void registerUi(ext::Context& ctx)
{
    struct Def
    {
        const char* id;
        const char* label;
        const char* command;
        json call;
        const char* doc;
        const char* python;
    };
    const Def defs[] = {
        {"trims.create", "New Trim Sheet", "trimSheetCreate", json{{"preset", "industrialMetal"}},
         "Make a trim sheet from a preset and bake it: generator, bake and material in one step.", "trimSheetCreate(preset='industrialMetal')"},
        {"trims.bake", "Bake Trim Sheet", "trimSheetBake", json::object(), "Rebake the current trim sheet's textures.", "trimSheetBake()"},
        {"trims.generate", "Show Trim Sheet High/Low", "trimSheetGenerate", json::object(),
         "Put the current sheet's generated high and low in the scene to look at or sculpt.", "trimSheetGenerate()"},
        {"trims.test", "Trim Test Blockout", "trimSheetTest", json::object(),
         "Put a test blockout (wall, beam, steps, pillar, slab) in the scene, auto trimmed onto the current sheet, to see how it reads.",
         "trimSheetTest()"},
        {"trims.auto", "Auto Trim", "trimAuto", json::object(),
         "Hotspot the selected faces or meshes onto the current trim sheet: each island goes to the trim that fits its shape and size.", "trimAuto()"},
        {"trims.live", "Live Trim", "trimLive", json::object(), "Keep the selected meshes trimmed as they are edited: adds a live hotspot modifier.",
         "trimLive()"},
        {"trims.check", "Check Trims", "trimCheck", json{{"showIssues", true}},
         "Check the selected meshes' trim mapping: density, alignment, orientation, stretch, readability and repetition, painted red where wrong.",
         "trimCheck(showIssues=True)"},
        {"trims.shiftUp", "Shift Trim Up", "trimShift", json{{"steps", -1}}, "Move the selected faces to the trim above.", "trimShift(steps=-1)"},
        {"trims.shiftDown", "Shift Trim Down", "trimShift", json{{"steps", 1}}, "Move the selected faces to the trim below.", "trimShift(steps=1)"},
        {"trims.edges", "Trim Edges", "trimEdges", json::object(),
         "Chamfer the selected edges, map the chamfers onto the sheet's edge trims and set weighted normals.", "trimEdges(width=1.0)"},
        {"trims.curve", "Trim Along Curve", "trimAlongCurve", json::object(),
         "Lay a tiling trim along the selected curves, with caps at the ends: kerbs, edging, skirting, cornices.", "trimAlongCurve(width=15)"},
        {"trims.decal", "Trim Decal", "trimDecal", json::object(), "Float a trim strip over the selected edges, its crest on the edge.",
         "trimDecal(width=2)"},
        {"trims.cut", "Trim Cut", "trimDecal", json{{"cut", true}}, "Carve a trim groove along the selected edges, its floor and walls on the trim.",
         "trimDecal(cut=True, depth=0.5)"},
        {"trims.decalSend", "Send Decals to Unreal", "trimDecalSend", json::object(),
         "Send the placed unique decals to Unreal as deferred decal actors with the generated decal material.", "trimDecalSend()"},
        {"trims.weightedNormals", "Weighted Normals", "polyWeightedNormals", json::object(),
         "Face-weighted normals: big faces stay flat beside chamfers and bevels.", "polyWeightedNormals()"},
        {"trims.texture", "Texture Trim Sheet", "trimSheetTexture", json::object(),
         "Put the sheet's smart material on its texture set: wear, dirt and chips follow the baked bevels on every mesh using it.",
         "trimSheetTexture(material='Rusty Iron')"},
        {"trims.export", "Export Trim Layout", "trimSheetExport", json::object(), "Write the current sheet's layout as a Valve .rect, SVG, ID map or JSON.",
         "trimSheetExport(format='rect')"},
        {"trims.send", "Send Trim Sheet to Unreal", "trimSheetSend", json::object(),
         "Send the sheet's textures, generate the trim master material and an instance, and put it on mapped meshes.", "trimSheetSend()"},
    };
    for (const Def& d : defs)
    {
        Action act{.id = d.id, .label = d.label, .category = "Trim Sheets", .doc = d.doc, .python = d.python, .command = d.command, .call = d.call};
        // Auto Trim starts a new cycle; Cycle Trim continues it.
        if (std::string_view(d.id) == "trims.auto")
            act.run = [] {
                gCycle = 0;
                run("trimAuto", json::object());
            };
        actions::add(ctx, std::move(act));
    }
    actions::add(ctx, {.id = "trims.paint",
                       .label = "Paint Trim Sheet",
                       .category = "Trim Sheets",
                       .run =
                           [] {
                               try
                               {
                                   ops::execute("trimSheetPaint", json::object(), CallSource::UI);
                               }
                               catch (const std::exception& e)
                               {
                                   log::write(LogLevel::Warning, e.what());
                                   return;
                               }
                               log::write(LogLevel::Info, "Painting the trim sheet itself: every mesh using it shows the strokes.");
                               actions::run(json("tex.paint"));
                           },
                       .doc = "Paint the current trim sheet by hand on a flat preview plane; strokes show on every mesh using the sheet.",
                       .python = "trimSheetPaint()"});
    actions::add(ctx, {.id = "trims.detail",
                       .label = "Bake Selection as Sheet Detail",
                       .category = "Trim Sheets",
                       .run =
                           [] {
                               const std::vector<std::string> detail = selectedDetail();
                               if (detail.empty())
                               {
                                   log::write(LogLevel::Warning, "Select the meshes you modelled on the sheet's high poly (Show Sheet High/Low); meshes "
                                                                 "mapped onto the sheet don't count.");
                                   return;
                               }
                               run("trimSheetBake", {{"high", detail}});
                           },
                       .doc = "Bake the selected meshes into the current sheet on top of its generated relief: carvings, motifs, a sculpt. The sheet "
                              "keeps them for every later bake.",
                       .python = "trimSheetBake(high=['Rosette', 'Dentil'])"});
    actions::add(ctx, {.id = "trims.detailClear",
                       .label = "Bake Without Detail",
                       .category = "Trim Sheets",
                       .run = [] { run("trimSheetBake", {{"high", json::array()}}); },
                       .doc = "Forget the sheet's detail meshes and bake its generated relief alone.",
                       .python = "trimSheetBake(high=[])"});
    actions::add(ctx, {.id = "trims.editor",
                       .label = "Trim Sheet Editor",
                       .category = "Trim Sheets",
                       .run = [] { panels::show("trimSheetEditor"); },
                       .doc = "Lay out the current sheet: drag borders to resize strips, split and retype trims, set bevels and tags.",
                       .python = "lila.ui.run_action('trims.editor')"});
    actions::add(ctx, {.id = "trims.cycle",
                       .label = "Cycle Trim",
                       .category = "Trim Sheets",
                       .run = [] { run("trimAuto", {{"cycle", ++gCycle}}); },
                       .doc = "Map the selection onto its next-best trims: press again to keep going.",
                       .python = "trimAuto(cycle=1)"});

    registerEditor(ctx);

    levelToolkit::add(ctx, {.id = "trims",
                            .title = "Trim Sheets",
                            .items = {{"trims.auto", "wand", "Auto", "Trim"},
                                      {"trims.cycle", "refresh", "Cycle"},
                                      {"trims.shiftUp", "arrow-up", "Up"},
                                      {"trims.shiftDown", "arrow-down", "Down"},
                                      {""},
                                      {"trims.edges", "border-sides", "Edges"},
                                      {"trims.decal", "sticker", "Decal"},
                                      {"trims.curve", "route", "Curve"},
                                      {"trims.editor", "layout-rows", "Edit"},
                                      {"trims.test", "box", "Test"},
                                      {"trims.bake", "flame", "Bake"},
                                      {"trims.texture", "brush", "Texture"},
                                      {"trims.paint", "paint", "Paint"},
                                      {"trims.send", "send", "Unreal"}},
                            .create = sectionWidget,
                            .order = 60, .docs = "trim-sheets"});

    menus::addItem(ctx, {.id = "trims.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Trim Sheets"});
    const char* order[] = {"trims.create", "trims.test", "trims.editor", "trims.bake", "trims.generate", "trims.detail", "trims.detailClear", "trims.texture", "trims.paint", "", "trims.auto", "trims.cycle", "trims.shiftUp", "trims.shiftDown",
                           "trims.edges", "trims.decal", "trims.cut", "trims.curve", "trims.weightedNormals", "", "trims.export", "trims.send"};
    int sep = 0;
    for (const char* a : order)
    {
        if (*a == '\0')
            menus::addItem(ctx, {.id = std::format("trims.menu.sep{}", sep++), .menu = "Level/Trim Sheets", .kind = menus::Kind::Separator});
        else
            menus::addItem(ctx, {.id = std::string("trims.menu.") + a, .menu = "Level/Trim Sheets", .action = a});
    }
}
} // namespace trims
