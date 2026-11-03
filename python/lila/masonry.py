"""Masonry from Python with typed parameters and dataclass results: walls, buildings, ruin, age, checks and Unreal.

    import lila.masonry as masonry
    w = masonry.wall([[0, 0, 0], [800, 0, 0], [800, 0, 600]], bond="flemish", height=300)
    print(w.stones, w.regions)
    r = masonry.ruin(w.mesh, left=0.5, collapse=90)
    print(r.removed, r.left)
    c = masonry.check(w.mesh)
    assert c.ok, c.issues

Every function runs the masonry command of the same name (masonryWall, masonryBuilding, ruin, age, masonryCheck,
masonryShip, ...), so it is undoable and echoed. Settings not named here pass through as keyword arguments.
"""

from dataclasses import dataclass, field
from typing import Any, Optional, Sequence

from . import ops

__all__ = ["Build", "RuinStats", "AgeStats", "Check", "Ship", "Wedge", "wall", "from_mesh", "building", "spec", "info", "ruin", "paint",
           "age", "check", "ship", "wedge", "make_destructible"]


@dataclass
class Build:
    """What masonryWall, masonryFromMesh and masonryBuilding return."""
    mesh: str
    modifier: str
    stones: int
    courses: int
    regions: dict
    roles: dict
    triangles: int
    collision_hulls: int
    ruin: dict
    raw: dict = field(repr=False)

    @staticmethod
    def of(r: dict) -> "Build":
        mod = r.get("modifier", "")
        return Build(mesh=r.get("mesh", ""), modifier=mod if isinstance(mod, str) else mod[0], stones=r.get("stones", 0), courses=r.get("courses", 0),
                     regions=r.get("regions", {}), roles=r.get("roles", {}), triangles=r.get("stoneTriangles", 0),
                     collision_hulls=r.get("collisionHulls", 0), ruin=r.get("ruin", {}), raw=r)


@dataclass
class RuinStats:
    """What a ruin did: stones removed, survivors, loose, unsupported, rubble, slab pieces, painted, unstable, and the share left."""
    removed: int
    survivors: int
    loose: int
    unsupported: int
    rubble: int
    slab_pieces: int
    fallen_pieces: int
    painted: int
    unstable: int
    left: float
    raw: dict = field(repr=False)

    @staticmethod
    def of(r: dict) -> "RuinStats":
        return RuinStats(removed=r.get("removed", 0), survivors=r.get("survivors", 0), loose=r.get("loose", 0), unsupported=r.get("unsupported", 0),
                         rubble=r.get("rubble", 0), slab_pieces=r.get("slabPieces", 0), fallen_pieces=r.get("fallenPieces", 0),
                         painted=r.get("painted", 0), unstable=r.get("unstable", 0), left=r.get("left", 1.0), raw=r)


@dataclass
class AgeStats:
    stones: int
    mossy: int
    mean_age: float
    mean_wet: float
    mean_moss: float


@dataclass
class Check:
    """masonryCheck: ok, the issues as text, and the counts behind them."""
    ok: bool
    issues: list
    floating: int
    overlaps: int
    collision_hulls: int
    stones: int
    triangles: int
    floating_stones: list


@dataclass
class Ship:
    stones: int
    assets: int
    groups: int
    chunks: int
    nanite: bool
    raw: dict = field(repr=False)


@dataclass
class Wedge:
    """masonryWedge: each variant's changed settings and stats, and the contact sheet."""
    variants: list
    sheet: str
    columns: int


def wall(points: Optional[Sequence[Sequence[float]]] = None, mesh: Optional[str] = None, closed: bool = False, heights: Optional[Sequence[float]] = None,
         bond: str = "running", height: float = 200.0, thickness: float = 21.5, openings: Optional[list] = None, **settings: Any) -> Build:
    """A wall along points (or a mesh's loose edges). Every masonryWall flag is accepted as a keyword."""
    kw = dict(settings, bond=bond, height=height, thickness=thickness, closed=closed)
    if heights is not None:
        kw["heights"] = list(heights)
    if openings is not None:
        kw["openings"] = openings
    if mesh is not None:
        return Build.of(ops.masonryWall(mesh, **kw))
    return Build.of(ops.masonryWall(points=[list(p) for p in points], **kw))


def from_mesh(blockout: str, thickness: float = 0.0, band: float = 0.0, **settings: Any) -> Build:
    """Walls laid over a blockout mesh, slice by slice."""
    r = ops.masonryFromMesh(blockout, thickness=thickness, band=band, **settings)
    return Build.of(r)


def building(spec: dict, node: Optional[str] = None, **settings: Any) -> Build:
    """A building from a spec (see lila://masonry/spec-schema); node= rebuilds an existing one."""
    if node is not None:
        settings["node"] = node
    return Build.of(ops.masonryBuilding(spec=spec, **settings))


def spec(node: str) -> dict:
    """A building's spec, settings and regions, to edit and pass back to building(node=...)."""
    return ops.masonrySpec(node)


def info(node: str) -> Build:
    return Build.of(ops.masonryInfo(node))


def ruin(node, left: Optional[float] = None, collapse: Optional[float] = None, regions: Optional[Sequence[str]] = None, **settings: Any) -> RuinStats:
    """Ruin a wall or building (or a list of hand-placed objects); only the settings given change."""
    if left is not None:
        settings["left"] = left
    if collapse is not None:
        settings["collapse"] = collapse
    if regions is not None:
        settings["regions"] = list(regions)
    return RuinStats.of(ops.ruin(node, **settings))


def paint(node: str, points: Sequence[Sequence[float]], radius: float = 50.0, strength: float = 1.0) -> RuinStats:
    """A ruin brush stroke: stones near the points fall (strength below 0 holds them up)."""
    return RuinStats.of(ops.ruinPaint(node, points=[list(p) for p in points], radius=radius, strength=strength))


def age(node: str, age: Optional[float] = None, **settings: Any) -> AgeStats:
    if age is not None:
        settings["age"] = age
    r = ops.age(node, **settings)
    return AgeStats(stones=r["stones"], mossy=r["mossy"], mean_age=r["meanAge"], mean_wet=r["meanWet"], mean_moss=r["meanMoss"])


def check(node: str, tolerance: float = 0.1, max_stones: int = 0, max_triangles: int = 0) -> Check:
    r = ops.masonryCheck(node, tolerance=tolerance, maxStones=max_stones, maxTriangles=max_triangles)
    return Check(ok=r["ok"], issues=r["issues"], floating=r["floating"], overlaps=r["overlaps"], collision_hulls=r["collisionHulls"],
                 stones=r["stones"], triangles=r["triangles"], floating_stones=r.get("floatingStones", []))


def ship(node: str, mode: str = "hism", wait: bool = True, **settings: Any) -> Ship:
    r = ops.masonryShip(node, mode=mode, wait=wait, **settings)
    return Ship(stones=r["stones"], assets=r["assets"], groups=r["groups"], chunks=r["chunks"], nanite=r.get("nanite", False), raw=r)


def wedge(node: str, count: int = 9, vary: Any = None, **settings: Any):
    """count variants of node side by side in a contact sheet; wedge(node, count, apply=k) sets variant k on it."""
    if vary is not None:
        settings["vary"] = vary
    r = ops.masonryWedge(node, count=count, **settings)
    if "variants" not in r:
        return r
    return Wedge(variants=r["variants"], sheet=r["sheet"], columns=r["columns"])


def make_destructible(node: str, strength: float = 30.0, **settings: Any) -> dict:
    return ops.makeDestructible(node, strength=strength, **settings)
