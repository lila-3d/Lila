"""Builds the rig fixtures in this folder with mayapy and records what Maya evaluates for each pose.

Run: mayapy make_rig_fixtures.py
Each fixture is <name>.ma with <name>.json beside it: the driver plugs with their rest values, and per pose the plugs
set, the world matrices (Maya's row-vector order, 16 numbers) of the checked transforms and the world positions of the
checked meshes' vertices.
"""

import json
import math
import os
import sys

import maya.standalone

maya.standalone.initialize(name="python")

import maya.api.OpenMaya as om  # noqa: E402
import maya.cmds as mc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
for plugin in ("matrixNodes", "quatNodes", "lookdevKit"):
    mc.loadPlugin(plugin, quiet=True)


def new_scene():
    mc.file(new=True, force=True)
    mc.currentUnit(linear="cm", angle="deg", time="film")


def loc(name, t=(0, 0, 0), r=(0, 0, 0), parent=None):
    n = mc.spaceLocator(name=name)[0]
    if parent:
        n = mc.parent(n, parent)[0]
    mc.setAttr(n + ".t", *t)
    mc.setAttr(n + ".r", *r)
    return n


def grp(name, t=(0, 0, 0), r=(0, 0, 0), parent=None):
    n = mc.group(empty=True, name=name)
    if parent:
        n = mc.parent(n, parent)[0]
    mc.setAttr(n + ".t", *t)
    mc.setAttr(n + ".r", *r)
    return n


def add(node, attr, kind="double", value=0.0, **kw):
    mc.addAttr(node, longName=attr, attributeType=kind, keyable=True, **kw)
    mc.setAttr(node + "." + attr, value)


def node(kind, name, **values):
    n = mc.createNode(kind, name=name)
    for k, v in values.items():
        if isinstance(v, (list, tuple)):
            mc.setAttr(n + "." + k, *v)
        else:
            mc.setAttr(n + "." + k, v)
    return n


def world_matrix(n):
    return [round(v, 9) for v in mc.getAttr(n + ".worldMatrix[0]")]


def mesh_points(n):
    sel = om.MSelectionList()
    sel.add(n)
    path = sel.getDagPath(0)
    path.extendToShape()
    pts = om.MFnMesh(path).getPoints(om.MSpace.kWorld)
    return [[round(p.x, 9), round(p.y, 9), round(p.z, 9)] for p in pts]


def record(name, drivers, poses, matrices=(), meshes=(), time_drivers=False):
    """Saves the scene, then evaluates every pose: rest values first, then the pose's own."""
    mc.file(rename=os.path.join(HERE, name + ".ma"))
    mc.file(save=True, type="mayaAscii", force=True)
    rest = {p: mc.getAttr(p) for p in drivers}
    out = {"drivers": rest, "poses": []}
    for pose in poses:
        for p, v in rest.items():
            mc.setAttr(p, v)
        for p, v in pose.items():
            if p == "time":
                mc.currentTime(v)
            else:
                mc.setAttr(p, v)
        entry = {"set": pose, "matrices": {m: world_matrix(m) for m in matrices}, "points": {m: mesh_points(m) for m in meshes}}
        out["poses"].append(entry)
    with open(os.path.join(HERE, name + ".json"), "w", newline="\n") as f:
        json.dump(out, f, indent=1)
    print("wrote", name)


# ---------------------------------------------------------------------------------------------------- utility nodes


def utility():
    new_scene()
    ctl = loc("ctl", (1, 2, 3), (10, 20, 30))
    add(ctl, "a", value=0.25)
    add(ctl, "b", value=0.5)
    add(ctl, "ang", "doubleAngle", value=15.0)
    add(ctl, "sel", "long", value=0)
    anchor = loc("anchor", (-2, 1, 4))
    outs = [loc("out%d" % i) for i in range(1, 20)]

    md = node("multiplyDivide", "md1", operation=1, input2=(2, 3, -1))
    mc.connectAttr(ctl + ".t", md + ".input1")
    mc.connectAttr(md + ".output", outs[0] + ".t")

    md2 = node("multiplyDivide", "md2", operation=2, input2X=4)
    mc.connectAttr(ctl + ".a", md2 + ".input1X")
    md3 = node("multiplyDivide", "md3", operation=3, input2X=2)
    mc.connectAttr(ctl + ".b", md3 + ".input1X")
    mc.connectAttr(md2 + ".outputX", outs[1] + ".tx")
    mc.connectAttr(md3 + ".outputX", outs[1] + ".ty")

    pma = node("plusMinusAverage", "pma1", operation=1)
    mc.connectAttr(ctl + ".t", pma + ".input3D[0]")
    mc.setAttr(pma + ".input3D[1]", 1, 1, 1)
    mc.connectAttr(ctl + ".tx", pma + ".input3D[2].input3Dy")
    mc.connectAttr(pma + ".output3D", outs[2] + ".t")
    pma2 = node("plusMinusAverage", "pma2", operation=2)
    mc.connectAttr(ctl + ".a", pma2 + ".input1D[0]")
    mc.connectAttr(ctl + ".b", pma2 + ".input1D[1]")
    mc.setAttr(pma2 + ".input1D[2]", 0.125)
    mc.connectAttr(pma2 + ".output1D", outs[2] + ".sx")
    pma3 = node("plusMinusAverage", "pma3", operation=3)
    mc.connectAttr(ctl + ".tx", pma3 + ".input2D[0].input2Dx")
    mc.setAttr(pma3 + ".input2D[1]", 3, 5)
    mc.connectAttr(pma3 + ".output2Dx", outs[2] + ".sy")
    mc.connectAttr(pma3 + ".output2Dy", outs[2] + ".sz")

    cond = node("condition", "cond1", operation=2, secondTerm=0.5, colorIfFalse=(-1, -2, -3))
    mc.connectAttr(ctl + ".a", cond + ".firstTerm")
    mc.connectAttr(ctl + ".t", cond + ".colorIfTrue")
    mc.connectAttr(cond + ".outColor", outs[3] + ".t")

    rev = node("reverse", "rev1")
    mc.connectAttr(ctl + ".t", rev + ".input")
    mc.connectAttr(rev + ".output", outs[4] + ".t")

    cl = node("clamp", "clamp1", min=(-1, -1, -1), max=(1, 2, 3))
    mc.connectAttr(ctl + ".t", cl + ".input")
    mc.connectAttr(cl + ".output", outs[5] + ".t")

    sr = node("setRange", "sr1", oldMin=(-2, -2, 0), oldMax=(2, 4, 6), min=(0, 10, -1), max=(1, 20, 1))
    mc.connectAttr(ctl + ".t", sr + ".value")
    mc.connectAttr(sr + ".outValue", outs[6] + ".t")

    rv = node("remapValue", "remap1", inputMin=0, inputMax=2, outputMin=-1, outputMax=3)
    mc.setAttr(rv + ".value[0].value_Position", 0)
    mc.setAttr(rv + ".value[0].value_FloatValue", 0)
    mc.setAttr(rv + ".value[0].value_Interp", 1)
    mc.setAttr(rv + ".value[1].value_Position", 0.5)
    mc.setAttr(rv + ".value[1].value_FloatValue", 1)
    mc.setAttr(rv + ".value[1].value_Interp", 1)
    mc.setAttr(rv + ".value[2].value_Position", 1)
    mc.setAttr(rv + ".value[2].value_FloatValue", 0.2)
    mc.setAttr(rv + ".value[2].value_Interp", 1)
    mc.connectAttr(ctl + ".tx", rv + ".inputValue")
    mc.connectAttr(rv + ".outValue", outs[7] + ".tx")

    bc = node("blendColors", "bc1", color2=(5, 6, 7))
    mc.connectAttr(ctl + ".b", bc + ".blender")
    mc.connectAttr(ctl + ".t", bc + ".color1")
    mc.connectAttr(bc + ".output", outs[8] + ".t")

    b2 = node("blendTwoAttr", "b2a1")
    mc.connectAttr(ctl + ".tx", b2 + ".input[0]")
    mc.connectAttr(ctl + ".ty", b2 + ".input[1]")
    mc.connectAttr(ctl + ".b", b2 + ".attributesBlender")
    mc.connectAttr(b2 + ".output", outs[9] + ".tx")

    db = node("distanceBetween", "dist1")
    mc.connectAttr(ctl + ".worldMatrix[0]", db + ".inMatrix1")
    mc.connectAttr(anchor + ".worldMatrix[0]", db + ".inMatrix2")
    mc.setAttr(db + ".point2", 0, 1, 0)
    mc.connectAttr(db + ".distance", outs[10] + ".tx")

    pb = node("pairBlend", "pb1", inTranslate2=(4, 5, 6), inRotate2=(30, 0, 45), rotInterpolation=1)
    mc.connectAttr(ctl + ".t", pb + ".inTranslate1")
    mc.connectAttr(ctl + ".r", pb + ".inRotate1")
    mc.connectAttr(ctl + ".b", pb + ".weight")
    mc.connectAttr(pb + ".outTranslate", outs[11] + ".t")
    mc.connectAttr(pb + ".outRotate", outs[11] + ".r")

    ch = node("choice", "choice1")
    mc.connectAttr(ctl + ".sel", ch + ".selector")
    mc.connectAttr(ctl + ".tx", ch + ".input[0]")
    mc.connectAttr(ctl + ".ty", ch + ".input[1]")
    mc.connectAttr(ch + ".output", outs[12] + ".tx")

    vp = node("vectorProduct", "vp1", operation=2, input2=(0, 1, 0))
    mc.connectAttr(ctl + ".t", vp + ".input1")
    mc.connectAttr(vp + ".output", outs[13] + ".t")
    vp2 = node("vectorProduct", "vp2", operation=4, input1=(1, 0, 0))
    mc.connectAttr(ctl + ".worldMatrix[0]", vp2 + ".matrix")
    mc.connectAttr(vp2 + ".output", outs[14] + ".t")

    md4 = node("multiplyDivide", "md4", input2X=2)
    mc.connectAttr(ctl + ".ang", md4 + ".input1X")
    mc.connectAttr(md4 + ".outputX", outs[15] + ".rz")
    mc.connectAttr(ctl + ".rx", md4 + ".input1Y")
    mc.connectAttr(md4 + ".outputY", outs[15] + ".tx")

    pma4 = node("plusMinusAverage", "pma4")
    mc.connectAttr(ctl + ".rx", pma4 + ".input1D[0]")
    mc.connectAttr(ctl + ".ry", pma4 + ".input1D[1]")
    mc.connectAttr(pma4 + ".output1D", outs[16] + ".ry")

    sr2 = node("setRange", "sr2", oldMax=(1, 0, 0), max=(90, 0, 0))
    mc.connectAttr(ctl + ".b", sr2 + ".valueX")
    mc.connectAttr(sr2 + ".outValueX", outs[17] + ".rx")

    ball = mc.polyCube(name="cpomBall", width=3, height=2, depth=4, subdivisionsX=2, subdivisionsY=2, subdivisionsZ=2,
                       constructionHistory=False)[0]
    mc.setAttr(ball + ".t", 0, 1, 0)
    mc.setAttr(ball + ".r", 0, 30, 0)
    cp = mc.createNode("closestPointOnMesh", name="cpom1")
    mc.connectAttr(mc.listRelatives(ball, shapes=True)[0] + ".worldMesh[0]", cp + ".inMesh")
    mc.connectAttr(ctl + ".t", cp + ".inPosition")
    mc.connectAttr(cp + ".position", outs[18] + ".t")

    drivers = [ctl + "." + a for a in ("translateX", "translateY", "translateZ", "rotateX", "rotateY", "rotateZ", "a", "b", "ang", "sel")]
    poses = [
        {},
        {ctl + ".translateX": 1.5, ctl + ".translateY": -3, ctl + ".a": 0.75, ctl + ".b": 0.2, ctl + ".sel": 1, ctl + ".ang": -40},
        {ctl + ".translateX": -0.4, ctl + ".translateZ": 7, ctl + ".rotateX": -60, ctl + ".rotateY": 95, ctl + ".a": 2.5, ctl + ".b": 0.9},
    ]
    record("rig_utility", drivers, poses, matrices=outs)


# ---------------------------------------------------------------------------------------------------- matrix nodes


def matrices():
    new_scene()
    ctl = loc("ctl", (1, 2, 3), (10, 20, 30))
    ctl2 = loc("ctlB", (-3, 1, 2), (0, 45, 0))
    add(ctl, "b", value=0.3)
    add(ctl, "sel", "long", value=0)
    base = loc("base", (0, 5, 0))
    outs = [loc("follow%d" % i) for i in range(1, 10)]
    holder = grp("holder", (2, 0, 0), (0, 0, 30))
    under = loc("follow10", parent=holder)
    outs.append(under)

    mm = node("multMatrix", "mm1")
    mc.connectAttr(ctl + ".worldMatrix[0]", mm + ".matrixIn[1]")
    mc.setAttr(mm + ".matrixIn[0]", [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1], type="matrix")
    mc.connectAttr(mm + ".matrixSum", outs[0] + ".offsetParentMatrix")

    dm = node("decomposeMatrix", "dm1")
    mc.setAttr(outs[1] + ".rotateOrder", 2)
    mc.connectAttr(ctl + ".worldMatrix[0]", dm + ".inputMatrix")
    mc.connectAttr(outs[1] + ".rotateOrder", dm + ".inputRotateOrder")
    mc.connectAttr(dm + ".outputTranslate", outs[1] + ".t")
    mc.connectAttr(dm + ".outputRotate", outs[1] + ".r")
    mc.connectAttr(dm + ".outputScale", outs[1] + ".s")

    cm = node("composeMatrix", "cm1", inputScale=(1, 2, 1), inputRotateOrder=3)
    mc.connectAttr(ctl + ".t", cm + ".inputTranslate")
    mc.connectAttr(ctl + ".r", cm + ".inputRotate")
    inv = node("inverseMatrix", "inv1")
    mc.connectAttr(cm + ".outputMatrix", inv + ".inputMatrix")
    mc.connectAttr(inv + ".outputMatrix", outs[2] + ".offsetParentMatrix")

    bm = node("blendMatrix", "bm1")
    mc.connectAttr(ctl + ".worldMatrix[0]", bm + ".inputMatrix")
    mc.connectAttr(ctl2 + ".worldMatrix[0]", bm + ".target[0].targetMatrix")
    mc.connectAttr(ctl + ".b", bm + ".target[0].weight")
    mc.connectAttr(base + ".worldMatrix[0]", bm + ".target[1].targetMatrix")
    mc.setAttr(bm + ".target[1].weight", 0.5)
    mc.setAttr(bm + ".target[1].rotateWeight", 0)
    mc.connectAttr(bm + ".outputMatrix", outs[3] + ".offsetParentMatrix")

    pk = node("pickMatrix", "pick1", useRotate=False, useScale=False)
    mc.connectAttr(ctl + ".worldMatrix[0]", pk + ".inputMatrix")
    mc.connectAttr(pk + ".outputMatrix", outs[4] + ".offsetParentMatrix")

    am = node("aimMatrix", "aim1")
    mc.connectAttr(base + ".worldMatrix[0]", am + ".inputMatrix")
    mc.connectAttr(ctl2 + ".worldMatrix[0]", am + ".primary.primaryTargetMatrix")
    mc.setAttr(am + ".primary.primaryInputAxis", 0, 0, 1)
    mc.setAttr(am + ".secondary.secondaryMode", 2)
    mc.setAttr(am + ".secondary.secondaryInputAxis", 0, 1, 0)
    mc.setAttr(am + ".secondary.secondaryTargetVector", 0, 1, 0)
    mc.connectAttr(ctl + ".worldMatrix[0]", am + ".secondary.secondaryTargetMatrix")
    mc.connectAttr(am + ".outputMatrix", outs[5] + ".offsetParentMatrix")

    wt = node("wtAddMatrix", "wt1")
    mc.connectAttr(ctl + ".worldMatrix[0]", wt + ".wtMatrix[0].matrixIn")
    mc.setAttr(wt + ".wtMatrix[0].weightIn", 0.25)
    mc.connectAttr(ctl2 + ".worldMatrix[0]", wt + ".wtMatrix[1].matrixIn")
    mc.setAttr(wt + ".wtMatrix[1].weightIn", 0.75)
    mc.connectAttr(wt + ".matrixSum", outs[6] + ".offsetParentMatrix")

    ff = node("fourByFourMatrix", "ff1")
    mc.connectAttr(ctl + ".tx", ff + ".in30")
    mc.connectAttr(ctl + ".ty", ff + ".in31")
    mc.connectAttr(ctl + ".b", ff + ".in00")
    mc.connectAttr(ff + ".output", outs[7] + ".offsetParentMatrix")

    ch = node("choice", "choiceM")
    mc.connectAttr(ctl + ".sel", ch + ".selector")
    mc.connectAttr(ctl + ".worldMatrix[0]", ch + ".input[0]")
    mc.connectAttr(ctl2 + ".worldMatrix[0]", ch + ".input[1]")
    mc.connectAttr(ch + ".output", outs[8] + ".offsetParentMatrix")

    mm2 = node("multMatrix", "mm2")
    mc.connectAttr(ctl + ".worldMatrix[0]", mm2 + ".matrixIn[0]")
    mc.connectAttr(holder + ".worldInverseMatrix[0]", mm2 + ".matrixIn[1]")
    mc.connectAttr(mm2 + ".matrixSum", under + ".offsetParentMatrix")
    mc.setAttr(under + ".t", 0, 1, 0)

    drivers = [ctl + "." + a for a in ("translateX", "translateY", "translateZ", "rotateX", "rotateY", "rotateZ", "scaleY", "b", "sel")]
    drivers += [ctl2 + ".translateX", ctl2 + ".rotateZ", holder + ".rotateY"]
    poses = [
        {},
        {ctl + ".translateX": -2, ctl + ".rotateY": -70, ctl + ".scaleY": 1.5, ctl + ".b": 0.8, ctl + ".sel": 1, ctl2 + ".translateX": 2},
        {ctl + ".translateZ": -4, ctl + ".rotateX": 120, ctl + ".b": 0.0, ctl2 + ".rotateZ": 60, holder + ".rotateY": 45},
    ]
    record("rig_matrix", drivers, poses, matrices=outs)


# ---------------------------------------------------------------------------------------------------- constraints


def constraints():
    new_scene()
    ta = loc("tgtA", (1, 2, 3), (10, 20, 30))
    tb = loc("tgtB", (-1, 0, 2), (0, 0, 45))
    mc.setAttr(tb + ".s", 1, 2, 1)
    up = loc("upObj", (0, 10, 0), (0, 0, 20))
    ctl = loc("ctl")
    add(ctl, "blend", value=0.0)

    o1 = loc("obj1", (0, 1, 0))
    mc.setAttr(o1 + ".rotateOrder", 1)
    mc.setAttr(o1 + ".rotatePivot", 0, 0, 1)
    pc = mc.parentConstraint(ta, tb, o1, maintainOffset=True)[0]
    mc.setAttr(pc + ".tgtBW1", 0.25)
    mc.setDrivenKeyframe(pc + ".tgtAW0", currentDriver=ctl + ".blend", driverValue=0, value=1)
    mc.setDrivenKeyframe(pc + ".tgtAW0", currentDriver=ctl + ".blend", driverValue=1, value=0)

    o2 = loc("obj2", (5, 5, 5))
    mc.pointConstraint(ta, o2, offset=(1, 0, 0), skip="y")

    mc.select(clear=True)
    jc = mc.joint(name="jointC", position=(2, 0, 0))
    mc.setAttr(jc + ".jointOrient", 0, 0, 45)
    mc.setAttr(jc + ".rotateOrder", 3)
    mc.orientConstraint(ta, tb, jc, maintainOffset=True)

    o4 = loc("obj4", (3, 3, -2))
    mc.aimConstraint(tb, o4, aimVector=(0, 1, 0), upVector=(1, 0, 0), worldUpType="objectrotation", worldUpObject=up, worldUpVector=(0, 0, 1),
                     offset=(10, 0, 0))

    o5 = loc("obj5")
    mc.scaleConstraint(ta, tb, o5)

    g6 = grp("grp6", (0, 0, -5), (0, 30, 0))
    o6 = loc("obj6", (1, 1, 1), parent=g6)
    mc.parentConstraint(ta, o6, maintainOffset=True)

    o7 = loc("obj7", (0, 4, 0))
    mc.orientConstraint(ta, o7, offset=(0, 90, 0))

    drivers = [ta + "." + a for a in ("translateX", "translateY", "translateZ", "rotateX", "rotateY", "rotateZ", "scaleX")]
    drivers += [tb + "." + a for a in ("translateX", "translateZ", "rotateY", "scaleZ")]
    drivers += [up + ".rotateX", g6 + ".translateX", g6 + ".rotateY", ctl + ".blend"]
    poses = [
        {},
        {ta + ".translateX": 4, ta + ".rotateY": 80, tb + ".translateZ": -3, up + ".rotateX": 30, g6 + ".rotateY": -40},
        {ta + ".rotateX": -50, ta + ".scaleX": 2, tb + ".rotateY": 120, tb + ".scaleZ": 0.5, g6 + ".translateX": 3, ctl + ".blend": 0.5},
        {ta + ".translateY": -2, tb + ".translateX": 6, ctl + ".blend": 1.0},
    ]
    record("rig_constraints", drivers, poses, matrices=[o1, o2, jc, o4, o5, o6, o7])


# ---------------------------------------------------------------------------------------------------- IK


def ik():
    new_scene()
    mc.select(clear=True)
    hip = mc.joint(name="hip", position=(0, 10, 0))
    knee = mc.joint(name="knee", position=(0, 5, 1))
    ankle = mc.joint(name="ankle", position=(0, 0, 0))
    mc.joint(hip, edit=True, orientJoint="xyz", secondaryAxisOrient="yup", children=True, zeroScaleOrient=True)
    foot = loc("footCtl", (0, 0, 0))
    h = mc.ikHandle(startJoint=hip, endEffector=ankle, solver="ikRPsolver", name="legIk")[0]
    mc.parent(h, foot)
    pv = loc("pvCtl", (0, 5, 8))
    mc.poleVectorConstraint(pv, h)
    add(foot, "twist", "doubleAngle", value=0.0)
    mc.connectAttr(foot + ".twist", h + ".twist")

    mc.select(clear=True)
    sh = mc.joint(name="shoulder", position=(6, 10, 0))
    el = mc.joint(name="elbow", position=(9, 10, -1))
    wr = mc.joint(name="wrist", position=(12, 10, 0))
    mc.joint(sh, edit=True, orientJoint="xyz", secondaryAxisOrient="yup", children=True, zeroScaleOrient=True)
    hand = loc("handCtl", (12, 10, 0))
    ah = mc.ikHandle(startJoint=sh, endEffector=wr, solver="ikSCsolver", name="armIk")[0]
    mc.parent(ah, hand)

    # A stretchy leg: past its length the knee and ankle translates grow by distance / length.
    mc.select(clear=True)
    s1 = mc.joint(name="stretchHip", position=(20, 10, 0))
    s2 = mc.joint(name="stretchKnee", position=(20, 5, 1))
    s3 = mc.joint(name="stretchAnkle", position=(20, 0, 0))
    mc.joint(s1, edit=True, orientJoint="xyz", secondaryAxisOrient="yup", children=True, zeroScaleOrient=True)
    stretchCtl = loc("stretchCtl", (20, 0, 0))
    root = loc("stretchRoot", (20, 10, 0))
    sh_ = mc.ikHandle(startJoint=s1, endEffector=s3, solver="ikRPsolver", name="stretchIk")[0]
    mc.parent(sh_, stretchCtl)
    length = mc.getAttr(s2 + ".tx") + mc.getAttr(s3 + ".tx")
    db = node("distanceBetween", "stretchDist")
    mc.connectAttr(root + ".worldMatrix[0]", db + ".inMatrix1")
    mc.connectAttr(stretchCtl + ".worldMatrix[0]", db + ".inMatrix2")
    ratio = node("multiplyDivide", "stretchRatio", operation=2, input2X=length)
    mc.connectAttr(db + ".distance", ratio + ".input1X")
    cond = node("condition", "stretchCond", operation=2, secondTerm=length, colorIfFalseR=1)
    mc.connectAttr(db + ".distance", cond + ".firstTerm")
    mc.connectAttr(ratio + ".outputX", cond + ".colorIfTrueR")
    lens = node("multiplyDivide", "stretchLengths", input2X=mc.getAttr(s2 + ".tx"), input2Y=mc.getAttr(s3 + ".tx"))
    mc.connectAttr(cond + ".outColorR", lens + ".input1X")
    mc.connectAttr(cond + ".outColorR", lens + ".input1Y")
    mc.connectAttr(lens + ".outputX", s2 + ".tx")
    mc.connectAttr(lens + ".outputY", s3 + ".tx")

    drivers = [foot + "." + a for a in ("translateX", "translateY", "translateZ")]
    drivers += [pv + ".translateX", pv + ".translateZ", foot + ".twist", hand + ".translateX", hand + ".translateY", hand + ".translateZ"]
    drivers += [stretchCtl + ".translateY", stretchCtl + ".translateZ"]
    poses = [
        {},
        {foot + ".translateY": 3, foot + ".translateZ": 2},
        {foot + ".translateX": 2, foot + ".translateY": 1, pv + ".translateX": 6},
        {foot + ".translateY": 4, foot + ".twist": 30},
        {foot + ".translateY": -3},
        {hand + ".translateX": 10, hand + ".translateY": 11, hand + ".translateZ": 1},
        {hand + ".translateX": 11, hand + ".translateY": 8.5, hand + ".translateZ": -1},
        {stretchCtl + ".translateY": 2, stretchCtl + ".translateZ": 1},
        {stretchCtl + ".translateY": -4, stretchCtl + ".translateZ": 2},
    ]
    record("rig_ik", drivers, poses, matrices=[hip, knee, ankle, sh, el, wr, s1, s2, s3])


# ---------------------------------------------------------------------------------------------------- set driven keys


def sdk():
    new_scene()
    ctl = loc("ctl")
    add(ctl, "curl", value=0.0)
    mc.select(clear=True)
    j1 = mc.joint(name="j1", position=(0, 0, 0))
    j2 = mc.joint(name="j2", position=(0, 3, 0))
    tip = loc("tip", (0, 2, 0), parent=j2)

    mc.setDrivenKeyframe(j1 + ".ty", currentDriver=ctl + ".tx", driverValue=0, value=0, inTangentType="linear", outTangentType="linear")
    mc.setDrivenKeyframe(j1 + ".ty", currentDriver=ctl + ".tx", driverValue=2, value=4, inTangentType="linear", outTangentType="linear")
    mc.setDrivenKeyframe(j1 + ".ty", currentDriver=ctl + ".tz", driverValue=0, value=0, inTangentType="spline", outTangentType="spline")
    mc.setDrivenKeyframe(j1 + ".ty", currentDriver=ctl + ".tz", driverValue=1, value=-1, inTangentType="spline", outTangentType="spline")
    mc.setDrivenKeyframe(j1 + ".ty", currentDriver=ctl + ".tz", driverValue=3, value=2, inTangentType="spline", outTangentType="spline")
    for curve in mc.listConnections(ctl + ".tz", type="animCurve") or []:
        mc.setAttr(curve + ".preInfinity", 1)
        mc.setAttr(curve + ".postInfinity", 1)

    mc.setDrivenKeyframe(j2 + ".rz", currentDriver=ctl + ".rx", driverValue=0, value=0, inTangentType="linear", outTangentType="linear")
    mc.setDrivenKeyframe(j2 + ".rz", currentDriver=ctl + ".rx", driverValue=90, value=-45, inTangentType="linear", outTangentType="linear")
    mc.setDrivenKeyframe(j2 + ".rz", currentDriver=ctl + ".curl", driverValue=0, value=0, inTangentType="flat", outTangentType="flat")
    mc.setDrivenKeyframe(j2 + ".rz", currentDriver=ctl + ".curl", driverValue=10, value=60, inTangentType="flat", outTangentType="flat")

    mc.setDrivenKeyframe(j1 + ".rx", currentDriver=ctl + ".ty", driverValue=-1, value=-30, inTangentType="clamped", outTangentType="clamped")
    mc.setDrivenKeyframe(j1 + ".rx", currentDriver=ctl + ".ty", driverValue=1, value=30, inTangentType="clamped", outTangentType="clamped")

    drivers = [ctl + "." + a for a in ("translateX", "translateY", "translateZ", "rotateX", "curl")]
    poses = [
        {},
        {ctl + ".translateX": 1},
        {ctl + ".translateX": 3, ctl + ".translateZ": 0.5},
        {ctl + ".translateZ": 2, ctl + ".rotateX": 45, ctl + ".curl": 5},
        {ctl + ".translateZ": -1, ctl + ".rotateX": 120, ctl + ".curl": 12, ctl + ".translateY": 0.4},
    ]
    record("rig_sdk", drivers, poses, matrices=[j1, j2, tip])


# ---------------------------------------------------------------------------------------------------- deformers


def plane(name, t=(0, 0, 0), sx=4, sy=4, w=4.0, h=4.0):
    p = mc.polyPlane(name=name, width=w, height=h, subdivisionsX=sx, subdivisionsY=sy, constructionHistory=False)[0]
    mc.setAttr(p + ".t", *t)
    mc.makeIdentity(p, apply=True, translate=True)
    return p


def cube(name, t=(0, 0, 0), size=2.0, div=3):
    c = mc.polyCube(name=name, width=size, height=size, depth=size, subdivisionsX=div, subdivisionsY=div, subdivisionsZ=div,
                    constructionHistory=False)[0]
    mc.setAttr(c + ".t", *t)
    mc.makeIdentity(c, apply=True, translate=True)
    return c


def deformers():
    new_scene()
    p1 = plane("clusterPlane")
    cl, clh = mc.cluster(p1 + ".vtx[0:12]", name="clu")
    cl = mc.listConnections(clh + ".worldMatrix[0]", type="cluster")[0]
    mc.percent(cl, p1 + ".vtx[5:9]", value=0.5)

    mc.optionVar(intValue=("deformationUseComponentTags", 0))
    p1b = plane("clusterPlaneB", (0, 0, 10))
    clb, clhb = mc.cluster(p1b + ".vtx[0:4]", name="cluB")
    mc.optionVar(intValue=("deformationUseComponentTags", 1))

    c2 = cube("latCube", (10, 0, 0))
    ffd, lat, latBase = mc.lattice(c2, name="lat", divisions=(2, 3, 2), objectCentered=True)
    mc.move(0.3, 0, 0.2, lat + ".pt[1][1][0]", relative=True, objectSpace=True)
    mc.move(-0.2, 0.4, 0, lat + ".pt[0][2][1]", relative=True, objectSpace=True)

    c3 = cube("bendCube", (0, 0, -10), div=4)
    bend, bendH = mc.nonLinear(c3, type="bend", name="bnd", curvature=60)
    c4 = cube("twistCube", (10, 0, -10), div=4)
    tw, twH = mc.nonLinear(c4, type="twist", name="twst", startAngle=-30, endAngle=90)
    c5 = cube("squashCube", (20, 0, -10), div=4)
    sq, sqH = mc.nonLinear(c5, type="squash", name="sqsh", factor=-0.4)
    c6 = plane("sinePlane", (20, 0, 0), sx=8, sy=8)
    sn, snH = mc.nonLinear(c6, type="sine", name="sne", amplitude=0.3, wavelength=1.5)
    c7 = plane("wavePlane", (20, 0, 10), sx=8, sy=8)
    wv, wvH = mc.nonLinear(c7, type="wave", name="wve", amplitude=0.2, wavelength=0.8)
    c8 = cube("flareCube", (30, 0, 0), div=4)
    fl, flH = mc.nonLinear(c8, type="flare", name="flr", startFlareX=1.5, startFlareZ=1.5, endFlareX=0.5, endFlareZ=0.5)

    p9 = plane("softPlane", (10, 0, 10), sx=8, sy=8)
    sm, smH = mc.softMod(p9 + ".vtx[40]", name="soft", falloffRadius=1.5)

    drivers = [clh + ".translateY", clh + ".rotateZ", clhb + ".translateX", lat + ".translateY", lat + ".scaleX", bendH + ".rotateY",
               twH + ".translateY", sqH + ".scaleY", snH + ".rotateZ", wvH + ".translateX", flH + ".translateY", smH + ".translateY"]
    poses = [
        {},
        {clh + ".translateY": 2, clh + ".rotateZ": 20, clhb + ".translateX": -1, lat + ".translateY": 0.5, lat + ".scaleX": 1.4,
         bendH + ".rotateY": 30, twH + ".translateY": 0.3, sqH + ".scaleY": 1.3, snH + ".rotateZ": 15, wvH + ".translateX": 0.4,
         flH + ".translateY": 0.25, smH + ".translateY": 1},
    ]
    meshes = [p1, p1b, c2, c3, c4, c5, c6, c7, c8, p9]
    record("rig_deformers", drivers, poses, meshes=meshes)


def surface_deformers():
    new_scene()
    mc.select(clear=True)
    j1 = mc.joint(name="root", position=(0, 0, 0))
    j2 = mc.joint(name="mid", position=(0, 3, 0))
    mc.joint(name="end", position=(0, 6, 0))
    cyl = mc.polyCylinder(name="tube", radius=1, height=6, subdivisionsAxis=12, subdivisionsHeight=12, constructionHistory=False)[0]
    mc.setAttr(cyl + ".ty", 3)
    mc.makeIdentity(cyl, apply=True, translate=True)
    mc.skinCluster(j1, j2, cyl, name="tubeSkin", maximumInfluences=2)
    mc.deltaMush(cyl, name="tubeMush", smoothingIterations=8)

    drv = cube("driver", (8, 0, 0), size=2, div=2)
    mc.select(clear=True)
    d1 = mc.joint(name="driverJoint", position=(8, -1, 0))
    mc.skinCluster(d1, drv, name="driverSkin")
    ball = mc.polySphere(name="ball", radius=0.9, subdivisionsAxis=12, subdivisionsHeight=8, constructionHistory=False)[0]
    mc.setAttr(ball + ".tx", 8)
    mc.makeIdentity(ball, apply=True, translate=True)
    mc.select(ball, drv)
    import maya.mel as mel

    mel.eval('doWrapArgList "7" { "1", "0", "1", "2", "0", "1", "0", "0" }')

    drivers = [j2 + ".rotateZ", d1 + ".rotateZ", d1 + ".translateY"]
    poses = [{}, {j2 + ".rotateZ": 60, d1 + ".rotateZ": 25, d1 + ".translateY": 0.5}]
    record("rig_surface_deformers", drivers, poses, meshes=[cyl, ball])


# ---------------------------------------------------------------------------------------------------- pins


def pins():
    new_scene()
    p = plane("pinPlane", sx=4, sy=4)
    mc.setAttr(p + ".t", 1, 2, 3)
    fol = mc.createNode("follicle", name="folShape")
    folT = mc.listRelatives(fol, parent=True)[0]
    folT = mc.rename(folT, "fol")
    fol = mc.listRelatives(folT, shapes=True)[0]
    shape = mc.listRelatives(p, shapes=True)[0]
    mc.connectAttr(shape + ".outMesh", fol + ".inputMesh")
    mc.connectAttr(shape + ".worldMatrix[0]", fol + ".inputWorldMatrix")
    mc.connectAttr(fol + ".outTranslate", folT + ".translate")
    mc.connectAttr(fol + ".outRotate", folT + ".rotate")
    mc.setAttr(fol + ".parameterU", 0.3)
    mc.setAttr(fol + ".parameterV", 0.7)

    uv = mc.createNode("uvPin", name="uvp")
    mc.connectAttr(shape + ".worldMesh[0]", uv + ".deformedGeometry")
    mc.setAttr(uv + ".coordinate[0].coordinateU", 0.6)
    mc.setAttr(uv + ".coordinate[0].coordinateV", 0.25)
    mc.setAttr(uv + ".coordinate[1].coordinateU", 0.9)
    mc.setAttr(uv + ".coordinate[1].coordinateV", 0.9)
    mc.setAttr(uv + ".normalAxis", 1)
    mc.setAttr(uv + ".tangentAxis", 0)
    pinA = loc("pinnedA")
    pinB = loc("pinnedB")
    mc.connectAttr(uv + ".outputMatrix[0]", pinA + ".offsetParentMatrix")
    mc.connectAttr(uv + ".outputMatrix[1]", pinB + ".offsetParentMatrix")

    # A proximityPin riding a sheet a cluster bends, bound where its input matrix is at rest.
    sheet = plane("proxSheet", (0, 0, 10), sx=4, sy=4)
    cl, clh = mc.cluster(sheet + ".vtx[0:9]", name="sheetBend")
    shapes = mc.listRelatives(sheet, shapes=True)
    orig = [x for x in shapes if mc.getAttr(x + ".intermediateObject")][0]
    live = [x for x in shapes if not mc.getAttr(x + ".intermediateObject")][0]
    pp = mc.createNode("proximityPin", name="prox")
    mc.connectAttr(live + ".worldMesh[0]", pp + ".deformedGeometry")
    mc.connectAttr(orig + ".worldMesh[0]", pp + ".originalGeometry")
    rest = om.MTransformationMatrix()
    rest.setTranslation(om.MVector(0.6, 0.3, 9.1), om.MSpace.kTransform)
    rest.setRotation(om.MEulerRotation(0.2, 0.4, 0.1))
    mc.setAttr(pp + ".inputMatrix[0]", list(rest.asMatrix()), type="matrix")
    pinC = loc("pinnedC")
    mc.connectAttr(pp + ".outputMatrix[0]", pinC + ".offsetParentMatrix")

    drivers = [p + ".translateX", p + ".rotateX", p + ".rotateY", p + ".scaleZ", clh + ".translateY", clh + ".rotateX"]
    poses = [{}, {p + ".translateX": -2, p + ".rotateX": 30, p + ".rotateY": 50, p + ".scaleZ": 1.5},
             {clh + ".translateY": 1.5, clh + ".rotateX": 25}]
    record("rig_pins", drivers, poses, matrices=[folT, pinA, pinB, pinC])


# ---------------------------------------------------------------------------------------------------- combined


def combined():
    """A leg: IK with a pole vector, a reverse foot driven by utility nodes, parent-constrained controls, a cluster and a
    lattice on a knee pad mesh, and stacked set driven keys on the toe."""
    new_scene()
    mc.select(clear=True)
    hip = mc.joint(name="hip", position=(0, 10, 0))
    knee = mc.joint(name="knee", position=(0, 5.5, 0.8))
    ankle = mc.joint(name="ankle", position=(0, 1, 0))
    ball = mc.joint(name="ball", position=(0, 0, 2))
    toe = mc.joint(name="toe", position=(0, 0, 4))
    mc.joint(hip, edit=True, orientJoint="xyz", secondaryAxisOrient="xup", children=True, zeroScaleOrient=True)

    rig = grp("rig")
    footCtl = loc("footCtl", parent=rig)
    add(footCtl, "footRoll", value=0.0)
    add(footCtl, "toeTap", value=0.0)
    add(footCtl, "toeCurl", value=0.0)
    heel = grp("heelPivot", (0, 0, -1), parent=footCtl)
    toePiv = grp("toePivot", (0, 0, 4), parent=heel)
    ballPiv = grp("ballPivot", (0, 0, 2), parent=toePiv)
    legIk = mc.ikHandle(startJoint=hip, endEffector=ankle, solver="ikRPsolver", name="legIk")[0]
    mc.parent(legIk, ballPiv)
    ballIk = mc.ikHandle(startJoint=ankle, endEffector=ball, solver="ikSCsolver", name="ballIk")[0]
    mc.parent(ballIk, toePiv)
    toeIk = mc.ikHandle(startJoint=ball, endEffector=toe, solver="ikSCsolver", name="toeIk")[0]
    tapGrp = grp("toeTapPivot", (0, 0, 2), parent=toePiv)
    mc.parent(toeIk, tapGrp)

    kneeCtl = loc("kneeCtl", (0, 5.5, 8), parent=rig)
    mc.poleVectorConstraint(kneeCtl, legIk)

    # footRoll < 0 lifts the toes about the heel; > 0 rolls onto the ball up to 30, past that onto the toe.
    heelClamp = node("clamp", "heelClamp", minR=-90, maxR=0)
    mc.connectAttr(footCtl + ".footRoll", heelClamp + ".inputR")
    mc.connectAttr(heelClamp + ".outputR", heel + ".rx")
    ballClamp = node("clamp", "ballClamp", minR=0, maxR=30)
    mc.connectAttr(footCtl + ".footRoll", ballClamp + ".inputR")
    toeRoll = node("plusMinusAverage", "toeRollSub", operation=2)
    mc.connectAttr(footCtl + ".footRoll", toeRoll + ".input1D[0]")
    mc.connectAttr(ballClamp + ".outputR", toeRoll + ".input1D[1]")
    toeClamp = node("clamp", "toeClamp", minR=0, maxR=90)
    mc.connectAttr(toeRoll + ".output1D", toeClamp + ".inputR")
    mc.connectAttr(toeClamp + ".outputR", toePiv + ".rx")
    mc.connectAttr(ballClamp + ".outputR", ballPiv + ".rx")
    tapRev = node("multiplyDivide", "toeTapNeg", input2X=-1)
    mc.connectAttr(footCtl + ".toeTap", tapRev + ".input1X")
    mc.connectAttr(tapRev + ".outputX", tapGrp + ".rx")

    # Stacked set driven keys on the toe joint: toeCurl and the foot's own tilt.
    mc.setDrivenKeyframe(toe + ".rz", currentDriver=footCtl + ".toeCurl", driverValue=0, value=0, inTangentType="linear", outTangentType="linear")
    mc.setDrivenKeyframe(toe + ".rz", currentDriver=footCtl + ".toeCurl", driverValue=10, value=40, inTangentType="linear", outTangentType="linear")
    mc.setDrivenKeyframe(toe + ".rz", currentDriver=footCtl + ".rz", driverValue=0, value=0, inTangentType="linear", outTangentType="linear")
    mc.setDrivenKeyframe(toe + ".rz", currentDriver=footCtl + ".rz", driverValue=30, value=-15, inTangentType="linear", outTangentType="linear")

    # A control parent-constrained to the knee, and a hip control the hip joint is point-constrained to.
    kneeFollow = loc("kneeFollow", (1, 5.5, 0.8), parent=rig)
    mc.parentConstraint(knee, kneeFollow, maintainOffset=True)
    hipCtl = loc("hipCtl", (0, 10, 0), parent=rig)
    mc.pointConstraint(hipCtl, hip, maintainOffset=True)

    pad = plane("kneePad", (0, 5.5, 2), sx=4, sy=4, w=2, h=2)
    mc.setAttr(pad + ".rx", 90)
    mc.makeIdentity(pad, apply=True, rotate=True)
    padCl, padClH = mc.cluster(pad + ".vtx[6:8]", pad + ".vtx[11:13]", name="padCluster")
    padFfd, padLat, padBase = mc.lattice(pad, name="padLattice", divisions=(2, 2, 2), objectCentered=True)
    mc.parentConstraint(kneeFollow, padClH, maintainOffset=True)

    drivers = [footCtl + "." + a for a in ("translateX", "translateY", "translateZ", "rotateZ", "footRoll", "toeTap", "toeCurl")]
    drivers += [kneeCtl + ".translateX", hipCtl + ".translateY", padLat + ".translateZ", padLat + ".scaleX"]
    poses = [
        {},
        {footCtl + ".translateY": 2, footCtl + ".translateZ": 1, footCtl + ".footRoll": -20},
        {footCtl + ".footRoll": 15, footCtl + ".toeTap": 25, footCtl + ".toeCurl": 4},
        {footCtl + ".footRoll": 50, footCtl + ".rotateZ": 20, kneeCtl + ".translateX": 4, footCtl + ".toeCurl": 10},
        {footCtl + ".translateX": 1.5, footCtl + ".translateY": 3, hipCtl + ".translateY": -1, padLat + ".translateZ": 0.5, padLat + ".scaleX": 1.3},
    ]
    record("rig_combined", drivers, poses, matrices=[hip, knee, ankle, ball, toe, kneeFollow, padClH], meshes=[pad])


def unsupported():
    """What stays a limitation: an expression, spline IK, a motion path, a wire deformer, curve info and a string attr."""
    new_scene()
    ctl = loc("ctl")
    mc.addAttr(ctl, longName="label", dataType="string")
    driven = loc("exprDriven")
    mc.expression(name="wobble", string=driven + ".ty = sin(time) * " + ctl + ".tx;")
    mc.select(clear=True)
    j1 = mc.joint(name="spine1", position=(0, 0, 0))
    mc.joint(name="spine2", position=(0, 2, 0))
    mc.joint(name="spine3", position=(0, 4, 0))
    j4 = mc.joint(name="spine4", position=(0, 6, 0))
    mc.ikHandle(startJoint=j1, endEffector=j4, solver="ikSplineSolver", name="spineIk", createCurve=True)
    path = mc.curve(name="path", degree=3, point=[(0, 0, 0), (2, 0, 3), (5, 0, 4), (8, 0, 0)])
    rider = loc("rider")
    mc.pathAnimation(rider, curve=path, name="riderPath", startTimeU=1, endTimeU=24)
    info = mc.createNode("curveInfo", name="pathInfo")
    mc.connectAttr(mc.listRelatives(path, shapes=True)[0] + ".worldSpace[0]", info + ".inputCurve")
    bumpy = plane("bumpy", (0, 0, -10), sx=6, sy=6)
    tex = mc.shadingNode("file", asTexture=True, name="bumpTex")
    mc.setAttr(tex + ".fileTextureName", "../sourceimages/front.png", type="string")
    td = mc.deformer(bumpy, type="textureDeformer", name="bumpDeform")[0]
    mc.connectAttr(tex + ".outColor", td + ".texture")
    mc.setAttr(td + ".strength", 0.5)
    mc.setAttr(td + ".offset", -0.25)
    sheet = plane("sheet", (0, 0, 10), sx=6, sy=6)
    wireCurve = mc.curve(name="wireCurve", degree=1, point=[(-2, 0, 10), (2, 0, 10)])
    mc.wire(sheet, wire=wireCurve, name="sheetWire")
    record("rig_unsupported", [ctl + ".translateX"], [{}], matrices=[driven])


if __name__ == "__main__":
    wanted = sys.argv[1:] or ["utility", "matrices", "constraints", "ik", "sdk", "deformers", "surface_deformers", "pins", "combined", "unsupported"]
    for name in wanted:
        globals()[name]()
    maya.standalone.uninitialize()
