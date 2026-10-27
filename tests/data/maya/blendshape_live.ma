//Maya ASCII 2026 scene
//Name: blendshape_live.ma
// Handmade: blendShapes fed by target meshes still in the scene. "strip" has a live target, an in-between mesh and a
// hidden (intermediate) target; "strip2" blends in world space toward a copy of itself moved 5 along X.
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
createNode transform -n "strip";
createNode mesh -n "stripShape" -p "strip";
createNode mesh -n "stripShapeOrig" -p "strip";
	setAttr ".io" yes;
	setAttr -s 6 ".vt[0:5]"  0 0 0  1 0 0  0 1 0  1 1 0  0 2 0  1 2 0;
	setAttr -s 7 ".ed[0:6]"  0 1 0  2 3 1  4 5 0  0 2 0  1 3 0  2 4 0  3 5 0;
	setAttr -s 2 -ch 8 ".fc[0:1]" -type "polyFaces"
		f 4 0 4 -2 -4
		f 4 1 6 -3 -6;
createNode transform -n "stretch";
	setAttr ".t" -type "double3" 3 0 0 ;
createNode mesh -n "stretchShape" -p "stretch";
	setAttr -s 6 ".vt[0:5]"  0 0 0  1 0 0  0 1 0  1 1 0  0 2 0  1 3 0;
	setAttr -s 7 ".ed[0:6]"  0 1 0  2 3 1  4 5 0  0 2 0  1 3 0  2 4 0  3 5 0;
	setAttr -s 2 -ch 8 ".fc[0:1]" -type "polyFaces"
		f 4 0 4 -2 -4
		f 4 1 6 -3 -6;
createNode transform -n "stretchHalf";
	setAttr ".t" -type "double3" 6 0 0 ;
createNode mesh -n "stretchHalfShape" -p "stretchHalf";
	setAttr -s 6 ".vt[0:5]"  0 0 0  1 0 0  0 1 0  1 1 0  0 2 0  2 2 0;
	setAttr -s 7 ".ed[0:6]"  0 1 0  2 3 1  4 5 0  0 2 0  1 3 0  2 4 0  3 5 0;
	setAttr -s 2 -ch 8 ".fc[0:1]" -type "polyFaces"
		f 4 0 4 -2 -4
		f 4 1 6 -3 -6;
createNode transform -n "hidden";
createNode mesh -n "hiddenShape" -p "hidden";
	setAttr ".io" yes;
	setAttr -s 6 ".vt[0:5]"  0 0 -1  1 0 0  0 1 0  1 1 0  0 2 0  1 2 0;
	setAttr -s 7 ".ed[0:6]"  0 1 0  2 3 1  4 5 0  0 2 0  1 3 0  2 4 0  3 5 0;
	setAttr -s 2 -ch 8 ".fc[0:1]" -type "polyFaces"
		f 4 0 4 -2 -4
		f 4 1 6 -3 -6;
createNode blendShape -n "liveBlend";
	addAttr -ci true -h true -sn "aal" -ln "attributeAliasList" -dt "attributeAlias";
	setAttr -s 2 ".w[0:1]"  1 1;
	setAttr ".aal" -type "attributeAlias" {"stretch","weight[0]","push","weight[1]"} ;
createNode transform -n "strip2";
	setAttr ".t" -type "double3" 0 0 10 ;
createNode mesh -n "strip2Shape" -p "strip2";
createNode mesh -n "strip2ShapeOrig" -p "strip2";
	setAttr ".io" yes;
	setAttr -s 6 ".vt[0:5]"  0 0 0  1 0 0  0 1 0  1 1 0  0 2 0  1 2 0;
	setAttr -s 7 ".ed[0:6]"  0 1 0  2 3 1  4 5 0  0 2 0  1 3 0  2 4 0  3 5 0;
	setAttr -s 2 -ch 8 ".fc[0:1]" -type "polyFaces"
		f 4 0 4 -2 -4
		f 4 1 6 -3 -6;
createNode transform -n "moved";
	setAttr ".t" -type "double3" 5 0 10 ;
createNode mesh -n "movedShape" -p "moved";
	setAttr -s 6 ".vt[0:5]"  0 0 0  1 0 0  0 1 0  1 1 0  0 2 0  1 2 0;
	setAttr -s 7 ".ed[0:6]"  0 1 0  2 3 1  4 5 0  0 2 0  1 3 0  2 4 0  3 5 0;
	setAttr -s 2 -ch 8 ".fc[0:1]" -type "polyFaces"
		f 4 0 4 -2 -4
		f 4 1 6 -3 -6;
createNode blendShape -n "worldBlend";
	setAttr ".w[0]" 0.5;
	setAttr ".or" 1;
connectAttr "liveBlend.og[0]" "stripShape.i";
connectAttr "stripShapeOrig.w" "liveBlend.ip[0].ig";
connectAttr "stretchShape.w" "liveBlend.it[0].itg[0].iti[6000].igt";
connectAttr "stretchHalfShape.w" "liveBlend.it[0].itg[0].iti[5500].igt";
connectAttr "hiddenShape.w" "liveBlend.it[0].itg[1].iti[6000].igt";
connectAttr "worldBlend.og[0]" "strip2Shape.i";
connectAttr "strip2ShapeOrig.w" "worldBlend.ip[0].ig";
connectAttr "movedShape.w" "worldBlend.it[0].itg[0].iti[6000].igt";
// End of blendshape_live.ma
