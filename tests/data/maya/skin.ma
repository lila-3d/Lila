//Maya ASCII 2026 scene
//Name: skin.ma
// Handmade: a two-joint chain skinning a strip of two quads, with both weight list forms Maya writes, and a key on the elbow.
requires maya "2026";
currentUnit -l centimeter -a degree -t ntsc;
createNode transform -n "strip";
createNode mesh -n "stripShape" -p "strip";
	setAttr -k off ".v";
createNode mesh -n "stripShapeOrig" -p "strip";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr -s 6 ".vt[0:5]"  0 0 0  1 0 0  0 1 0  1 1 0  0 2 0  1 2 0;
	setAttr -s 7 ".ed[0:6]"  0 1 0  2 3 1  4 5 0  0 2 0  1 3 0  2 4 0  3 5 0;
	setAttr -s 2 -ch 8 ".fc[0:1]" -type "polyFaces"
		f 4 0 4 -2 -4
		f 4 1 6 -3 -6;
createNode joint -n "root";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".jo" -type "double3" 0 0 90 ;
	setAttr ".radi" 0.5;
createNode joint -n "elbow" -p "root";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".t" -type "double3" 1 0 0 ;
	setAttr ".sd" 1;
	setAttr ".typ" 11;
createNode skinCluster -n "skinCluster1";
	setAttr -s 6 ".wl";
	setAttr ".wl[0:3].w"
		1 0 1
		1 0 1
		2 0 0.5 1 0.5
		2 0 0.25 1 0.75;
	setAttr -s 2 ".wl[4].w[0:1]"  0 1;
	setAttr ".wl[5].w[1]" 1;
	setAttr -s 2 ".pm";
	setAttr ".pm[0]" -type "matrix" 0 -1 0 0 1 0 0 0 0 0 1 0 0 0 0 1;
	setAttr ".pm[1]" -type "matrix" 0 -1 0 0 1 0 0 0 0 0 1 0 -1 0 0 1;
	setAttr ".gm" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
	setAttr -s 2 ".ma";
	setAttr ".mi" 2;
createNode tweak -n "tweak1";
createNode groupParts -n "skinCluster1GroupParts";
createNode animCurveTA -n "elbow_rotateZ";
	setAttr ".tan" 18;
	setAttr -s 2 ".ktv[0:1]"  0 0 30 90;
	setAttr -s 2 ".kit[0:1]"  2 2;
	setAttr -s 2 ".kot[0:1]"  2 2;
	setAttr ".pst" 1;
connectAttr "skinCluster1.og[0]" "stripShape.i";
connectAttr "skinCluster1GroupParts.og" "skinCluster1.ip[0].ig";
connectAttr "tweak1.og[0]" "skinCluster1GroupParts.ig";
connectAttr "stripShapeOrig.w" "tweak1.ip[0].ig";
connectAttr "root.wm" "skinCluster1.ma[0]";
connectAttr "elbow.wm" "skinCluster1.ma[1]";
connectAttr "root.s" "elbow.is";
connectAttr "elbow_rotateZ.o" "elbow.rz";
connectAttr "stripShape.iog" ":initialShadingGroup.dsm" -na;
// End of skin.ma
