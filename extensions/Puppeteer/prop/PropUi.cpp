// Props and weapons, the UI: the Props panel (class, hands, hold, aim, the holds over time, solve, Unreal), grip point
// handles drawn in the viewport, and the prop actions and menus. Every edit is a puppeteerProp* command.

#include "prop/Props.h"

#include "Puppeteer.h"
#include "hand/Hands.h"
#include "rig/Rig.h"

#include <lila/Controls.h>
#include <lila/Events.h>
#include <lila/Grapple.h>
#include <lila/Math.h>
#include <lila/Icons.h>
#include <lila/Pins.h>
#include <lila/Prefs.h>
#include <lila/Simulation.h>
#include <lila/Spaces.h>
#include <lila/UI.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer::prop
{
namespace
{
constexpr const char* kCat = "Puppeteer Props";
constexpr const char* kSections = "/props/sections";
constexpr const char* kPanelId = "puppeteerProps";

const std::vector<std::string>& keywords()
{
    static const std::vector<std::string> kw = {"prop", "weapon", "sword", "gun", "pistol", "rifle", "shotgun", "bazooka", "launcher", "bow",
                                                "shield", "axe", "spear", "dagger", "attach", "hold", "wield", "equip", "holster", "grip", "aim"};
    return kw;
}

struct Settings
{
    double blendFrames = 6.0;
    int aimStyle = 0; // 0 auto, then AimStyle + 1
    double aimWeight = 1.0;
};

Settings& settings()
{
    static Settings s = [] {
        const PrefStore& p = PrefStore::get();
        Settings c;
        c.blendFrames = std::max(0.0, p.value<double>(kPrefs, "/props/blendFrames", 6.0));
        c.aimStyle = std::clamp(p.value<int>(kPrefs, "/props/aimStyle", 0), 0, 4);
        c.aimWeight = std::clamp(p.value<double>(kPrefs, "/props/aimWeight", 1.0), 0.0, 1.0);
        return c;
    }();
    return s;
}

void saveSettings()
{
    PrefStore& p = PrefStore::get();
    const Settings& s = settings();
    p.set(kPrefs, "/props/blendFrames", s.blendFrames);
    p.set(kPrefs, "/props/aimStyle", s.aimStyle);
    p.set(kPrefs, "/props/aimWeight", s.aimWeight);
}

double blendSeconds() { return settings().blendFrames / std::max(anim::playback().fps, 1e-6); }

/** The prop the UI works on: the selection's, else the scene's only one. */
NodeId currentProp(Graph& g)
{
    for (NodeId id : g.selection())
        if (const NodeId p = propFor(g, id))
            return p;
    const std::vector<NodeId> all = props(g);
    return all.size() == 1 ? all.front() : kNoNode;
}

std::optional<Prop> current(Graph& g)
{
    const NodeId p = currentProp(g);
    return p ? load(g, p) : std::nullopt;
}

json propCall(Graph& g, const Prop& p)
{
    json call = {{"args", {g.path(p.root)}}, {"blend", blendSeconds()}};
    // The selected character, when one is: otherwise the command picks the holder or the only one.
    for (NodeId id : g.selection())
        if (const NodeId c = characterFor(g, id))
        {
            call["character"] = g.path(c);
            break;
        }
    return call;
}

void holdAction(const std::string& hand)
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return void(log::write(LogLevel::Warning, "Props: select a prop (or make one from a mesh with a class)"));
    json call = propCall(g, *p);
    call["hand"] = hand;
    runCommand("puppeteerPropHold", call);
}

/** Holds the prop in the hand that isn't holding it now, keeping one or two hands as it is. */
void handOffAction()
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return;
    g.evaluate();
    const NodeId holder = holderNow(g, *p);
    std::string hand = p->rightHanded ? "left" : "right";
    if (holder)
        if (const NodeId hn = hand::handFor(g, holder))
            if (const std::optional<hand::Hand> h = hand::load(g, hn))
                hand = h->side == hand::Side::Left ? "right" : "left";
    holdAction(hand);
}

void simpleAction(const char* command)
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return void(log::write(LogLevel::Warning, "Props: select a prop first"));
    json call = propCall(g, *p);
    if (std::string(command) == "puppeteerPropDrop")
        call.erase("character");
    runCommand(command, call);
}

/** The aim target: the other selected transform, else a new locator in front of the character, selected to place. */
void aimAction()
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return void(log::write(LogLevel::Warning, "Props: select a prop to aim"));
    std::string target;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && !propFor(g, id) && !characterFor(g, id) && n->type().attrIndex("worldMatrix") >= 0)
            target = n->path();
    g.evaluate();
    if (target.empty())
    {
        const NodeId holder = holderNow(g, *p);
        const NodeId ch = holder ? characterFor(g, holder) : kNoNode;
        glm::dvec3 at = glm::dvec3(worldOf(g, p->root)[3]);
        const glm::dvec3 up = lila::math::axisBasis().up;
        glm::dvec3 forward = lila::math::axisBasis().forward;
        // The character faces across the line from its left hand to its right.
        glm::dvec3 right(0.0), left(0.0);
        for (NodeId hn : hand::handsOf(g, ch))
            if (const std::optional<hand::Hand> h = hand::load(g, hn))
                (h->side == hand::Side::Left ? left : right) = glm::dvec3(worldOf(g, h->joint)[3]);
        glm::dvec3 across = right - left;
        across -= up * glm::dot(across, up);
        if (ch && glm::length(across) > 1e-3)
            forward = glm::normalize(glm::cross(up, across));
        at += forward * 500.0;
        const json made = runCommand("spaceLocator", {{"name", "propTarget"}});
        if (!made.is_array() || made.empty())
            return;
        target = made.at(0).get<std::string>();
        runCommand("setAttr", {{"args", {target + ".translate", at.x, at.y, at.z}}});
    }
    json call = propCall(g, *p);
    call["target"] = target;
    if (settings().aimStyle > 0)
        call["style"] = aimStyleId(AimStyle(settings().aimStyle - 1));
    call["weight"] = settings().aimWeight;
    runCommand("puppeteerPropAim", call);
    if (Node* t = named(g, target))
        g.select({t->id()});
}

void aimOffAction()
{
    Graph& g = Graph::current();
    const std::optional<Prop> p = current(g);
    if (!p)
        return;
    json call = propCall(g, *p);
    call["off"] = true;
    runCommand("puppeteerPropAim", call);
}

/** Makes the selection a prop of the class, or changes the selected prop's class. */
void classAction(Class c)
{
    Graph& g = Graph::current();
    json args = json::array();
    for (NodeId id : g.selection())
    {
        NodeId root = id;
        if (const NodeId pn = propFor(g, id))
            root = g.parent(pn);
        else if (characterFor(g, id))
            continue;
        const std::string path = g.path(root);
        if (std::find(args.begin(), args.end(), path) == args.end())
            args.push_back(path);
    }
    if (args.empty())
        return void(log::write(LogLevel::Warning, "Props: select a mesh or group to make a prop"));
    runCommand("puppeteerPropSetup", {{"args", args}, {"kind", preset(c).id}});
}

bool hasProp() { return currentProp(Graph::current()) != kNoNode; }

/** Whether the character holding the prop (or the selected one) has every bone Unreal's hand IK bones need. */
bool hasIkBones(Graph& g, NodeId character)
{
    if (!character)
        return false;
    int found = 0;
    const NodeId root = characterRoot(g, character);
    for (NodeId d : g.descendants(root))
    {
        std::string n = g.node(d)->name();
        if (const size_t c = n.rfind(':'); c != std::string::npos)
            n = n.substr(c + 1);
        found += n == "ik_hand_gun" || n == "ik_hand_r" || n == "ik_hand_l";
    }
    return found == 3;
}

NodeId propCharacter(Graph& g, const Prop& p)
{
    for (const Hold& h : holdsOf(g, p))
        if (h.target)
            if (const NodeId c = characterFor(g, h.target))
                return c;
    const std::vector<NodeId> all = characters(g);
    return all.size() == 1 ? all.front() : kNoNode;
}

std::string bare(const std::string& path)
{
    std::string n = path.substr(path.rfind('|') == std::string::npos ? 0 : path.rfind('|') + 1);
    if (const size_t c = n.rfind(':'); c != std::string::npos)
        n = n.substr(c + 1);
    return n;
}

QString holdLabel(const Hold& h)
{
    if (h.kind == "hand")
        return h.side == "left" ? "Left hand" : "Right hand";
    if (h.kind == "holster")
        return "Holster";
    if (h.kind == "world")
        return "World";
    if (h.kind == "parent")
        return "Parent";
    return QString::fromStdString(bare(h.space));
}

// Panel -------------------------------------------------------------------------------------------------------------

class PropsPanel final : public QWidget
{
public:
    explicit PropsPanel(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        mTitle = new QLabel(this);
        QFont f = mTitle->font();
        f.setBold(true);
        mTitle->setFont(f);
        v->addWidget(mTitle);
        mEmpty = new QLabel("No prop selected. Select a mesh and pick a class.", this);
        mEmpty->setWordWrap(true);
        mEmpty->setForegroundRole(QPalette::PlaceholderText);
        v->addWidget(mEmpty);

        // Class: one tile per class, the current one checked.
        auto* classBox = new QWidget(this);
        std::vector<QWidget*> tiles;
        for (const Preset& p : presets())
        {
            QToolButton* t = controls::tile(std::string("puppeteer.prop.class.") + p.id, p.icon, classBox, p.label);
            tiles.push_back(t);
            mTiles.push_back(t);
        }
        auto* cv = new QVBoxLayout(classBox);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(4);
        cv->addWidget(controls::grid(tiles, 4, classBox));
        auto* form = new QGridLayout;
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(4);
        auto label = [&](QGridLayout* grid, const char* text, int row, QWidget* owner) {
            auto* l = new QLabel(text, owner);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
        };
        mHand = new controls::Segmented(classBox);
        mHand->add("Right", {}, "Hand: the prop's main hand, which Hold uses", 0);
        mHand->add("Left", {}, "Hand: the prop's main hand, which Hold uses", 1);
        connect(mHand, &controls::Segmented::picked, this, [](int id) {
            Graph& g = Graph::current();
            if (const std::optional<Prop> p = current(g))
                runCommand("setAttr", {{"args", {g.path(p->node) + ".hand", id}}});
        });
        label(form, "Hand", 0, classBox);
        form->addWidget(mHand, 0, 1);
        mHands = new controls::Segmented(classBox);
        mHands->add("One", {}, "Hold: in the main hand only", 1);
        mHands->add("Both", {}, "Hold: the other hand on the off grip too, both arms sharing the reach", 2);
        connect(mHands, &controls::Segmented::picked, this, [](int id) {
            Graph& g = Graph::current();
            if (const std::optional<Prop> p = current(g))
                runCommand("setAttr", {{"args", {g.path(p->node) + ".twoHanded", id == 2}}});
        });
        label(form, "Hands", 1, classBox);
        form->addWidget(mHands, 1, 1);
        form->setColumnStretch(1, 1);
        cv->addLayout(form);
        v->addWidget(controls::section("Prop", classBox, this, kSections));

        // Hold: the daily actions, then the blend they share.
        auto* holdBox = new QWidget(this);
        auto* hv = new QVBoxLayout(holdBox);
        hv->setContentsMargins(0, 0, 0, 0);
        hv->setSpacing(4);
        hv->addWidget(controls::grid({controls::row("puppeteer.prop.hold", "hand-grab", holdBox), controls::row("puppeteer.prop.handOff", "arrows-exchange", holdBox),
                                      controls::row("puppeteer.prop.holster", "backpack", holdBox), controls::row("puppeteer.prop.drop", "hand-stop", holdBox)},
                                     2, holdBox));
        auto* blendRow = new QGridLayout;
        blendRow->setHorizontalSpacing(8);
        mBlend = controls::valueField(0.0, 240.0, 0, "f", holdBox);
        mBlend->setToolTip("Blend: frames a hold, handoff or aim takes to settle after the current frame");
        connect(mBlend, &QDoubleSpinBox::valueChanged, this, [](double fr) {
            settings().blendFrames = fr;
            saveSettings();
        });
        label(blendRow, "Blend", 0, holdBox);
        blendRow->addWidget(mBlend, 0, 1);
        blendRow->setColumnStretch(1, 1);
        hv->addLayout(blendRow);
        v->addWidget(controls::section("Hold", holdBox, this, kSections));

        // Aim.
        auto* aimBox = new QWidget(this);
        auto* av = new QVBoxLayout(aimBox);
        av->setContentsMargins(0, 0, 0, 0);
        av->setSpacing(4);
        av->addWidget(controls::grid({controls::row("puppeteer.prop.aim", "target-arrow", aimBox), controls::row("puppeteer.prop.aimOff", "target-off", aimBox)},
                                     2, aimBox));
        auto* aimForm = new QGridLayout;
        aimForm->setHorizontalSpacing(8);
        aimForm->setVerticalSpacing(4);
        mStyle = new controls::Segmented(aimBox);
        mStyle->add("Auto", {}, "Style: the class's (shoulder for long guns and launchers, extend for pistols and bows, guard for blades)", 0);
        mStyle->add("Shoulder", {}, "Style: the stock to the shoulder, then turned onto the target", 2);
        mStyle->add("Extend", {}, "Style: held out at arm's length toward the target", 3);
        mStyle->add("Guard", {}, "Style: held out, below the line to the target", 4);
        mStyle->add("Turn", {}, "Style: turned where it is", 1);
        connect(mStyle, &controls::Segmented::picked, this, [](int id) {
            settings().aimStyle = id;
            saveSettings();
        });
        label(aimForm, "Style", 0, aimBox);
        aimForm->addWidget(mStyle, 0, 1);
        mWeight = new controls::SliderField(aimBox);
        mWeight->setRange(0.0, 1.0);
        mWeight->setDecimals(2);
        mWeight->setSingleStep(0.05);
        mWeight->setToolTip("Weight: how fully Aim turns the prop onto the target");
        connect(mWeight, &QDoubleSpinBox::valueChanged, this, [](double w) {
            settings().aimWeight = w;
            saveSettings();
        });
        label(aimForm, "Weight", 1, aimBox);
        aimForm->addWidget(mWeight, 1, 1);
        aimForm->setColumnStretch(1, 1);
        av->addLayout(aimForm);
        v->addWidget(controls::section("Aim", aimBox, this, kSections));

        // The holds over time; a row jumps to its frame.
        auto* listBox = new QWidget(this);
        auto* lv = new QVBoxLayout(listBox);
        lv->setContentsMargins(0, 0, 0, 0);
        lv->setSpacing(4);
        mHolds = new QTreeWidget(listBox);
        mHolds->setColumnCount(2);
        mHolds->setHeaderHidden(true);
        mHolds->setRootIsDecorated(false);
        mHolds->header()->setStretchLastSection(true);
        mHolds->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        mHolds->setMinimumHeight(70);
        mHolds->setToolTip("Who holds the prop from which frame. Click a row to go to its frame.");
        connect(mHolds, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* it) {
            runCommand("currentTime", {{"args", {it->data(0, Qt::UserRole).toDouble()}}});
        });
        lv->addWidget(mHolds);
        mHoldsEmpty = new QLabel("Not held yet. Hold puts it in a hand at the current frame.", listBox);
        mHoldsEmpty->setWordWrap(true);
        mHoldsEmpty->setForegroundRole(QPalette::PlaceholderText);
        lv->addWidget(mHoldsEmpty);
        v->addWidget(controls::section("Holds", listBox, this, kSections));

        // Solve, with the rarer actions in overflow.
        auto* solveBox = new QWidget(this);
        auto* sv = new QHBoxLayout(solveBox);
        sv->setContentsMargins(0, 0, 0, 0);
        sv->setSpacing(4);
        sv->addWidget(controls::push("puppeteer.prop.solve", solveBox));
        mStatus = new QLabel(solveBox);
        mStatus->setForegroundRole(QPalette::PlaceholderText);
        mStatus->setToolTip("Gap: the largest distance left between a hand and its grip, or an aim and its target, at this frame");
        sv->addWidget(mStatus, 1);
        sv->addWidget(controls::overflow({"puppeteer.prop.fit", "puppeteer.prop.addGrip", "puppeteer.prop.unreal"}, solveBox));
        v->addWidget(controls::section("Solve", solveBox, this, kSections));

        // Unreal: the socket the prop attaches by, and the optional hand IK bones.
        auto* ueBox = new QWidget(this);
        auto* uf = new QGridLayout(ueBox);
        uf->setContentsMargins(0, 0, 0, 0);
        uf->setHorizontalSpacing(8);
        uf->setVerticalSpacing(4);
        mBone = new QLineEdit(ueBox);
        mBone->setToolTip("Bone: the bone of the character's skeletal mesh the prop attaches to. Empty: the hand holding it (hand_r)");
        label(uf, "Bone", 0, ueBox);
        uf->addWidget(mBone, 0, 1);
        mSocket = new QLineEdit(ueBox);
        mSocket->setToolTip("Socket: the socket made on that bone, at the offset the prop is held at. Empty: weapon_r or weapon_l");
        label(uf, "Socket", 1, ueBox);
        uf->addWidget(mSocket, 1, 1);
        mIkBones = new QCheckBox("IK Bones", ueBox);
        mIkBones->setToolTip("IK Bones: also key ik_hand_gun, ik_hand_r and ik_hand_l to follow the hands, for projects whose locomotion reads them");
        uf->addWidget(mIkBones, 2, 1);
        auto* setUp = new QToolButton(ueBox);
        setUp->setText("Set Up");
        setUp->setToolTip("Set the prop up for Unreal with this bone and socket; the Unreal link makes the socket when it sends the prop");
        connect(setUp, &QToolButton::clicked, this, [this] {
            Graph& g = Graph::current();
            const std::optional<Prop> p = current(g);
            if (!p)
                return;
            json call = {{"args", {g.path(p->root)}}};
            if (!mBone->text().trimmed().isEmpty())
                call["bone"] = mBone->text().trimmed().toStdString();
            if (!mSocket->text().trimmed().isEmpty())
                call["socket"] = mSocket->text().trimmed().toStdString();
            if (mIkBones->isVisible() && mIkBones->isChecked())
                call["ikBones"] = true;
            runCommand("puppeteerPropUnreal", call);
            sync();
        });
        uf->addWidget(setUp, 3, 1, Qt::AlignLeft);
        uf->setColumnStretch(1, 1);
        mUnreal = controls::section("Unreal", ueBox, this, kSections, false);
        v->addWidget(mUnreal);
        v->addStretch();

        for (events::Event e : {events::Event::UndoStackChanged, events::Event::SelectionChanged, events::Event::TimeChanged, events::Event::SceneOpened,
                                events::Event::SceneNew})
            mSubs.push_back(events::subscribe(e, [p = QPointer<PropsPanel>(this)](const events::EventData&) {
                if (p)
                    p->sync();
            }, "Puppeteer"));
        sync();
    }

    ~PropsPanel() override
    {
        for (events::SubscriptionId s : mSubs)
            events::unsubscribe(s);
    }

    void sync()
    {
        Graph& g = Graph::current();
        const Settings& s = settings();
        {
            const QSignalBlocker b1(mBlend), b2(mWeight);
            mBlend->setValue(s.blendFrames);
            mWeight->setValue(s.aimWeight);
        }
        mStyle->setCurrent(s.aimStyle);
        const std::optional<Prop> p = current(g);
        mTitle->setVisible(bool(p));
        mEmpty->setVisible(!p);
        for (QToolButton* t : mTiles)
            t->setChecked(false);
        mHolds->clear();
        if (!p)
        {
            mHand->clearCurrent();
            mHands->clearCurrent();
            mHoldsEmpty->setVisible(true);
            mHolds->setVisible(false);
            mStatus->clear();
            mUnreal->setVisible(false);
            return;
        }
        mTitle->setText(QString::fromStdString(g.node(p->root)->name()));
        mTiles[size_t(p->cls)]->setChecked(true);
        mHand->setCurrent(p->rightHanded ? 0 : 1);
        mHands->setCurrent(p->twoHanded ? 2 : 1);

        const std::vector<Hold> holds = holdsOf(g, *p);
        const double fps = std::max(anim::playback().fps, 1e-6);
        bool held = false;
        for (const Hold& h : holds)
        {
            // Where it sat before its first hold is the scene's, not a hold.
            if (h.kind == "parent" && &h == &holds.front() && holds.size() > 1)
                continue;
            held |= h.kind == "hand" || h.kind == "holster";
            auto* it = new QTreeWidgetItem(mHolds, {QString::number(h.time * fps, 'f', 0), holdLabel(h)});
            it->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
            it->setData(0, Qt::UserRole, h.time);
            it->setToolTip(1, QString::fromStdString(h.space));
        }
        mHolds->setVisible(held);
        mHoldsEmpty->setVisible(!held);

        const NodeId ch = propCharacter(g, *p);
        QString status;
        if (const NodeId gr = ch ? propGrapple(g, ch, false) : kNoNode)
        {
            const std::string layers = textOf(*g.node(gr), sim::kRecordingsAttr);
            const bool solved = !layers.empty() && layers != "[]";
            if (auto f = sim::cached(g, gr, sim::frameOf(g.time())); solved && f && f->state.size() >= 3)
                status = QString("Gap %1 cm").arg(f->state[f->state.size() - 3], 0, 'f', 2);
            else
                status = solved ? "Solved" : "Not solved";
        }
        mStatus->setText(status);
        mUnreal->setVisible(true);
        mIkBones->setVisible(hasIkBones(g, ch));
        const Node& root = *g.node(p->root);
        if (!mBone->hasFocus())
            mBone->setText(QString::fromStdString(lila::scene::pins::socketBoneOf(root)));
        if (!mSocket->hasFocus())
            mSocket->setText(QString::fromStdString(lila::scene::pins::socketOf(root)));
    }

private:
    QLabel *mTitle = nullptr, *mEmpty = nullptr, *mHoldsEmpty = nullptr, *mStatus = nullptr;
    std::vector<QToolButton*> mTiles;
    controls::Segmented *mHand = nullptr, *mHands = nullptr, *mStyle = nullptr;
    QDoubleSpinBox* mBlend = nullptr;
    controls::SliderField* mWeight = nullptr;
    QTreeWidget* mHolds = nullptr;
    QLineEdit *mBone = nullptr, *mSocket = nullptr;
    QCheckBox* mIkBones = nullptr;
    QWidget* mUnreal = nullptr;
    std::vector<events::SubscriptionId> mSubs;
};

// Points in the viewport ----------------------------------------------------------------------------------------------

const glm::vec4 kGrip{0.92f, 0.78f, 0.22f, 1.0f};
const glm::vec4 kAim{0.85f, 0.35f, 0.3f, 1.0f};
const glm::vec4 kRest{0.75f, 0.75f, 0.75f, 0.9f};
const glm::vec4 kSelectedPoint{1.0f, 1.0f, 1.0f, 1.0f};

/** Points show while they, their prop, or (for a holster) the prop put away there is selected. */
bool pointsShown(Graph& g, NodeId point)
{
    for (NodeId id : g.selection())
    {
        if (id == point || g.parent(id) == point)
            return true;
        const NodeId pn = propFor(g, point), sel = propFor(g, id);
        if (pn && sel == pn)
            return true;
        if (Node* n = g.node(point); n && roleOf(*n) == Role::Holster && sel)
            if (const std::optional<Prop> p = load(g, sel); p && textOf(*g.node(sel), "holster") == g.path(point))
                return true;
    }
    return false;
}

struct Drawn
{
    QPointF at, axisEnd, palmEnd;
    Role role = Role::Main;
};

std::optional<Drawn> drawnPoint(const ViewInfo& view, const Node& n, const glm::dmat4& world)
{
    const Role r = roleOf(n);
    const glm::dvec3 o(world[3]);
    const glm::dvec3 x = glm::normalize(glm::dvec3(world[0])), z = glm::normalize(glm::dvec3(world[2]));
    const double axisLen = r == Role::Muzzle || r == Role::Tip ? 25.0 : 6.0;
    const auto a = view.project(o), b = view.project(o + x * axisLen), c = view.project(o + z * 3.0);
    if (!a || !b || !c)
        return std::nullopt;
    return Drawn{*a, *b, *c, r};
}

void drawPoint(drawOverrides::DrawContext& c)
{
    Graph& g = Graph::current();
    if (!pointsShown(g, c.node.id()))
        return;
    const std::optional<Drawn> d = drawnPoint(c.view, c.node, c.world);
    if (!d)
        return;
    // Only the point itself reads as selected: a selected prop would otherwise whiten every handle on it.
    const std::vector<NodeId> sel = g.selection();
    const bool selected = std::find(sel.begin(), sel.end(), c.node.id()) != sel.end();
    const bool aims = d->role == Role::Muzzle || d->role == Role::Tip;
    const glm::vec4 col = selected ? kSelectedPoint : aims ? kAim : isGrip(d->role) ? kGrip : kRest;
    c.painter.smoothLine(d->at, d->axisEnd, selected ? 2.5f : 2.0f, col);
    if (isGrip(d->role) || d->role == Role::Holster)
    {
        // The palm side, short, and a ring for the handle.
        c.painter.smoothLine(d->at, d->palmEnd, 1.5f, col);
        c.painter.arc(d->at, selected ? 6.0 : 5.0, 0.0, 360.0, 1.5f, col);
        if (d->role == Role::Main)
            c.painter.smoothDisc(d->at, 2.5, col);
    }
    else if (aims)
        c.painter.smoothDisc(d->axisEnd, selected ? 4.0 : 3.0, col);
    else
        c.painter.rect(QRectF(d->at - QPointF(3, 3), QSizeF(6, 6)), col);
}

std::optional<double> pickPoint(const drawOverrides::PickContext& c)
{
    Graph& g = Graph::current();
    if (!pointsShown(g, c.node.id()))
        return std::nullopt;
    const std::optional<Drawn> d = drawnPoint(c.view, c.node, c.world);
    if (!d)
        return std::nullopt;
    if (!c.click)
    {
        const QRectF box = QRectF(c.a, c.b).normalized();
        return box.contains(d->at) ? std::optional<double>(0.0) : std::nullopt;
    }
    const double dist = std::hypot(c.a.x() - d->at.x(), c.a.y() - d->at.y()) - 5.0;
    return dist <= drawOverrides::kClickPixels ? std::optional<double>(std::max(dist, 0.0)) : std::nullopt;
}

void addAction(ext::Context& ctx, const char* id, const char* label, std::function<void()> run, const char* doc, const char* icon,
               std::vector<std::string> extra = {}, std::function<bool()> enabled = hasProp)
{
    std::vector<std::string> kw = keywords();
    kw.insert(kw.end(), extra.begin(), extra.end());
    actions::add(ctx, {.id = id, .label = label, .category = kCat, .run = std::move(run), .enabled = std::move(enabled), .doc = doc, .icon = icon,
                       .keywords = kw});
}
} // namespace
} // namespace puppeteer::prop

namespace puppeteer
{
void registerPropUi(lila::ext::Context& ctx)
{
    using namespace prop;
    panels::add(ctx, {.id = kPanelId,
                      .title = "Props",
                      .create = [](QWidget* parent) -> QWidget* { return new PropsPanel(parent); },
                      .area = Qt::LeftDockWidgetArea,
                      .icon = "sword",
                      .open = false,
                      .keywords = keywords(),
                      .docs = "props"});
    drawOverrides::add(ctx, {.id = "puppeteer.propPoint", .nodeType = kPointType, .draw = drawPoint, .pick = pickPoint});

    for (const Preset& p : presets())
        actions::add(ctx, {.id = std::string("puppeteer.prop.class.") + p.id,
                           .label = p.label,
                           .category = kCat,
                           .run = [c = p.cls] { classAction(c); },
                           .doc = std::string("Make the selection a ") + p.label + " prop (or change the selected prop to one), its grips, aim and "
                                  "holster fitted to its mesh.",
                           .checked =
                               [c = p.cls] {
                                   const std::optional<Prop> cur = current(Graph::current());
                                   return cur && cur->cls == c;
                               },
                           .icon = p.icon,
                           .keywords = {"prop", "weapon", p.id, hand::lowerOf(p.label)}});

    addAction(ctx, "puppeteer.prop.make", "Make Prop", [] { classAction(Class::Generic); },
              "Make the selected mesh or group a prop, with grip points fitted to it; pick its class in the Props panel.", "sword",
              {"make prop", "mark as prop", "weapon setup"}, [] { return actions::hasSelection(); });
    addAction(ctx, "puppeteer.prop.hold", "Hold", [] { holdAction("main"); },
              "Put the prop in the character's main hand from the current frame (both hands when it is set to), settling over the panel's Blend.",
              "hand-grab", {"pick up", "grab", "draw", "unholster"});
    addAction(ctx, "puppeteer.prop.handOff", "Other Hand", handOffAction,
              "Hand the prop to the character's other hand from the current frame, without a pop.", "arrows-exchange", {"handoff", "hand off", "swap hands", "switch hands"});
    addAction(ctx, "puppeteer.prop.holster", "Holster", [] { simpleAction("puppeteerPropHolster"); },
              "Put the prop away on the character (back, hip or thigh by class) from the current frame; Hold draws it again.", "backpack",
              {"sheathe", "put away", "stow", "back", "hip"});
    addAction(ctx, "puppeteer.prop.drop", "Drop", [] { simpleAction("puppeteerPropDrop"); },
              "Let go of the prop at the current frame: it stays in the world where it is and the hands open.", "hand-stop",
              {"release", "let go", "throw", "detach"});
    addAction(ctx, "puppeteer.prop.aim", "Aim", aimAction,
              "Aim the held prop at the other selected object, or at a new target placed in front of the character, from the current frame; "
              "the arms and body follow.",
              "target-arrow", {"point at", "look at", "shoulder", "aim at"});
    addAction(ctx, "puppeteer.prop.aimOff", "Aim Off", aimOffAction, "Blend the prop's aim out from the current frame.", "target-off", {"stop aiming"});
    addAction(ctx, "puppeteer.prop.solve", "Solve Props",
              [] {
                  Graph& g = Graph::current();
                  if (const std::optional<Prop> p = current(g))
                      runCommand("puppeteerPropSolve", {{"args", {g.path(p->root)}}});
              },
              "Solve the arms and body holding props over the playback range: off hands on their grips, aims on their targets.", "player-record",
              {"bake", "solve", "two hands"});
    addAction(ctx, "puppeteer.prop.fit", "Fit Points",
              [] {
                  Graph& g = Graph::current();
                  if (const std::optional<Prop> p = current(g))
                      runCommand("puppeteerPropFit", {{"args", {g.path(p->root)}}});
              },
              "Fit the prop's grip, aim and stock points to its mesh again by its class.", "focus-centered", {"refit", "auto fit", "grip points"});
    addAction(ctx, "puppeteer.prop.addGrip", "Add Grip",
              [] {
                  Graph& g = Graph::current();
                  if (const std::optional<Prop> p = current(g))
                  {
                      const json made = runCommand("puppeteerPropPoint", {{"args", {g.path(p->root)}}, {"role", "alt"}, {"name", "grip"}});
                      if (made.is_object())
                          if (Node* n = named(g, made.value("point", std::string())))
                              g.select({n->id()});
                  }
              },
              "Add another grip to the prop, starting on the main grip, to move where a hand should take it.", "plus", {"alternate grip", "grip point"});
    actions::add(ctx, {.id = "puppeteer.prop.unreal",
                       .label = "Set Up for Unreal",
                       .category = kCat,
                       .enabled = hasProp,
                       .doc = "Attach the prop to the holding bone (hand_r) through a socket at the offset it is held at, for the Unreal link; "
                              "optionally key the hand IK bones.",
                       .command = "puppeteerPropUnreal",
                       .icon = "package-export",
                       .keywords = [] {
                           std::vector<std::string> kw = keywords();
                           for (const char* k : {"unreal", "socket", "ik_hand_gun", "export", "ue5"})
                               kw.push_back(k);
                           return kw;
                       }()});
    actions::setEnabledWhen("puppeteer.prop.unreal", hasProp);

    menus::addItem(ctx, {.id = "puppeteer.props.menu", .menu = "Puppeteer", .kind = menus::Kind::Submenu, .label = "Props"});
    menus::addItem(ctx, {.id = "anim.props.menu", .menu = "Animation", .kind = menus::Kind::Submenu, .label = "Props"});
    int sep = 0;
    for (const std::string m : {"Puppeteer/Props", "Animation/Props"})
    {
        for (const char* a : {"window.puppeteerProps", "puppeteer.prop.make", "", "puppeteer.prop.hold", "puppeteer.prop.handOff", "puppeteer.prop.holster",
                              "puppeteer.prop.drop", "", "puppeteer.prop.aim", "puppeteer.prop.aimOff", "", "puppeteer.prop.solve", "puppeteer.prop.fit",
                              "puppeteer.prop.addGrip", "puppeteer.prop.unreal"})
            if (*a)
                menus::addItem(ctx, {.id = "puppeteer.props." + m + "." + a, .menu = m, .action = a});
            else
                menus::addItem(ctx, {.id = "puppeteer.props.sep" + std::to_string(sep++), .menu = m, .kind = menus::Kind::Separator});
        menus::addItem(ctx, {.id = "puppeteer.props." + m + ".classes", .menu = m, .kind = menus::Kind::Submenu, .label = "Class"});
        for (const Preset& p : presets())
            menus::addItem(ctx, {.id = "puppeteer.props." + m + ".class." + p.id, .menu = m + "/Class", .action = std::string("puppeteer.prop.class.") + p.id});
    }

    const events::SubscriptionId sub =
        events::subscribe(events::Event::SelectionChanged, [](const events::EventData&) { viewportTools::requestRedraw(); }, "Puppeteer");
    ctx.track("puppeteer:propsSelection", [sub] { events::unsubscribe(sub); });
}
} // namespace puppeteer
