#pragma once

#include "scene/AnimKeyOps.h"
#include "scene/Plug.h"

#include <QColor>
#include <QString>

#include <string>

namespace lila::ui::graphed
{
namespace anim = scene::anim;
using scene::NodeId;
using scene::PlugRef;

constexpr double kEps = 1e-6;
constexpr int kRulerHeight = 18;

/** One curve as the editor shows it: a scalar curve, or one channel of a rotation curve (see slotId). */
struct Channel
{
    PlugRef plug;
    NodeId curve = scene::kNoNode;
    std::string curveName;
    QString label;
    QString unit; // "°", "cm" or empty
    int level = 180; // gray
    Qt::PenStyle dash = Qt::SolidLine;
    int axis = -1; // 0..2 for an X, Y or Z component, which can draw in its axis colour
    bool shown = true;
    /** Display = value * mapScale + mapOffset: identity in the Absolute view, -1..1 bands in Stacked and Normalized. */
    double mapScale = 1.0;
    double mapOffset = 0.0;
    /** The band's centre in the Stacked view, in display units. */
    double bandCentre = 0.0;
    std::vector<anim::Modifier> modifiers;

    double display(double v) const { return v * mapScale + mapOffset; }
    double value(double display) const { return (display - mapOffset) / mapScale; }
};

struct KeySel
{
    NodeId curve = scene::kNoNode;
    double time = 0.0;
};

/**
 * A rotation curve shows as several channels, each with an id of its own: the curve's id with the channel's slot in
 * the top byte. Slots 0..2 are the Euler X, Y, Z it outputs, 3..6 the quaternion W, X, Y, Z.
 */
constexpr int kSlotShift = 56;
inline NodeId slotId(NodeId curve, int slot) { return curve | (NodeId(slot + 1) << kSlotShift); }
inline NodeId realCurve(NodeId id) { return id & ((NodeId(1) << kSlotShift) - 1); }
/** -1 for a scalar curve's channel. */
inline int slotOf(NodeId id) { return int(id >> kSlotShift) - 1; }

/** Smallest of 1, 2, 5 x 10^n at least rough. */
double niceNumber(double rough);
QString formatValue(double v);
/** Runs a command quietly; callers group several into one undo chunk. */
bool runUnechoed(const char* command, const nlohmann::json& call);
QColor channelColor(const Channel& ch, int level);
bool axisColors();
/** A rotation channel as a scalar curve: its keys' values, with fixed tangents matching the slopes it really has. */
anim::Curve derivedChannel(const anim::RotationCurve& rc, int slot);
double quatComponent(const glm::dquat& q, int c);
} // namespace lila::ui::graphed
