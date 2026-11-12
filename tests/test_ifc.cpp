#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <filesystem>
#include <fstream>

using namespace lila;
namespace fs = std::filesystem;

namespace
{
/** One 4 m x 0.2 m x 3 m wall at engineering (10, 20, 0) on a ground storey, with a property set and a map conversion to UTM 33N. */
const char* kWall = R"(ISO-10303-21;
HEADER;
FILE_DESCRIPTION(('ViewDefinition [ReferenceView]'),'2;1');
FILE_NAME('wall.ifc','2026-11-12T00:00:00',(''),(''),'','','');
FILE_SCHEMA(('IFC4'));
ENDSEC;
DATA;
#1=IFCPROJECT('0YvctVUKr0kugbFTf53O9L',$,'Test Project',$,$,$,$,(#20),#7);
#2=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
#7=IFCUNITASSIGNMENT((#2));
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCAXIS2PLACEMENT3D(#10,$,$);
#20=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,#11,$);
#21=IFCPROJECTEDCRS('EPSG:32633',$,$,$,$,$,$);
#22=IFCMAPCONVERSION(#20,#21,500000.,4649776.,100.,1.,0.,1.);
#30=IFCSITE('1YvctVUKr0kugbFTf53O9L',$,'Site',$,$,#31,$,$,.ELEMENT.,$,$,$,$,$);
#31=IFCLOCALPLACEMENT($,#11);
#40=IFCBUILDING('2YvctVUKr0kugbFTf53O9L',$,'Building',$,$,#41,$,$,.ELEMENT.,$,$,$);
#41=IFCLOCALPLACEMENT(#31,#11);
#50=IFCBUILDINGSTOREY('3YvctVUKr0kugbFTf53O9L',$,'Ground',$,$,#51,$,$,.ELEMENT.,0.);
#51=IFCLOCALPLACEMENT(#41,#11);
#60=IFCRELAGGREGATES('4YvctVUKr0kugbFTf53O9L',$,$,$,#1,(#30));
#61=IFCRELAGGREGATES('5YvctVUKr0kugbFTf53O9L',$,$,$,#30,(#40));
#62=IFCRELAGGREGATES('6YvctVUKr0kugbFTf53O9L',$,$,$,#40,(#50));
#70=IFCWALL('7YvctVUKr0kugbFTf53O9L',$,'Wall A',$,$,#71,#80,$,.STANDARD.);
#71=IFCLOCALPLACEMENT(#51,#72);
#72=IFCAXIS2PLACEMENT3D(#73,$,$);
#73=IFCCARTESIANPOINT((10.,20.,0.));
#80=IFCPRODUCTDEFINITIONSHAPE($,$,(#81));
#81=IFCSHAPEREPRESENTATION(#20,'Body','SweptSolid',(#82));
#82=IFCEXTRUDEDAREASOLID(#83,#11,#86,3.);
#83=IFCRECTANGLEPROFILEDEF(.AREA.,$,#84,4.,0.2);
#84=IFCAXIS2PLACEMENT2D(#85,$);
#85=IFCCARTESIANPOINT((0.,0.));
#86=IFCDIRECTION((0.,0.,1.));
#90=IFCRELCONTAINEDINSPATIALSTRUCTURE('8YvctVUKr0kugbFTf53O9L',$,$,$,(#70),#50);
#100=IFCPROPERTYSINGLEVALUE('FireRating',$,IFCLABEL('REI60'),$);
#101=IFCPROPERTYSINGLEVALUE('IsExternal',$,IFCBOOLEAN(.T.),$);
#102=IFCPROPERTYSINGLEVALUE('ThermalTransmittance',$,IFCTHERMALTRANSMITTANCEMEASURE(0.35),$);
#103=IFCPROPERTYSET('9YvctVUKr0kugbFTf53O9L',$,'Pset_WallCommon',$,(#100,#101,#102));
#104=IFCRELDEFINESBYPROPERTIES('AYvctVUKr0kugbFTf53O9L',$,$,$,(#70),#103);
ENDSEC;
END-ISO-10303-21;
)";
} // namespace

TEST_CASE("an IFC wall comes in under its storey, with its properties, on the grid its map conversion names")
{
    scene::georef::setCurrent(std::nullopt);
    const fs::path dir = fs::temp_directory_path() / "lila_ifc";
    fs::create_directories(dir);
    std::ofstream(dir / "wall.ifc", std::ios::binary) << kWall;
    const json r = ops::execute("importIfc", {{"args", {(dir / "wall.ifc").string()}}});
    CHECK(r["meshes"].get<int>() == 1);
    CHECK(r["groups"].get<int>() == 4);
    CHECK(r["georeference"].get<std::string>().find("IfcMapConversion") != std::string::npos);
    const auto geo = scene::georef::current();
    REQUIRE(geo);
    CHECK(geo->crs == "EPSG:32633");
    CHECK(geo->origin == glm::dvec3(500000.0, 4649776.0, 100.0));

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::Node* wall = g.find("Wall_A") ? g.find("Wall_A") : g.find("Wall A");
    REQUIRE(wall);
    const scene::Node* storey = g.node(g.parent(wall->id()));
    REQUIRE(storey);
    CHECK(storey->name() == "Ground");
    CHECK(wall->as<std::string>(wall->type().attrIndex("ifcType")) == "IfcWall");
    CHECK(wall->as<std::string>(wall->type().attrIndex("Pset_WallCommon_FireRating")) == "REI60");
    CHECK(wall->as<bool>(wall->type().attrIndex("Pset_WallCommon_IsExternal")));
    CHECK(wall->as<double>(wall->type().attrIndex("Pset_WallCommon_ThermalTransmittance")) == doctest::Approx(0.35));

    // The wall's box on the UTM grid: 4 m along easting around 500010, at northing 4649796, 3 m up from 100.
    const scene::MeshPtr m = wall->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    const glm::dmat4 world = wall->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
        {
            const glm::dvec3 q = scene::georef::toProjected(*geo, glm::dvec3(world * glm::dvec4(m->position(p), 1.0)), scene::georef::sceneZUp());
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
    CHECK(lo.x == doctest::Approx(500008.0).epsilon(1e-9));
    CHECK(hi.x == doctest::Approx(500012.0).epsilon(1e-9));
    CHECK(lo.y == doctest::Approx(4649795.9).epsilon(1e-9));
    CHECK(hi.y == doctest::Approx(4649796.1).epsilon(1e-9));
    CHECK(lo.z == doctest::Approx(100.0).epsilon(1e-9));
    CHECK(hi.z == doctest::Approx(103.0).epsilon(1e-9));
    CHECK(m->numPoints() == 8);
    scene::georef::setCurrent(std::nullopt);
}
