#pragma once

#include <lila/GeometryNodes.h>

#include <optional>
#include <string>

/** Socket declarations for Stagehand's geometry nodes. */
namespace stagehand
{
using Socket = lila::gn::SocketType;

inline lila::gn::SocketDecl in(std::string id, Socket t, lila::gn::json def, std::string doc)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
}
inline lila::gn::SocketDecl in(std::string id, Socket t, lila::gn::json def, std::string doc, std::optional<double> lo,
                               std::optional<double> hi = std::nullopt)
{
    lila::gn::SocketDecl s = in(std::move(id), t, std::move(def), std::move(doc));
    s.min = lo;
    s.max = hi;
    return s;
}
inline lila::gn::SocketDecl geoIn(std::string id, std::string doc, bool multi = false)
{
    return {.id = std::move(id), .type = Socket::Geometry, .multi = multi, .doc = std::move(doc)};
}
inline lila::gn::SocketDecl geoOut(std::string id, std::string doc) { return {.id = std::move(id), .type = Socket::Geometry, .doc = std::move(doc)}; }
} // namespace stagehand
