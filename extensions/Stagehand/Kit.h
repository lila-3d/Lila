#pragma once

#include "Stagehand.h"

#include <string>
#include <vector>

/**
 * Modular kit snap points. A snap point is a stagehandSnap locator under a piece: its position is where the piece joins
 * another, its local +Z points out of the piece along the join and its local +Y is up. Two points join when their types
 * are compatible (each accepts the other's type, or both accept nothing and share a type); joined, they sit on the same
 * spot facing each other with their ups matching.
 */
namespace stagehand::kit
{
inline constexpr const char* kSnapType = "stagehandSnap";

struct SnapPoint
{
    NodeId node = lila::scene::kNoNode;
    /** The piece it belongs to: the top-level ancestor (a placed library asset's root). */
    NodeId piece = lila::scene::kNoNode;
    std::string type;
    std::vector<std::string> accepts;
    glm::dmat4 world{1.0};
};

std::vector<SnapPoint> snapPointsOf(Graph& g, NodeId piece);
/** Every snap point in the scene that's visible. */
std::vector<SnapPoint> allSnapPoints(Graph& g);
bool compatible(const SnapPoint& a, const SnapPoint& b);
/** The world matrix the piece of `mover` needs for mover to join `target`. */
glm::dmat4 joinedPieceMatrix(Graph& g, const SnapPoint& mover, const SnapPoint& target);
/** Whether two points are already joined, within tolerance cm and 2 degrees. */
bool joined(const SnapPoint& a, const SnapPoint& b, double tolerance);
/** A snap point node's accepts array. */
std::vector<std::string> acceptsOf(const lila::scene::Node& n);
/** The top-level DAG ancestor of a node. */
NodeId pieceOf(Graph& g, NodeId n);
} // namespace stagehand::kit
