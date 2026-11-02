#pragma once

#include <lila/Mesh.h>

#include <string>
#include <string_view>
#include <vector>

/** 3MF (3D Manufacturing Format): a zip holding 3D/3dmodel.model, the core spec's triangle meshes plus vertex colours. */
namespace print::threemf
{
struct Object
{
    std::string name;
    /** Written triangulated, positions as they are in the file's unit. */
    lila::geo::Mesh mesh;
};

struct WriteOptions
{
    /** micron, millimeter, centimeter, inch, foot or meter. */
    std::string unit = "millimeter";
    /** The current colour set per vertex, as a materials extension colour group. */
    bool colors = true;
};

/** The zip's bytes. */
std::string write(const std::vector<Object>& objects, const WriteOptions& opts = {});

struct Model
{
    std::string unit = "millimeter";
    std::vector<Object> objects;
    /** Names of the zip's entries, for checks. */
    std::vector<std::string> entries;
    bool colors = false;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** Mesh objects of the model part, each with its build item's transform applied; vertex colours into "colorSet". */
Model read(std::string_view zipBytes);

/** Scene centimetres per file unit; 0 for an unknown unit. */
double unitToCm(std::string_view unit);
} // namespace print::threemf
