#pragma once

#include "CoreMinimal.h"

/**
 * Methods behind Lila's crowdSendToUnreal mass=True, registered with LilaLink under the feature "massCrowd" (positions are
 * Unreal space, cm):
 *
 * setMassCrowd {id, label, folder, lanes: [{points, crossing}], laneWidth, crowdTag, crossingTag, count, minGap, maxGap,
 * groups: [{name, share, speed, speedVariance, pause, radius}], mesh} makes the crowd's pedestrian network as ZoneGraph
 * shapes (a spline per lane on the Lila_Crowd profile, two lanes one each way, tagged crowdTag and crossingTag on
 * crossings, a small polygon wherever two or more lanes meet), points MassCrowd's settings at the tags, then per group a
 * StateTree (ST_<label>_<group>: find a wander target on the lanes, follow it, stand for pause seconds, again) and a Mass
 * entity config (CE_<label>_<group>) in folder, and a Mass spawner on the crowd lanes spawning count agents in the groups'
 * shares. Resending replaces the shapes and the spawner and rewrites the assets. Replies {actor, lanes, nodes, spawner,
 * stateTrees, configs, warnings}.
 *
 * sampleMassCrowd {radius} reads the Mass crowd agents of the play-in-editor world: per agent its position, its lane's tags,
 * and its distance to the nearest crowd lane and the nearest vehicle lane within radius (-1 beyond it). Replies {playing,
 * agents: [{id, p, lane, tags, crowd, vehicle}], instances}, instances counting the mesh instances
 * Mass draws.
 *
 * removeMassCrowd {id} deletes the lanes actor and the spawner made for id. Replies {removed}.
 */
namespace LilaLinkCrowd
{
inline const FString Feature = TEXT("massCrowd");

void RegisterMethods();
} // namespace LilaLinkCrowd
