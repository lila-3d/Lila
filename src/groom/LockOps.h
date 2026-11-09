#pragma once

#include "groom/Style.h"

#include <string>

namespace lila::groom
{
/** The groom graph node whose guides become the locks mesh. */
inline constexpr const char* kLocksType = "Locks";

/** The groom graph node type of a lock style ("LockCurl"). */
std::string lockStyleType(Style kind);

/** Registers Locks and the Lock<Style> operators with GroomLab: Lila's lock styles on a graph's guides. */
void registerLockOps();
} // namespace lila::groom
