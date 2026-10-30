#include "ui/UvEditorInternal.h"

namespace lila::ui
{
namespace
{
std::function<void()>& opener()
{
    static std::function<void()> fn;
    return fn;
}

/** Arrows and the wheel step through power-of-two texture sizes; any other size can still be typed. */
class TextureSizeBox : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

    void stepBy(int steps) override
    {
        int v = value();
        for (; steps > 0; --steps)
            v = int(std::bit_floor(unsigned(v))) * 2;
        for (; steps < 0; ++steps)
            v = std::has_single_bit(unsigned(v)) ? v / 2 : int(std::bit_floor(unsigned(v)));
        setValue(v);
    }
};

/** "vertex", "edge" or "face" switch the component mode; "island" is face mode with island select. */
void setPickMode(const std::string& m)
{
    const bool island = m == "island";
    if (!island)
        uvLive::setIslandSelect(false);
    UvView::safeRun("selectMode", {{"args", {island ? std::string("face") : m}}}, CallSource::UI);
    if (island)
        uvLive::setIslandSelect(true);
}
} // namespace

namespace
{
QToolButton* button(const QString& text, const QString& tip, QWidget* parent, const char* iconName = nullptr)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(false);
    if (iconName)
    {
        b->setIcon(icon(iconName));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    return b;
}

QDoubleSpinBox* spin(double lo, double hi, double value, int decimals, QWidget* parent)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setValue(value);
    s->setKeyboardTracking(false);
    // The hint fits the range's widest value; typical values are far shorter, so a narrow editor may squeeze the box.
    s->setMinimumWidth(s->minimumSizeHint().width() * 2 / 3);
    return s;
}
} // namespace

UvEditor::UvEditor(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    // Tools and display.
    auto* row1 = new FixedRowLayout();
    auto* tools = new QButtonGroup(this);
    tools->setExclusive(true);
    const std::tuple<const char*, const char*, UvView::Tool> toolDefs[] = {{"Select", "pointer", UvView::Tool::Select},
                                                                          {"Move", "arrows-move", UvView::Tool::Move},
                                                                          {"Rotate", "rotate", UvView::Tool::Rotate},
                                                                          {"Scale", "resize", UvView::Tool::Scale},
                                                                          {"Brush", "brush", UvView::Tool::Brush}};
    std::vector<QToolButton*> toolButtons;
    for (auto [name, ic, t] : toolDefs)
    {
        QToolButton* b = button(name, QString(name) + " uvs (Q W E R in the view)", this, ic);
        b->setCheckable(true);
        tools->addButton(b);
        row1->addWidget(b);
        toolButtons.push_back(b);
        connect(b, &QToolButton::clicked, this, [this, t] { mView->setTool(t); });
    }
    toolButtons.front()->setChecked(true);
    toolButtons.back()->setToolTip("Brush the uvs with the brush picked in the live row. B or F held + drag: size, M + drag: strength, "
                                   "Ctrl inverts, Shift relaxes. A click with the pin brushes toggles one pin");
    auto* modes = new QButtonGroup(this);
    modes->setExclusive(true);
    const std::tuple<const char*, const char*, const char*, const char*> modeDefs[] = {
        {"Vertex", "vertex", "Select uv points (F9 in the view)", "point"},
        {"Edge", "edge", "Select uv edges (F10 in the view)", "line"},
        {"Face", "face", "Select uv faces (F11 in the view)", "square"},
        {"Island", "island", "Select whole uv islands (F12 in the view)", "shape"}};
    std::vector<QToolButton*> modeButtons;
    for (auto [label, mode, tip, ic] : modeDefs)
    {
        QToolButton* b = button(label, tip, this, ic);
        b->setCheckable(true);
        modes->addButton(b);
        row1->addWidget(b);
        modeButtons.push_back(b);
        connect(b, &QToolButton::clicked, this, [m = std::string(mode)] { setPickMode(m); });
    }
    auto showMode = [modes, modeButtons] {
        ComponentMode m = scene::components::mode();
        if (uvLive::islandSelect() && m != ComponentMode::Face)
            uvLive::setIslandSelect(false);
        int i = uvLive::islandSelect() ? 3 : m == ComponentMode::Vertex ? 0 : m == ComponentMode::Edge ? 1 : m == ComponentMode::Face ? 2 : -1;
        modes->setExclusive(false);
        for (size_t k = 0; k < modeButtons.size(); ++k)
            modeButtons[k]->setChecked(int(k) == i);
        modes->setExclusive(true);
    };
    showMode();
    QToolButton* frameBtn = button("Frame", "Frame the selection (F), or everything (A)", this, "focus-centered");
    row1->addWidget(frameBtn);
    row1->endRow();
    auto toggle = [this](const QString& text, const QString& tip, const char* ic, bool on) {
        QToolButton* b = button(text, tip, this, ic);
        b->setCheckable(true);
        b->setChecked(on);
        return b;
    };
    mDisplay = new QComboBox(this);
    mDisplay->addItems({"Plain", "Checker", "Distortion", "Stretch", "Islands"});
    mDisplay->setCurrentIndex(1);
    mDisplay->setToolTip("Distortion shades distorted faces brighter; Stretch shades stretched faces light and compressed ones dark; "
                         "Islands shades each island apart");
    row1->addWidget(mDisplay);
    QToolButton* udim = toggle("UDIM", "Show the UDIM tiles", "grid-4x4", false);
    row1->addWidget(udim);
    QToolButton* twoColour = toggle("2-colour", "Stretch display in two colours instead of grayscale", "palette", uvLive::twoColourHeatmap());
    row1->addWidget(twoColour);
    // A texture set channel under the UVs, and the texture paint brush in UV space.
    auto* texture = new QToolButton(this);
    texture->setText("Texture");
    texture->setIcon(icon("photo"));
    texture->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    texture->setPopupMode(QToolButton::InstantPopup);
    texture->setToolTip("Show a texture set channel under the UVs (drawn on the GPU at the view's resolution)");
    auto* textureMenu = new QMenu(texture);
    texture->setMenu(textureMenu);
    connect(textureMenu, &QMenu::aboutToShow, this, [this, textureMenu, texture] {
        textureMenu->clear();
        textureMenu->addAction("None", this, [this, texture] {
            mView->texturePaint().setChannel({}, {});
            texture->setText("Texture");
        });
        for (const auto& [set, channel] : UvTexturePaint::choices())
        {
            const QString label = QString::fromStdString(set + " / " + channel);
            textureMenu->addAction(label, this, [this, texture, set, channel, label] {
                mView->texturePaint().setChannel(set, channel);
                texture->setText(texture->fontMetrics().elidedText(label, Qt::ElideMiddle, 160));
            });
        }
    });
    row1->addWidget(texture);
    QToolButton* paint = toggle("Paint", "Paint the shown texture set in UV space with the Texture Paint tool's brush and channels", "brush", false);
    connect(paint, &QToolButton::toggled, this, [this](bool on) {
        mView->texturePaint().setPainting(on);
        mView->update();
    });
    row1->addWidget(paint);
    row1->endRow();
    row1->addWidget(new QLabel("Set", this));
    mSet = new QComboBox(this);
    mSet->setEditable(true);
    mSet->setMinimumContentsLength(4);
    mSet->setToolTip("UV set the editor and tools work on (uvSetCurrent)");
    row1->addWidget(mSet);
    QToolButton* children = toggle("Children", "Show and edit the uvs of meshes under the selected objects too, not only the selected meshes",
                                   "hierarchy", uv::includeChildren());
    row1->addWidget(children);
    QToolButton* checker = toggle("3D checker", "Checker texture on the meshes in the viewport", "chess", uvLive::checker3d());
    row1->addWidget(checker);
    auto* tiling = new QSpinBox(this);
    tiling->setRange(1, 256);
    tiling->setValue(uvLive::checkerTiling());
    tiling->setKeyboardTracking(false);
    tiling->setToolTip("Checks across one 0-1 tile, in the viewport and the editor's checker display");
    row1->addWidget(tiling);
    QToolButton* seams = toggle("3D seams", "Draw the uv seams over the meshes in the viewport", "line-dashed", uvLive::seams3d());
    row1->addWidget(seams);
    root->addLayout(row1);

    // Operations, one row per stage: unwrap, edit, pack.
    auto* row2 = new FixedRowLayout();
    auto addOp = [&](const char* ic, const QString& text, const QString& tip, std::function<void()> fn) {
        QToolButton* b = button(text, tip, this, ic);
        connect(b, &QToolButton::clicked, this, std::move(fn));
        row2->addWidget(b);
        return b;
    };
    auto addMenuOp = [&](const char* ic, const QString& text, const QString& tip,
                         std::initializer_list<std::tuple<const char*, const char*, json>> items) {
        QToolButton* b = addOp(ic, text, tip, [] {});
        auto* menu = new QMenu(b);
        for (const auto& [label, command, call] : items)
        {
            std::string c = command;
            json j = call;
            menu->addAction(label, this, [this, c, j] { runCommand(c.c_str(), j); });
        }
        b->setMenu(menu);
        b->setPopupMode(QToolButton::InstantPopup);
        setRightClickAction(b, menu->actions().front());
    };
    auto packCall = [this] { return json{{"resolution", mResolution->value()}, {"padding", mPadding->value()}}; };
    addOp("wand", "Auto", "Seams, unwrap and pack in one step (uvAuto)", [this, packCall] { runCommand("uvAuto", packCall()); });
    addOp("line-dashed", "Seams", "Place seams automatically (uvAutoSeams)", [this] { runCommand("uvAutoSeams"); });
    addMenuOp("route", "Seams 2", "Auto Seams 2: seams and stretch optimised together, short and hidden (uvAutoSeams method=optcuts)",
              {{"Auto Seams 2", "uvAutoSeams", json{{"method", "optcuts"}}},
               {"Auto Seams 2, tighter (bound 0.02)", "uvAutoSeams", json{{"method", "optcuts"}, {"distortionBound", 0.02}}},
               {"Auto Seams 2, looser (bound 0.1)", "uvAutoSeams", json{{"method", "optcuts"}, {"distortionBound", 0.1}}}});
    addOp("cube-unfolded", "Unwrap", "Unwrap along the seams (uvUnwrap)", [this] { runCommand("uvUnwrap"); });
    addMenuOp("circle-dashed", "BFF", "Boundary First Flattening: conformal unwrap with the boundary chosen first (uvBff)",
              {{"Free boundary (least area stretch)", "uvBff", json::object()},
               {"Disk", "uvBff", json{{"boundary", "disk"}}},
               {"Rectangle (selected or sharpest corners)", "uvBff", json{{"boundary", "rectangle"}}},
               {"Polygon (selected corners)", "uvBff", json{{"boundary", "polygon"}}},
               {"Free with 4 cones", "uvBff", json{{"cones", 4}}},
               {"Free with 8 cones", "uvBff", json{{"cones", 8}}}});
    addMenuOp("flip-horizontal", "Symmetric", "Unwrap one side and mirror it to the other (uvUnwrapSymmetric)",
              {{"Mirror flipped", "uvUnwrapSymmetric", json::object()}, {"Mirror stacked", "uvUnwrapSymmetric", json{{"mode", "stack"}}}});
    QToolButton* projectBtn = addOp("cube-send", "Project", "Planar, box, cylindrical or spherical projection (uvProject)", [] {});
    auto* projectMenu = new QMenu(projectBtn);
    for (auto [label, type, axis] : {std::tuple{"Planar (fit)", "planar", "best"}, {"Planar X", "planar", "x"}, {"Planar Y", "planar", "y"},
                                     {"Planar Z", "planar", "z"}, {"Box", "box", "best"}, {"Cylindrical", "cylindrical", "best"},
                                     {"Spherical", "spherical", "best"}})
    {
        std::string t = type, a = axis;
        projectMenu->addAction(label, this, [this, t, a] { runCommand("uvProject", {{"type", t}, {"axis", a}}); });
    }
    projectBtn->setMenu(projectMenu);
    projectBtn->setPopupMode(QToolButton::InstantPopup);
    setRightClickAction(projectBtn, projectMenu->actions().front());
    addOp("bulb", "Lightmap", "Generate Unreal lightmap uvs in uv1 (uvLightmap)", [this] { runCommand("uvLightmap"); });
    row2->endRow();
    // Cut and sew re-solve the islands they touch straight away in live mode.
    auto cutOrSew = [this](const char* command, json call) {
        json r = uvLive::cutOrSew(command, std::move(call));
        if (!r.is_null())
            mStatus->setText(QString(command) + ": " + QString::fromStdString(r.dump()));
        mView->invalidate();
    };
    addOp("scissors", "Cut", "Cut the uvs along the selected edges and let the cut open (uvCut)", [cutOrSew] { cutOrSew("uvCut", json::object()); });
    addOp("needle-thread", "Sew", "Move and Sew: weld the selected seams, the smaller island travelling to meet the other (uvSew). In edge mode you "
                 "can also drag a border edge onto its partner",
          [cutOrSew] { cutOrSew("uvSew", json{{"move", true}}); });
    addOp("link", "Weld", "Weld the selected seams where they are, both sides meeting halfway (uvSew move=False)",
          [cutOrSew] { cutOrSew("uvSew", json{{"move", false}}); });
    addOp("layout-grid", "Straighten", "Rectify islands into grids and rectangles (uvStraighten)", [this] { runCommand("uvStraighten"); });
    addMenuOp("align-left", "Align", "Straighten selected edges, align island sides to U or V (uvStraightenEdges, uvAlignToEdge)",
              {{"Straighten edges", "uvStraightenEdges", json::object()},
               {"Align side to U", "uvAlignToEdge", json{{"axis", "u"}}},
               {"Align side to V", "uvAlignToEdge", json{{"axis", "v"}}},
               {"Orient by edge", "uvAlignToEdge", json::object()}});
    addOp("arrow-up-circle", "Orient", "Turn islands upright (uvOrient)", [this] { runCommand("uvOrient"); });
    addMenuOp("vector-triangle", "Optimize", "Relax islands from where they are, keeping angles, areas or a blend (uvOptimize, uvMosaic)",
              {{"Conformal (angles)", "uvOptimize", json{{"mode", "conformal"}}},
               {"Authalic (areas)", "uvOptimize", json{{"mode", "authalic"}}},
               {"Blend", "uvOptimize", json{{"mode", "blend"}}},
               {"Isometric (SLIM)", "uvOptimize", json{{"mode", "isometric"}}},
               {"Mosaic: split stretched islands", "uvMosaic", json::object()}});
    row2->endRow();

    // Packing with its texture size and padding, then texel density.
    addOp("packages", "Pack", "Pack islands with padding for the resolution and the Pack options (uvPack)", [this] { runPack(); });
    QToolButton* packOptions = addOp("adjustments", "Pack Options", "Show the Pack options: rotation, scale, stacking, tiles, groups, effort", [] {});
    packOptions->setCheckable(true);
    row2->addWidget(new QLabel("Res", this));
    mResolution = new TextureSizeBox(this);
    mResolution->setRange(16, 16384);
    mResolution->setValue(1024);
    mResolution->setToolTip("Texture size for packing and texel density, px");
    row2->addWidget(mResolution);
    row2->addWidget(new QLabel("Pad", this));
    mPadding = spin(0.0, 64.0, 4.0, 1, this);
    mPadding->setToolTip("Texels between islands");
    row2->addWidget(mPadding);
    row2->endRow();
    addOp("info-circle", "Info", "Islands, overlaps, distortion and density (uvLayout)", [this] { showLayout(); });
    row2->addWidget(new QLabel("Density", this));
    mDensity = spin(0.0, 100000.0, 10.24, 3, this);
    mDensity->setToolTip("Texel density, px/cm");
    row2->addWidget(mDensity);
    QToolButton* getTd = button("Get", "Measure texel density of the selection", this, "color-picker");
    QToolButton* setTd = button("Set", "Scale each selected island to the density", this, "ruler-measure");
    row2->addWidget(getTd);
    row2->addWidget(setTd);
    root->addLayout(row2);

    mPackPanel = new UvPackPanel(this);
    mPackPanel->setVisible(false);
    root->addWidget(mPackPanel);
    mPackPanel->onPack = [this] { runPack(); };
    mPackPanel->onShowStacks = [this](bool on) { mView->setShowStacks(on); };
    connect(packOptions, &QToolButton::toggled, mPackPanel, &QWidget::setVisible);

    // Live solving: the mode, one-shot solves, the stretch/compress balance, brushes, pins and the 3D display.
    auto* row5 = new FixedRowLayout();
    auto* live = button("Live", "Live mode: drag uvs and the island relaxes around them (Ctrl on release pins them); cuts, sews and pins "
                                "re-solve as they happen",
                        this, "bolt");
    live->setCheckable(true);
    live->setChecked(uvLive::liveMode());
    row5->addWidget(live);
    QToolButton* relaxBtn = button("Relax", "Relax the selected islands (or all) live until they settle", this, "ripple");
    QToolButton* flattenBtn = button("Flatten", "Solve the selected islands from scratch, keeping pins, size and placement, then relax live", this,
                                    "arrows-diagonal-minimize-2");
    QToolButton* settleBtn = button("Settle", "Relax to convergence and finish with a fold-free SLIM pass", this, "circle-check");
    row5->addWidget(relaxBtn);
    row5->addWidget(flattenBtn);
    row5->addWidget(settleBtn);
    row5->addWidget(new QLabel("Squash", this));
    auto* balance = new QSlider(Qt::Horizontal, this);
    balance->setRange(-100, 100);
    balance->setValue(int(std::lround(uvLive::balance() * 100.0)));
    balance->setMinimumWidth(30);
    balance->setMaximumWidth(90);
    balance->setToolTip("Stretch against compression: right tolerates stretch and fixes compression, left the reverse, middle treats both "
                        "alike");
    row5->addWidget(balance);
    row5->addWidget(new QLabel("Stretch", this));
    row5->endRow();
    auto* brushCombo = new QComboBox(this);
    for (int i = 0; i <= int(uv::LiveBrush::Soften); ++i)
    {
        std::string_view n = uv::liveBrushName(uv::LiveBrush(i));
        brushCombo->addItem(QString::fromUtf8(n.data(), qsizetype(n.size())));
    }
    brushCombo->setCurrentIndex(int(uvLive::brush().brush));
    brushCombo->setToolTip("Brush for the Brush tool here and the UV brush on the mesh (uv.brush3d)");
    row5->addWidget(brushCombo);
    QToolButton* brush3d = button("3D", "Paint the brush on the mesh in the viewport (uv.brush3d)", this, "cube");
    row5->addWidget(brush3d);
    QToolButton* pinBtn = button("Pin", "Pin the selected uvs (P pins the one under the cursor)", this, "pin");
    QToolButton* unpinBtn = button("Unpin", "Unpin the selected uvs", this, "pinned-off");
    row5->addWidget(pinBtn);
    row5->addWidget(unpinBtn);
    root->addLayout(row5);
    connect(live, &QToolButton::toggled, this, [](bool on) { uvLive::setLiveMode(on); });
    connect(relaxBtn, &QToolButton::clicked, this, [] { uvLive::run(uvLive::Op::Relax); });
    connect(flattenBtn, &QToolButton::clicked, this, [] { uvLive::run(uvLive::Op::Flatten); });
    connect(settleBtn, &QToolButton::clicked, this, [] { uvLive::run(uvLive::Op::Settle); });
    connect(balance, &QSlider::valueChanged, this, [](int v) { uvLive::setBalance(v / 100.0); });
    connect(brushCombo, &QComboBox::currentIndexChanged, this, [this, toolButtons](int i) {
        uvLive::brush().brush = uv::LiveBrush(std::clamp(i, 0, int(uv::LiveBrush::Soften)));
        uvLive::settingsChanged();
        toolButtons.back()->setChecked(true);
        mView->setTool(UvView::Tool::Brush);
    });
    connect(brush3d, &QToolButton::clicked, this, [] { uvLive::startBrushTool3d(); });
    connect(pinBtn, &QToolButton::clicked, this, [this] { runCommand("uvPin"); });
    connect(unpinBtn, &QToolButton::clicked, this, [this] { runCommand("uvUnpin"); });
    connect(checker, &QToolButton::toggled, this, [](bool on) { uvLive::setChecker3d(on); });
    connect(tiling, &QSpinBox::valueChanged, this, [this](int v) {
        uvLive::setCheckerTiling(v);
        mView->update();
    });
    connect(seams, &QToolButton::toggled, this, [](bool on) { uvLive::setSeams3d(on); });
    connect(children, &QToolButton::toggled, this, [](bool on) {
        uv::setIncludeChildren(on);
        uvLive::settingsChanged();
        viewportTools::requestRedraw();
    });
    connect(twoColour, &QToolButton::toggled, this, [this](bool on) {
        uvLive::setTwoColourHeatmap(on);
        mView->update();
    });
    mLiveListener = uvLive::addListener([this, live, brushCombo, checker, tiling, seams, children, showMode] {
        QSignalBlocker b1(live), b2(brushCombo), b3(checker), b4(seams), b5(tiling), b6(children);
        children->setChecked(uv::includeChildren());
        live->setChecked(uvLive::liveMode());
        brushCombo->setCurrentIndex(int(uvLive::brush().brush));
        checker->setChecked(uvLive::checker3d());
        tiling->setValue(uvLive::checkerTiling());
        showMode();
        seams->setChecked(uvLive::seams3d());
        mView->update();
    });

    mView = new UvView(this);
    root->addWidget(mView, 1);

    // Numeric transforms.
    auto* row4 = new FixedRowLayout();
    mMoveU = spin(-100.0, 100.0, 0.0, 4, this);
    mMoveV = spin(-100.0, 100.0, 0.0, 4, this);
    mRotate = spin(-360.0, 360.0, 90.0, 2, this);
    mScaleU = spin(-100.0, 100.0, 1.0, 4, this);
    mScaleV = spin(-100.0, 100.0, 1.0, 4, this);
    row4->addWidget(new QLabel("U", this));
    row4->addWidget(mMoveU);
    row4->addWidget(new QLabel("V", this));
    row4->addWidget(mMoveV);
    QToolButton* moveBtn = button("Move", "Move the selected uvs by U, V", this, "arrows-move");
    row4->addWidget(moveBtn);
    row4->endRow();
    row4->addWidget(mRotate);
    QToolButton* rotBtn = button("Rotate", "Rotate the selected uvs, degrees counter-clockwise", this, "rotate");
    row4->addWidget(rotBtn);
    std::vector<QToolButton*> iconOnly;
    for (auto [ic, text, tip, call] : {std::tuple{"rotate-2", "Rotate 90 Left", "Rotate 90 left: each selected island a quarter turn counter-clockwise",
                                                  json{{"rotate", 90.0}, {"perIsland", true}}},
                                       {"rotate-clockwise-2", "Rotate 90 Right", "Rotate 90 right: each selected island a quarter turn clockwise",
                                        json{{"rotate", -90.0}, {"perIsland", true}}},
                                       {"flip-horizontal", "Flip Horizontal", "Flip horizontal: mirror each selected island left to right",
                                        json{{"scale", {-1.0, 1.0}}, {"perIsland", true}}},
                                       {"flip-vertical", "Flip Vertical", "Flip vertical: mirror each selected island top to bottom",
                                        json{{"scale", {1.0, -1.0}}, {"perIsland", true}}}})
    {
        QToolButton* b = button(text, tip, this, ic);
        connect(b, &QToolButton::clicked, this, [this, call] { runCommand("uvTransform", call); });
        iconOnly.push_back(b);
    }
    row4->addWidget(iconOnly[0]);
    row4->addWidget(iconOnly[1]);
    row4->endRow();
    row4->addWidget(mScaleU);
    row4->addWidget(mScaleV);
    QToolButton* scaleBtn = button("Scale", "Scale the selected uvs about their centre", this, "resize");
    row4->addWidget(scaleBtn);
    row4->addWidget(iconOnly[2]);
    row4->addWidget(iconOnly[3]);
    iconOnly.insert(iconOnly.end(), {moveBtn, rotBtn, scaleBtn});
    for (QToolButton* b : iconOnly)
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    root->addLayout(row4);

    mStatus = new QLabel(this);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Command results can be one long line; without this the dock could not be narrower than the last one.
    mStatus->setWordWrap(true);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(mStatus);

    mView->onToolChanged = [toolButtons](UvView::Tool t) { toolButtons[size_t(t)]->setChecked(true); };
    connect(frameBtn, &QToolButton::clicked, this, [this] { mView->frameSelection(); });
    connect(mDisplay, &QComboBox::currentIndexChanged, this, [this](int i) { mView->setDisplay(UvView::Display(i)); });
    connect(udim, &QToolButton::toggled, this, [this](bool on) { mView->setShowUdim(on); });
    connect(mSet, &QComboBox::textActivated, this, [this](const QString& s) {
        if (!s.isEmpty())
            UvView::safeRun("uvSetCurrent", {{"args", {s.toStdString()}}}, CallSource::UI);
        mView->invalidate();
    });
    connect(getTd, &QToolButton::clicked, this, [this] {
        json r = runCommand("uvTexelDensity", {{"resolution", mResolution->value()}});
        if (r.is_object() && !r.empty())
            r = r.begin().value();
        if (r.is_number())
            mDensity->setValue(r.get<double>());
    });
    connect(setTd, &QToolButton::clicked, this,
            [this] { runCommand("uvTexelDensity", {{"resolution", mResolution->value()}, {"density", mDensity->value()}}); });
    connect(moveBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"translate", {mMoveU->value(), mMoveV->value()}}}); });
    connect(rotBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"rotate", mRotate->value()}}); });
    connect(scaleBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"scale", {mScaleU->value(), mScaleV->value()}}}); });

    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) {
        mView->invalidate();
        refreshSets();
    });
    mComponentListener = scene::components::addListener([this, showMode] {
        showMode();
        mView->update();
    });
    refreshSets();
}

UvEditor::~UvEditor()
{
    uvLive::setEditorShown(this, false);
    scene::Graph::current().removeListener(mGraphListener);
    scene::components::removeListener(mComponentListener);
    uvLive::removeListener(mLiveListener);
}

void UvEditor::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    uvLive::setEditorShown(this, true);
}

void UvEditor::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    uvLive::setEditorShown(this, false);
}

json UvEditor::runCommand(const char* command, const json& call)
{
    json r;
    try
    {
        r = ops::execute(command, call, CallSource::UI);
        actions::remember({{"command", command}, {"args", call}});
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    if (!r.is_null())
    {
        std::string text = r.dump();
        if (text.size() > 200)
            text = text.substr(0, 197) + "...";
        mStatus->setText(QString(command) + ": " + QString::fromStdString(text));
    }
    mView->invalidate();
    return r;
}

void UvEditor::refreshSets()
{
    std::set<std::string> names{uv::currentUvSet(), "uv0", "uv1"};
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : UvView::shownMeshes())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            if (scene::MeshPtr m = scene::modifiers::cage(g, id))
                for (const std::string& name : m->cornerAttrs().names())
                    if (const geo::Attribute* a = m->cornerAttrs().find(name); a && a->type() == geo::AttrType::Float2)
                        names.insert(name);
    QStringList items;
    for (const std::string& n : names)
        items << QString::fromStdString(n);
    QString cur = QString::fromStdString(uv::currentUvSet());
    QStringList existing;
    for (int i = 0; i < mSet->count(); ++i)
        existing << mSet->itemText(i);
    if (existing != items)
    {
        QSignalBlocker block(mSet);
        mSet->clear();
        mSet->addItems(items);
    }
    if (mSet->currentText() != cur)
    {
        QSignalBlocker block(mSet);
        mSet->setCurrentText(cur);
    }
}

void UvEditor::runPack()
{
    json call = mPackPanel->options();
    call["resolution"] = mResolution->value();
    call["padding"] = mPadding->value();
    setUvPackOptions(call);
    json r = runCommand("uvPack", call);
    mPackPanel->showResult(r);
}

void UvEditor::showLayout()
{
    json r = UvView::safeRun("uvLayout", {{"resolution", mResolution->value()}}, CallSource::Script);
    if (r.is_null())
        return;
    auto line = [](const std::string& name, const json& j) {
        if (!j.value("exists", true))
            return QString::fromStdString(name + ": no " + j.value("uvSet", std::string("uv")) + " set");
        return QString::fromStdString(name) + QString(": %1 islands, %2 overlapping, %3 folded, stretch %4 / %5, %6 px/cm")
                                                  .arg(j.value("islands", 0))
                                                  .arg(j.value("overlappingFaces", 0))
                                                  .arg(j.value("foldedFaces", 0))
                                                  .arg(j["areaDistortion"].value("mean", 0.0), 0, 'f', 3)
                                                  .arg(j["angleDistortion"].value("mean", 0.0), 0, 'f', 3)
                                                  .arg(j.value("texelDensity", 0.0), 0, 'f', 2);
    };
    QStringList lines;
    if (r.contains("islands"))
        lines << line("selection", r);
    else
        for (auto& [name, j] : r.items())
            lines << line(name, j);
    mStatus->setText(lines.join("\n"));
    log::info("{}", lines.join("; ").toStdString());
}

void setUvEditorOpener(std::function<void()> open)
{
    opener() = std::move(open);
}

void registerUvActions()
{
    registerUvPackActions();
    installUvProjectTool();
    actions::add({.id = "window.uvEditor", .label = "UV Editor", .category = "Windows", .run = [] {
                      if (opener())
                          opener()();
                  },
                  .doc = "2D uv view with selection, manipulators and the uv tools.",
                  .python = "lila.ui.run_action(\"window.uvEditor\")"});
    auto cmd = [](std::string id, std::string label, const char* command, json call, std::string doc) {
        std::string py = ops::toPython(command, call);
        actions::add({.id = std::move(id), .label = std::move(label), .category = "UV", .doc = std::move(doc), .python = py, .command = command,
                      .call = call});
    };
    cmd("uv.auto", "Auto UV", "uvAuto", json::object(), "Seams, unwrap and pack in one step.");
    cmd("uv.autoSeams", "Auto Seams", "uvAutoSeams", json::object(), "Place seams for game assets.");
    cmd("uv.unwrap", "Unwrap", "uvUnwrap", json::object(), "Unwrap along the seams with low stretch.");
    // uv.cut, uv.sew and uv.weld come from uvLive::install, which re-solves the touched islands in live mode.
    uvLive::install();
    for (auto [mode, label, doc] : {std::tuple{"vertex", "UV Vertex Select", "Select uv points in the UV editor (vertex mode)."},
                                    {"edge", "UV Edge Select", "Select uv edges in the UV editor (edge mode)."},
                                    {"face", "UV Face Select", "Select uv faces in the UV editor (face mode)."},
                                    {"island", "UV Island Select", "Face mode where clicks and marquees, in the UV editor and the viewports, select whole uv islands."}})
    {
        std::string id = std::string("uv.") + mode + "Mode";
        actions::add({.id = id, .label = label, .category = "UV", .run = [m = std::string(mode)] { setPickMode(m); }, .doc = doc,
                      .python = "lila.ui.run_action(\"" + id + "\")"});
    }
    cmd("uv.straighten", "Straighten UVs", "uvStraighten", json::object(), "Rectify islands into grids.");
    cmd("uv.orient", "Orient UVs", "uvOrient", json::object(), "Turn islands upright.");
    cmd("uv.rotateLeft", "Rotate Islands Left", "uvTransform", json{{"rotate", 90.0}, {"perIsland", true}},
        "Rotate each selected island 90 degrees counter-clockwise about its centre.");
    cmd("uv.rotateRight", "Rotate Islands Right", "uvTransform", json{{"rotate", -90.0}, {"perIsland", true}},
        "Rotate each selected island 90 degrees clockwise about its centre.");
    cmd("uv.flipX", "Flip Islands X", "uvTransform", json{{"scale", {-1.0, 1.0}}, {"perIsland", true}},
        "Mirror each selected island left to right about its centre.");
    cmd("uv.flipY", "Flip Islands Y", "uvTransform", json{{"scale", {1.0, -1.0}}, {"perIsland", true}},
        "Mirror each selected island top to bottom about its centre.");
    cmd("uv.lightmap", "Lightmap UVs", "uvLightmap", json::object(), "Generate Unreal lightmap uvs in uv1.");
    cmd("uv.bff", "BFF Unwrap", "uvBff", json::object(), "Boundary First Flattening: conformal, boundary lengths kept, least area stretch.");
    cmd("uv.bffDisk", "BFF Unwrap to Disk", "uvBff", json{{"boundary", "disk"}}, "Flatten islands conformally onto circles.");
    cmd("uv.bffRectangle", "BFF Unwrap to Rectangle", "uvBff", json{{"boundary", "rectangle"}},
        "Flatten islands conformally onto rectangles; the selected vertices (or the sharpest four) are the corners.");
    cmd("uv.bffCones", "BFF Unwrap with Cones", "uvBff", json{{"cones", 6}}, "BFF with six automatic cone points to cut area stretch.");
    cmd("uv.optimizeConformal", "Optimize Angles", "uvOptimize", json{{"mode", "conformal"}}, "Relax islands in place to keep angles.");
    cmd("uv.optimizeAuthalic", "Optimize Areas", "uvOptimize", json{{"mode", "authalic"}}, "Relax islands in place to keep areas.");
    cmd("uv.optimizeBlend", "Optimize Blend", "uvOptimize", json{{"mode", "blend"}}, "Relax islands in place, half angles, half areas.");
    cmd("uv.optimizeIsometric", "Optimize Isometric", "uvOptimize", json{{"mode", "isometric"}}, "Relax islands in place with SLIM.");
    cmd("uv.mosaic", "Mosaic", "uvMosaic", json::object(), "Split islands that cannot flatten without stretch, then re-solve the pieces.");
    cmd("uv.autoSeams2", "Auto Seams 2", "uvAutoSeams", json{{"method", "optcuts"}},
        "Seams and stretch optimised together (OptCuts): short, hidden seams under a stretch bound.");
    cmd("uv.unwrapSymmetric", "Unwrap Symmetric", "uvUnwrapSymmetric", json::object(),
        "Unwrap one side and mirror it onto the other; islands across the plane come out symmetric.");
    cmd("uv.unwrapSymmetricStack", "Unwrap Symmetric Stacked", "uvUnwrapSymmetric", json{{"mode", "stack"}},
        "Unwrap one side; mirrored islands share its uvs.");
    cmd("uv.straightenEdges", "Straighten UV Edges", "uvStraightenEdges", json::object(),
        "Lay the selected edge chains straight in uv and re-solve around them.");
    cmd("uv.alignU", "Align Side to U", "uvAlignToEdge", json{{"axis", "u"}}, "Turn islands so the selected edges run along U.");
    cmd("uv.alignV", "Align Side to V", "uvAlignToEdge", json{{"axis", "v"}}, "Turn islands so the selected edges run along V.");
    cmd("uv.orientByEdge", "Orient by Edge", "uvAlignToEdge", json::object(), "Turn islands so the selected edges run along the nearest axis.");
    cmd("uv.projectPlanar", "Planar Projection", "uvProject", json{{"type", "planar"}}, "Project uvs along the best-fit plane.");
    cmd("uv.projectBox", "Box Projection", "uvProject", json{{"type", "box"}}, "Project uvs from six sides.");
    cmd("uv.projectCylindrical", "Cylindrical Projection", "uvProject", json{{"type", "cylindrical"}}, "Project uvs around an axis.");
    cmd("uv.projectSpherical", "Spherical Projection", "uvProject", json{{"type", "spherical"}}, "Project uvs from the centre.");
}
} // namespace lila::ui
