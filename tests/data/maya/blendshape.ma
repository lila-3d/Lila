//Maya ASCII 2026 scene
//Name: blendshape.ma
// Handmade: a strip of two quads with a blendShape holding its targets inline: an in-between, per-target and base
// weights, a keyed weight, a set driven key off a control, a weight wired straight to a control, and one expression.
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
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
createNode transform -n "ctrl";
	setAttr ".t" -type "double3" 0 1 0 ;
	setAttr ".r" -type "double3" 0 0 0.25 ;
createNode blendShape -n "faceBlend";
	addAttr -ci true -h true -sn "aal" -ln "attributeAliasList" -dt "attributeAlias";
	setAttr -s 5 ".w[0:4]"  0.5 0 0 0 0;
	setAttr -s 5 ".it[0].itg";
	setAttr -s 2 ".it[0].itg[0].iti";
	setAttr ".it[0].itg[0].iti[5500].ipt" -type "pointArray" 1 1 0 0 1 ;
	setAttr ".it[0].itg[0].iti[5500].ict" -type "componentList" 1 "vtx[4]";
	setAttr ".it[0].itg[0].iti[6000].ipt" -type "pointArray" 2 0 1 0 1 0 1 0 1 ;
	setAttr ".it[0].itg[0].iti[6000].ict" -type "componentList" 1 "vtx[4:5]";
	setAttr ".it[0].itg[1].iti[6000].ipt" -type "pointArray" 2 0 0 1 1 0 0 2 1 ;
	setAttr ".it[0].itg[1].iti[6000].ict" -type "componentList" 2 "vtx[0]" "vtx[2]";
	setAttr -s 6 ".it[0].itg[1].tw[0:5]"  1 1 0.5 1 1 1;
	setAttr ".it[0].itg[2].iti[6000].ipt" -type "pointArray" 1 0 2 0 1 ;
	setAttr ".it[0].itg[2].iti[6000].ict" -type "componentList" 1 "vtx[1]";
	setAttr ".it[0].itg[3].iti[6000].ipt" -type "pointArray" 1 0 0 1 1 ;
	setAttr ".it[0].itg[3].iti[6000].ict" -type "componentList" 1 "vtx[3]";
	setAttr ".it[0].itg[4].iti[6000].ipt" -type "pointArray" 6 0 0 3 1 0 0 3 1 0 0 3 1 0 0 3 1
		 0 0 3 1 0 0 3 1 ;
	setAttr ".it[0].itg[4].iti[6000].ict" -type "componentList" 1 "vtx[*]";
	setAttr ".it[0].bw[0]" 0.5;
	setAttr ".aal" -type "attributeAlias" {"smile","weight[0]","frown","weight[1]","raise","weight[2]","tilt","weight[3]"
		,"scripted","weight[4]"} ;
createNode groupParts -n "faceBlendGroupParts";
createNode animCurveTU -n "faceBlend_frown";
	setAttr ".tan" 18;
	setAttr -s 2 ".ktv[0:1]"  0 0 24 1;
	setAttr -s 2 ".kit[0:1]"  2 2;
	setAttr -s 2 ".kot[0:1]"  2 2;
createNode animCurveUU -n "faceBlend_raise";
	setAttr ".tan" 2;
	setAttr -s 2 ".ktv[0:1]"  0 0 2 1;
createNode unitConversion -n "unitConversion1";
	setAttr ".cf" 57.295779513082323;
createNode expression -n "expression1";
	setAttr -k on ".nds";
	setAttr ".ixp" -type "string" ".O[0] = sin(time)";
connectAttr "faceBlend.og[0]" "stripShape.i";
connectAttr "faceBlendGroupParts.og" "faceBlend.ip[0].ig";
connectAttr "stripShapeOrig.w" "faceBlendGroupParts.ig";
connectAttr "faceBlend_frown.o" "faceBlend.w[1]";
connectAttr "ctrl.ty" "faceBlend_raise.i";
connectAttr "faceBlend_raise.o" "faceBlend.w[2]";
connectAttr "ctrl.rz" "unitConversion1.i";
connectAttr "unitConversion1.o" "faceBlend.w[3]";
connectAttr "expression1.out[0]" "faceBlend.w[4]";
connectAttr "stripShape.iog" ":initialShadingGroup.dsm" -na;
// End of blendshape.ma
