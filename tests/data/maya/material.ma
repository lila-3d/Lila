//Maya ASCII 2026 scene
//Name: material.ma
// Handmade: a cube with a textured standardSurface on four faces and a see-through lambert on two; a plane on a blinn,
// another on a surfaceShader.
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
createNode transform -n "crate";
createNode mesh -n "crateShape" -p "crate";
	setAttr -k off ".v";
	setAttr -s 2 ".iog[0].og";
	setAttr ".iog[0].og[0].gcl" -type "componentList" 1 "f[4:5]";
	setAttr ".iog[0].og[1].gcl" -type "componentList" 2 "f[0:2]" "f[3]";
	setAttr -s 8 ".vt[0:7]"  -1 -1 1 1 -1 1 -1 1 1 1 1 1 -1 1 -1 1 1 -1 -1 -1 -1 1 -1 -1;
	setAttr -s 12 ".ed[0:11]"  0 1 0 2 3 0 4 5 0 6 7 0 0 2 0 1 3 0 2 4 0
		 3 5 0 4 6 0 5 7 0 6 0 0 7 1 0;
	setAttr -s 6 -ch 24 ".fc[0:5]" -type "polyFaces"
		f 4 0 5 -2 -5
		f 4 1 7 -3 -7
		f 4 2 9 -4 -9
		f 4 3 11 -1 -11
		f 4 -12 -10 -8 -6
		f 4 10 4 6 8;
createNode transform -n "floor";
createNode mesh -n "floorShape" -p "floor";
	setAttr -s 4 ".vt[0:3]"  -5 0 5 5 0 5 -5 0 -5 5 0 -5;
	setAttr -s 4 ".ed[0:3]"  0 1 0 0 2 0 1 3 0 2 3 0;
	setAttr -ch 4 ".fc[0]" -type "polyFaces"
		f 4 0 2 -4 -2;
createNode transform -n "card";
createNode mesh -n "cardShape" -p "card";
	setAttr -s 4 ".vt[0:3]"  -5 0 5 5 0 5 -5 0 -5 5 0 -5;
	setAttr -s 4 ".ed[0:3]"  0 1 0 0 2 0 1 3 0 2 3 0;
	setAttr -ch 4 ".fc[0]" -type "polyFaces"
		f 4 0 2 -4 -2;
createNode standardSurface -n "Paint";
	setAttr ".bc" -type "float3" 0.8 0.1 0.1 ;
	setAttr ".m" 0.7;
	setAttr ".sr" 0.3;
createNode file -n "paintTex";
	setAttr ".ftn" -type "string" "textures/paint_bc.png";
	setAttr ".ft" 0;
createNode file -n "paintNrm";
	setAttr ".ftn" -type "string" "C:\\maps\\paint_n.png";
createNode bump2d -n "bump2d1";
	setAttr ".bi" 1;
createNode place2dTexture -n "place2dTexture1";
	setAttr ".wu" no;
	setAttr ".mv" yes;
createNode shadingEngine -n "PaintSG";
	setAttr ".ihi" 0;
	setAttr ".ro" yes;
createNode lambert -n "Glass";
	setAttr ".c" -type "float3" 0.2 0.4 0.6 ;
	setAttr ".it" -type "float3" 0.5 0.5 0.5 ;
createNode shadingEngine -n "GlassSG";
createNode blinn -n "Shiny";
	setAttr ".ec" 0.1;
createNode shadingEngine -n "ShinySG";
createNode surfaceShader -n "Flat";
	setAttr ".oc" -type "float3" 0.9 0.5 0.1 ;
createNode shadingEngine -n "FlatSG";
createNode groupId -n "groupId1";
createNode groupId -n "groupId2";
connectAttr "paintTex.oc" "Paint.bc";
connectAttr "place2dTexture1.o" "paintTex.uv";
connectAttr "paintNrm.oa" "bump2d1.bv";
connectAttr "bump2d1.o" "Paint.n";
connectAttr "Paint.oc" "PaintSG.ss";
connectAttr "Glass.oc" "GlassSG.ss";
connectAttr "Shiny.oc" "ShinySG.ss";
connectAttr "Flat.oc" "FlatSG.ss";
connectAttr "groupId1.id" "crateShape.iog.og[0].gid";
connectAttr "groupId2.id" "crateShape.iog.og[1].gid";
connectAttr "crateShape.iog.og[0]" "GlassSG.dsm" -na;
connectAttr "crateShape.iog.og[1]" "PaintSG.dsm" -na;
connectAttr "floorShape.iog" "ShinySG.dsm" -na;
connectAttr "cardShape.iog" "FlatSG.dsm" -na;
// End of material.ma
