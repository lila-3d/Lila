#include "ui/UvPackPanel.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/SpinScrub.h"
#include "ui/UvEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
using json = nlohmann::json;

namespace
{
constexpr const char* kPrefKey = "/uvEditor/pack";

json runPack(const json& call)
{
    try
    {
        return ops::execute("uvPack", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return nullptr;
}

/** Combo box whose items carry the flag value each stands for. */
QComboBox* choice(QWidget* parent, std::initializer_list<std::pair<const char*, const char*>> items, const QString& tip)
{
    auto* c = new QComboBox(parent);
    for (auto [label, value] : items)
        c->addItem(label, QString(value));
    c->setToolTip(tip);
    c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    c->setMinimumContentsLength(12);
    return c;
}

void selectValue(QComboBox* c, const std::string& value)
{
    int i = c->findData(QString::fromStdString(value));
    if (i >= 0)
        c->setCurrentIndex(i);
}

std::string valueOf(const QComboBox* c)
{
    return c->currentData().toString().toStdString();
}

QDoubleSpinBox* numberSpin(QWidget* parent, double lo, double hi, double value, int decimals, const QString& tip)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setValue(value);
    s->setKeyboardTracking(false);
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setToolTip(tip);
    return s;
}

QSpinBox* integer(QWidget* parent, int lo, int hi, int value, const QString& tip)
{
    auto* s = new QSpinBox(parent);
    s->setRange(lo, hi);
    s->setValue(value);
    s->setKeyboardTracking(false);
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setToolTip(tip);
    return s;
}
} // namespace

json uvPackOptions()
{
    json o = PrefStore::get().value("general", kPrefKey, json::object());
    return o.is_object() ? o : json::object();
}

void setUvPackOptions(const json& options)
{
    PrefStore::get().set("general", kPrefKey, options);
}

UvPackPanel::UvPackPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);
    auto row = [&](std::initializer_list<QWidget*> widgets) {
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        for (QWidget* x : widgets)
            h->addWidget(x);
        h->addStretch(1);
        return w;
    };
    // Groups are a text label over label/field rows: no boxes inside the Pack section.
    auto section = [&](const QString& title) {
        auto* label = new QLabel(title, this);
        QFont f = label->font();
        f.setWeight(QFont::DemiBold);
        label->setFont(f);
        root->addSpacing(4);
        root->addWidget(label);
        auto* box = new QWidget(this);
        auto* form = new QFormLayout(box);
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(6);
        form->setVerticalSpacing(4);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        root->addWidget(box);
        return form;
    };

    QFormLayout* islands = section("Islands");
    mKeep = choice(this, {{"All", "none"}, {"Around Selected", "selected"}, {"Selected Into Gaps", "unselected"}},
                   "Pack: Around Selected keeps the selected islands put and packs the rest around them. Selected Into Gaps packs only "
                   "the selected islands pack, into the space the others leave");
    mFixPinned = new QCheckBox("Keep pinned islands", this);
    mFixPinned->setToolTip("Islands with pinned uvs stay put");
    islands->addRow("Pack", mKeep);
    islands->addRow("", mFixPinned);
    mSeparate = choice(this, {{"Together", "none"}, {"Per Material", "material"}, {"Per Object", "object"}, {"Per Attribute", "attribute"}},
                       "Separate: Together shares one layout between every island. Per Material, Object or Attribute packs each group apart from "
                       "the others");
    mGroupAttr = new QLineEdit("uvGroup", this);
    mGroupAttr->setToolTip("Group Attribute: the face int attribute holding each face's group");
    mGroupAttr->setMaximumWidth(90);
    islands->addRow("Separate", row({mSeparate, mGroupAttr}));
    mInto = choice(this, {{"Own 0-1", "groupLayouts"}, {"Own UDIM Tile", "groupTiles"}, {"Own Region", "groupRegions"}},
                   "Each Into: Own 0-1 fills the whole tile per group, overlapping the others (a texture set per material). Own UDIM Tile: "
                   "one tile each at one texel density. Own Region: tile 1001 split between the groups by area");
    islands->addRow("Each Into", mInto);

    QFormLayout* fit = section("Fit");
    mRotation = choice(this, {{"None", "0"}, {"90\u00B0", "90"}, {"45\u00B0", "45"}, {"15\u00B0", "15"}, {"Free", "free"}},
                       "Rotation: angles each island may turn to. Free also tries each island's best-fitting angles");
    mFlip = new QCheckBox("Flip", this);
    mFlip->setToolTip("Also try islands mirrored (the texture reads mirrored on them)");
    mOrient = new QCheckBox("Pre-orient", this);
    mOrient->setToolTip("Turn each island to its smallest bounding rectangle first");
    fit->addRow("Rotation", mRotation);
    fit->addRow("", row({mFlip, mOrient}));
    mScale = choice(this, {{"Normalize", "normalize"}, {"Keep Relative", "keep"}, {"Lock Size", "lock"}},
                    "Scale: Normalize evens texel density across islands and objects. Keep Relative keeps size ratios. "
                    "Lock Size keeps each island's exact uv size and only moves it");
    fit->addRow("Scale", mScale);
    mDensity = numberSpin(this, 0.0, 100000.0, 0.0, 3, "Density: pack at this texel density instead of filling the tiles; Fill packs as large as fits");
    mDensity->setSpecialValueText("Fill");
    mDensity->setSuffix(" px/cm");
    fit->addRow("Density", mDensity);
    mStack = new QCheckBox("Identical", this);
    mStack->setToolTip("Islands with the same shape share one spot");
    mStackMirrored = new QCheckBox("Mirrored", this);
    mStackMirrored->setToolTip("Mirror-image islands (the halves of a symmetric model) stack as well");
    mShowStacks = new QCheckBox("Mark", this);
    mShowStacks->setToolTip("Mark stacked islands in the view with how many share the spot");
    fit->addRow("Stack", row({mStack, mStackMirrored, mShowStacks}));

    QFormLayout* space = section("Space");
    mTilesU = integer(this, 1, 10, 1, "UDIM Tiles: across");
    mTilesU->setPrefix("U ");
    mTilesV = integer(this, 1, 100, 1, "UDIM Tiles: up");
    mTilesV->setPrefix("V ");
    mKeepTile = new QCheckBox("Keep current tile", this);
    mKeepTile->setToolTip("Islands stay in the UDIM tile they are in now instead of filling the tiles in order");
    space->addRow("UDIM Tiles", row({mTilesU, mTilesV}));
    space->addRow("", mKeepTile);
    const char* regionPrefix[] = {"U  ", "V  ", "U  ", "V  "};
    const char* regionTips[] = {"Region Min: left, in tile uvs", "Region Min: bottom", "Region Max: right", "Region Max: top"};
    for (int i = 0; i < 4; ++i)
    {
        mRegion[i] = new controls::SliderField(this);
        mRegion[i]->setRange(0.0, 1.0);
        mRegion[i]->setDecimals(3);
        mRegion[i]->setSingleStep(0.05);
        mRegion[i]->setValue(i < 2 ? 0.0 : 1.0);
        mRegion[i]->setToolTip(regionTips[i]);
        mRegion[i]->setPrefix(regionPrefix[i]);
    }
    space->addRow("Region Min", row({mRegion[0], mRegion[1]}));
    space->addRow("Region Max", row({mRegion[2], mRegion[3]}));
    mMargin = numberSpin(this, -1.0, 512.0, -1.0, 1, "Border: texels from the tile border; Auto is half the padding");
    mMargin->setSpecialValueText("Auto");
    mMargin->setSuffix(" px");
    space->addRow("Border", mMargin);
    mMips = integer(this, 0, 8, 0, "Mip Safe: padding and border hold down to this mip level (doubled per level)");
    space->addRow("Mip Safe", mMips);

    QFormLayout* search = section("Search");
    mEffort = choice(this, {{"Fast", "0"}, {"Normal", "1"}, {"High", "2"}, {"Max", "3"}},
                     "Effort: more island orders and a finer search grid pack denser and take longer");
    search->addRow("Effort", mEffort);
    mTimeLimit = numberSpin(this, 0.0, 600.0, 0.0, 1, "Time Limit: stop refining after this many seconds (results can then vary between runs)");
    mTimeLimit->setSpecialValueText("None");
    mTimeLimit->setSuffix(" s");
    search->addRow("Time Limit", mTimeLimit);

    mResult = new QLabel(this);
    mResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mResult->setWordWrap(true);
    mResult->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(mResult);

    // The fallbacks load() reads an empty options object with.
    auto comboDefault = [](QComboBox* c, const char* value) { setComboDefault(c, c->findData(QString(value))); };
    comboDefault(mKeep, "none");
    comboDefault(mSeparate, "none");
    comboDefault(mInto, "groupLayouts");
    comboDefault(mRotation, "90");
    comboDefault(mScale, "normalize");
    comboDefault(mEffort, "1");
    for (QCheckBox* c : {mFixPinned, mFlip, mStack, mShowStacks, mKeepTile})
        setCheckDefault(c, false);
    for (QCheckBox* c : {mOrient, mStackMirrored})
        setCheckDefault(c, true);
    for (int i = 0; i < 4; ++i)
        setSpinDefault(mRegion[i], i < 2 ? 0.0 : 1.0);
    for (QAbstractSpinBox* s : std::initializer_list<QAbstractSpinBox*>{mDensity, mMips, mTimeLimit})
        setSpinDefault(s, 0.0);
    for (QSpinBox* s : {mTilesU, mTilesV})
        setSpinDefault(s, 1);
    setSpinDefault(mMargin, -1.0);

    load(uvPackOptions());

    connect(mShowStacks, &QCheckBox::toggled, this, [this](bool on) {
        if (onShowStacks)
            onShowStacks(on);
    });
    auto changed = [this] { save(); };
    for (QComboBox* c : {mRotation, mScale, mSeparate, mInto, mKeep, mEffort})
        connect(c, &QComboBox::currentIndexChanged, this, changed);
    for (QCheckBox* c : {mFlip, mOrient, mStack, mStackMirrored, mFixPinned, mKeepTile})
        connect(c, &QCheckBox::toggled, this, changed);
    for (QDoubleSpinBox* s : {mDensity, mMargin, mTimeLimit, mRegion[0], mRegion[1], mRegion[2], mRegion[3]})
        connect(s, &QDoubleSpinBox::valueChanged, this, changed);
    for (QSpinBox* s : {mTilesU, mTilesV, mMips})
        connect(s, &QSpinBox::valueChanged, this, changed);
    connect(mGroupAttr, &QLineEdit::editingFinished, this, changed);
    connect(mSeparate, &QComboBox::currentIndexChanged, this, [this] { updateEnabled(); });
    connect(mStack, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    updateEnabled();
}

void UvPackPanel::updateEnabled()
{
    const std::string separate = valueOf(mSeparate);
    mInto->setEnabled(separate != "none");
    mGroupAttr->setVisible(separate == "attribute");
    mKeepTile->setEnabled(separate == "none");
    mStackMirrored->setEnabled(mStack->isChecked());
}

json UvPackPanel::options() const
{
    json o;
    std::string rot = valueOf(mRotation);
    o["freeRotation"] = rot == "free";
    o["rotateStep"] = rot == "free" ? 90 : std::stoi(rot);
    o["flip"] = mFlip->isChecked();
    o["orient"] = mOrient->isChecked();
    o["scaleMode"] = valueOf(mScale);
    o["texelsPerUnit"] = mDensity->value();
    o["stack"] = mStack->isChecked();
    o["stackMirrored"] = mStackMirrored->isChecked();
    const std::string separate = valueOf(mSeparate);
    o["groupBy"] = separate;
    o["layout"] = separate != "none" ? valueOf(mInto) : mKeepTile->isChecked() ? "keepTile" : "fill";
    o["groupAttr"] = mGroupAttr->text().toStdString();
    o["tilesU"] = mTilesU->value();
    o["tilesV"] = mTilesV->value();
    o["region"] = {mRegion[0]->value(), mRegion[1]->value(), mRegion[2]->value(), mRegion[3]->value()};
    o["margin"] = mMargin->value();
    o["paddingMips"] = mMips->value();
    o["keep"] = valueOf(mKeep);
    o["fixPinned"] = mFixPinned->isChecked();
    o["perObject"] = false;
    o["effort"] = std::stoi(valueOf(mEffort));
    o["timeLimit"] = mTimeLimit->value();
    return o;
}

void UvPackPanel::load(const json& o)
{
    mLoading = true;
    selectValue(mRotation, o.value("freeRotation", false) ? "free" : std::to_string(o.value("rotateStep", 90)));
    mFlip->setChecked(o.value("flip", false));
    mOrient->setChecked(o.value("orient", true));
    selectValue(mScale, o.value("scaleMode", std::string("normalize")));
    mDensity->setValue(o.value("texelsPerUnit", 0.0));
    mStack->setChecked(o.value("stack", false));
    mStackMirrored->setChecked(o.value("stackMirrored", true));
    const std::string layout = o.value("layout", std::string("fill")), groupBy = o.value("groupBy", std::string("none"));
    const bool grouped = groupBy != "none" && layout.starts_with("group");
    if (o.value("perObject", false))
    {
        selectValue(mSeparate, "object");
        selectValue(mInto, "groupLayouts");
    }
    else
    {
        selectValue(mSeparate, grouped ? groupBy : "none");
        selectValue(mInto, grouped ? layout : "groupLayouts");
    }
    mKeepTile->setChecked(layout == "keepTile");
    mGroupAttr->setText(QString::fromStdString(o.value("groupAttr", std::string("uvGroup"))));
    mTilesU->setValue(o.value("tilesU", 1));
    mTilesV->setValue(o.value("tilesV", 1));
    json region = o.value("region", json::array({0.0, 0.0, 1.0, 1.0}));
    for (int i = 0; i < 4; ++i)
        if (region.is_array() && region.size() == 4)
            mRegion[i]->setValue(region[size_t(i)].get<double>());
    mMargin->setValue(o.value("margin", -1.0));
    mMips->setValue(o.value("paddingMips", 0));
    selectValue(mKeep, o.value("keep", std::string("none")));
    mFixPinned->setChecked(o.value("fixPinned", false));
    selectValue(mEffort, std::to_string(o.value("effort", 1)));
    mTimeLimit->setValue(o.value("timeLimit", 0.0));
    mLoading = false;
    updateEnabled();
}

void UvPackPanel::save()
{
    if (mLoading)
        return;
    json o = uvPackOptions();
    o.update(options());
    setUvPackOptions(o);
}

void UvPackPanel::showResult(const json& r)
{
    if (!r.is_object() || !r.contains("density"))
    {
        mResult->clear();
        return;
    }
    QString text = QString("%1% dense, %2 px/unit").arg(r.value("density", 0.0), 0, 'f', 1).arg(r.value("texelsPerUnit", 0.0), 0, 'f', 2);
    if (int tiles = r.value("tiles", 1); tiles > 1)
        text += QString(", %1 tiles").arg(tiles);
    if (int stacked = r.value("stacked", 0); stacked > 0)
        text += QString(", %1 stacked").arg(stacked);
    text += QString(", %1 s").arg(r.value("seconds", 0.0), 0, 'f', 2);
    mResult->setText(text);
}

void registerUvPackActions()
{
    auto add = [](std::string id, std::string label, json extra, std::string doc) {
        json example = json{{"resolution", 1024}, {"padding", 4}};
        example.update(extra);
        optionBox::addCustom(id, showUvPackOptions);
        actions::add({.id = std::move(id),
                      .label = std::move(label),
                      .category = "UV",
                      .run = [extra] {
                          json call = uvPackOptions();
                          call.update(extra);
                          runPack(call);
                      },
                      .doc = std::move(doc),
                      .python = ops::toPython("uvPack", example)});
    };
    add("uv.pack", "Pack UVs", json::object(),
        "Pack islands with the UV editor's Pack options (resolution, padding, rotation, stacking, tiles, effort).");
    add("uv.packAroundSelected", "Pack Around Selected", json{{"keep", "selected"}},
        "The selected islands stay where they are; every other island packs around them.");
    add("uv.packSelectionOnly", "Pack Selection Into Gaps", json{{"keep", "unselected"}},
        "Only the selected islands pack, into the space the other islands leave.");
    add("uv.packStacked", "Pack and Stack", json{{"stack", true}},
        "Pack with identical and mirrored islands stacked on one spot.");
    add("uv.packUdimByMaterial", "Pack UDIM Tile per Material", json{{"layout", "groupTiles"}, {"groupBy", "material"}},
        "Each material slot gets its own UDIM tile, all at one texel density.");
    add("uv.packPerMaterial", "Pack Each Material Separately", json{{"layout", "groupLayouts"}, {"groupBy", "material"}},
        "Each material slot packs into the full 0-1 on its own, overlapping the others: one texture set per material.");
    add("uv.packPerObject", "Pack Each Object Separately", json{{"perObject", true}},
        "Each selected object packs into a layout of its own.");
    actions::add({.id = "uv.selectStacks",
                  .label = "Select Stacked Islands",
                  .category = "UV",
                  .run =
                      [] {
                          try
                          {
                              ops::execute("uvStacks", {{"select", true}}, CallSource::UI);
                          }
                          catch (const CommandError& e)
                          {
                              log::error("{}", e.what());
                          }
                      },
                  .doc = "Select the faces of every island stacked on another one with the same shape.",
                  .python = ops::toPython("uvStacks", json{{"select", true}})});
}
} // namespace lila::ui
