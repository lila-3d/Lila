#include "Isolate.h"

#include "io/SceneFile.h"
#include "python/Python.h"
#include "scene/SceneCommands.h"

#include <doctest/doctest.h>

using namespace lila;

// Python can't start again after the python extension test shuts it down, so this runs as its own ctest.
TEST_CASE("python nodes API: instances and arrays" * doctest::skip())
{
    isolate();
    scene::registerCommands();
    io::registerCommands();
    REQUIRE(python::init({"lila_extensions_tests"}));
    python::Result r = python::run("from lila import nodes, ops\n"
                                   "ops.newScene()\n"
                                   "src = nodes.node(ops.polyCube(name='Bolt')[0])\n"
                                   "a = nodes.instance(src)\n"
                                   "b, c = nodes.instance(src, a)\n"
                                   "r = nodes.instance(src, reference=True, name='BoltRef')\n"
                                   "assert a.is_instance and b.is_instance and r.is_instance\n"
                                   "assert not src.is_instance and src.instance_kind == 'source'\n"
                                   "assert a.instance_kind == 'instance' and r.instance_kind == 'reference'\n"
                                   "assert c.instance_source == src, c.instance_source\n"
                                   "assert r.name == 'BoltRef'\n"
                                   "assert set(src.instances()) == {a, b, c, r}, src.instances()\n"
                                   "assert src not in src.instances()\n"
                                   "a.make_unique()\n"
                                   "assert not a.is_instance and a.instance_source is None and a.instances() == []\n"
                                   "loc = nodes.locator()\n"
                                   "assert not loc.is_instance and loc.instances() == []\n"
                                   "spoke = nodes.node(ops.polyCube(name='Spoke')[0])\n"
                                   "spoke.move(100, 0, 0)\n"
                                   "arr = nodes.array(spoke, mode='radial', count=6, center=(0, 0, 0), axis='y', name='Wheel')\n"
                                   "assert arr.is_array and not spoke.is_array and arr.name == 'Wheel'\n"
                                   "assert arr.array_sources() == [spoke]\n"
                                   "assert len(arr.array_copies()[spoke]) == 5, arr.array_copies()\n"
                                   "arr.count = 4\n"
                                   "assert arr.count == 4 and len(arr.array_copies()[spoke]) == 3\n"
                                   "row = nodes.array(nodes.node(ops.polyCube()[0]), count=3, offset=(50, 0, 0))\n"
                                   "assert len(row.array_copies()[row.array_sources()[0]]) == 2\n"
                                   "grp = arr.bake_array()\n"
                                   "assert not grp.is_array and grp.name == 'Wheel' and len(grp.children) == 4, grp.children\n"
                                   "assert all(c.is_instance for c in grp.children if c != spoke)\n");
    CHECK(r.ok);
    python::shutdown();
}
