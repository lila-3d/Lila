//Maya ASCII 2026 scene
//Name: materials.ma
//Last modified: Thu, Oct 22, 2026 08:31:41 AM
//Codeset: 1252
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
fileInfo "product" "Maya 2026";
fileInfo "version" "2026";
fileInfo "cutIdentifier" "202510291147-60ec9eda33";
fileInfo "osv" "Windows 11 Pro v2009 (Build: 26200)";
fileInfo "UUID" "03616DF4-46EF-4C04-E5C5-A7A3B9F8590B";
createNode transform -s -n "persp";
	rename -uid "92D43986-4010-7961-8A3F-D08937370589";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 28 21 28 ;
	setAttr ".r" -type "double3" -27.938352729602379 44.999999999999972 -5.172681101354183e-14 ;
createNode camera -s -n "perspShape" -p "persp";
	rename -uid "BD7544B4-4B1E-CD4C-FE51-E48CC8DD60D3";
	setAttr -k off ".v" no;
	setAttr ".fl" 34.999999999999993;
	setAttr ".coi" 44.82186966202994;
	setAttr ".imn" -type "string" "persp";
	setAttr ".den" -type "string" "persp_depth";
	setAttr ".man" -type "string" "persp_mask";
	setAttr ".hc" -type "string" "viewSet -p %camera";
createNode transform -s -n "top";
	rename -uid "06336746-4347-5BE2-9F6B-D884B7A72E91";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 1000.1 0 ;
	setAttr ".r" -type "double3" -90 0 0 ;
createNode camera -s -n "topShape" -p "top";
	rename -uid "B5E03909-4ED2-50F6-3427-B6AF8F9879F0";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "top";
	setAttr ".den" -type "string" "top_depth";
	setAttr ".man" -type "string" "top_mask";
	setAttr ".hc" -type "string" "viewSet -t %camera";
	setAttr ".o" yes;
createNode transform -s -n "front";
	rename -uid "67FE9072-44EF-8B57-889D-03A6FD97ED33";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 0 1000.1 ;
createNode camera -s -n "frontShape" -p "front";
	rename -uid "08140C51-4AD6-1934-E05E-DF9EBFD18E1C";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "front";
	setAttr ".den" -type "string" "front_depth";
	setAttr ".man" -type "string" "front_mask";
	setAttr ".hc" -type "string" "viewSet -f %camera";
	setAttr ".o" yes;
createNode transform -s -n "side";
	rename -uid "2F8D315F-4601-4054-3BE7-B1B0E87C5F43";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 1000.1 0 0 ;
	setAttr ".r" -type "double3" 0 90 0 ;
createNode camera -s -n "sideShape" -p "side";
	rename -uid "42366BE7-4055-504E-29DA-0B902EB75FBD";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "side";
	setAttr ".den" -type "string" "side_depth";
	setAttr ".man" -type "string" "side_mask";
	setAttr ".hc" -type "string" "viewSet -s %camera";
	setAttr ".o" yes;
createNode transform -n "matbox";
	rename -uid "DB09FDE5-4FE9-F9EF-290F-05B4DD68C2C2";
createNode mesh -n "matboxShape" -p "matbox";
	rename -uid "FB7F07E6-4E2A-48A4-251D-C58F3B343852";
	setAttr -k off ".v";
	setAttr -s 2 ".iog[0].og";
	setAttr ".iog[0].og[0].gcl" -type "componentList" 1 "f[4:5]";
	setAttr ".iog[0].og[1].gcl" -type "componentList" 1 "f[0:3]";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[2]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[3]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[5]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[4]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[1]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 14 ".uvst[0].uvsp[0:13]" -type "float2" 0.33000001 0 0.66333336
		 0 0.33000001 0.25 0.66333336 0.25 0.33000001 0.5 0.66333336 0.5 0.33000001 0.75 0.66333336
		 0.75 0.33000001 1 0.66333336 1 1 0 1 0.25 0 0 0 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 8 ".vt[0:7]"  -0.5 -0.5 0.5 0.5 -0.5 0.5 -0.5 0.5 0.5 0.5 0.5 0.5
		 -0.5 0.5 -0.5 0.5 0.5 -0.5 -0.5 -0.5 -0.5 0.5 -0.5 -0.5;
	setAttr -s 12 ".ed[0:11]"  0 1 0 2 3 0 4 5 0 6 7 0 0 2 0 1 3 0 2 4 0
		 3 5 0 4 6 0 5 7 0 6 0 0 7 1 0;
	setAttr -s 6 -ch 24 ".fc[0:5]" -type "polyFaces" 
		f 4 0 5 -2 -5
		mu 0 4 0 1 3 2
		f 4 1 7 -3 -7
		mu 0 4 2 3 5 4
		f 4 2 9 -4 -9
		mu 0 4 4 5 7 6
		f 4 3 11 -1 -11
		mu 0 4 6 7 9 8
		f 4 -12 -10 -8 -6
		mu 0 4 1 10 11 3
		f 4 10 4 6 8
		mu 0 4 12 0 2 13;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "blinnbox";
	rename -uid "C897EDC5-4F38-291D-369A-7DA232BAB914";
	setAttr ".t" -type "double3" 3 0 0 ;
createNode mesh -n "blinnboxShape" -p "blinnbox";
	rename -uid "4C3726DA-4762-A0CC-FE7C-699BFD73CB10";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[2]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[3]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[5]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[4]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[1]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 14 ".uvst[0].uvsp[0:13]" -type "float2" 0.33000001 0 0.66333336
		 0 0.33000001 0.25 0.66333336 0.25 0.33000001 0.5 0.66333336 0.5 0.33000001 0.75 0.66333336
		 0.75 0.33000001 1 0.66333336 1 1 0 1 0.25 0 0 0 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 8 ".vt[0:7]"  -0.5 -0.5 0.5 0.5 -0.5 0.5 -0.5 0.5 0.5 0.5 0.5 0.5
		 -0.5 0.5 -0.5 0.5 0.5 -0.5 -0.5 -0.5 -0.5 0.5 -0.5 -0.5;
	setAttr -s 12 ".ed[0:11]"  0 1 0 2 3 0 4 5 0 6 7 0 0 2 0 1 3 0 2 4 0
		 3 5 0 4 6 0 5 7 0 6 0 0 7 1 0;
	setAttr -s 6 -ch 24 ".fc[0:5]" -type "polyFaces" 
		f 4 0 5 -2 -5
		mu 0 4 0 1 3 2
		f 4 1 7 -3 -7
		mu 0 4 2 3 5 4
		f 4 2 9 -4 -9
		mu 0 4 4 5 7 6
		f 4 3 11 -1 -11
		mu 0 4 6 7 9 8
		f 4 -12 -10 -8 -6
		mu 0 4 1 10 11 3
		f 4 10 4 6 8
		mu 0 4 12 0 2 13;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode lightLinker -s -n "lightLinker1";
	rename -uid "5360CF3F-49B1-E102-11BB-EF9E6E937677";
	setAttr -s 5 ".lnk";
	setAttr -s 5 ".slnk";
createNode displayLayerManager -n "layerManager";
	rename -uid "2ED9845A-49A4-0F8D-956E-9BB6345E29B9";
createNode displayLayer -n "defaultLayer";
	rename -uid "222C29B8-4621-3BC4-1740-7BB2F6C54FE6";
	setAttr ".ufem" -type "stringArray" 0  ;
createNode renderLayerManager -n "renderLayerManager";
	rename -uid "B742F615-46F0-AC6D-6909-BA8131B32F78";
createNode renderLayer -n "defaultRenderLayer";
	rename -uid "C8EC8081-4F85-896A-F998-F485C8A02437";
	setAttr ".g" yes;
createNode standardSurface -n "Paint";
	rename -uid "2FFB7040-472B-0929-F3F1-80865D82D301";
	setAttr ".sr" 0.30000001192092896;
	setAttr ".m" 0.69999998807907104;
createNode file -n "paintTex";
	rename -uid "C50B0B51-4E75-3573-C20A-8FB4C726FF84";
	setAttr ".ftn" -type "string" "textures/paint_bc.png";
	setAttr ".cs" -type "string" "sRGB Encoded Rec.709 (sRGB)";
createNode file -n "paintNrm";
	rename -uid "33677CA8-457B-3EA0-F3BE-74AE67C21CA6";
	setAttr ".ftn" -type "string" "textures/paint_n.png";
	setAttr ".cs" -type "string" "sRGB Encoded Rec.709 (sRGB)";
createNode bump2d -n "bump2d1";
	rename -uid "B08D2312-45AE-3C76-C4E3-8F9678D9C89C";
	setAttr ".bi" 1;
createNode shadingEngine -n "PaintSG";
	rename -uid "8861B06A-4A68-4954-77FD-058AE6F9B001";
	setAttr ".ihi" 0;
	setAttr ".ro" yes;
createNode materialInfo -n "materialInfo1";
	rename -uid "97A959B6-4C10-CB2E-EEE2-F9B68287B5D7";
createNode lambert -n "Rubber";
	rename -uid "8E0D4F79-4F7D-9511-3759-8CBEE440FFA7";
	setAttr ".c" -type "float3" 0.1 0.1 0.1 ;
createNode shadingEngine -n "RubberSG";
	rename -uid "00A075FB-473E-9685-525E-E1A6A804EE07";
	setAttr ".ihi" 0;
	setAttr ".ro" yes;
createNode materialInfo -n "materialInfo2";
	rename -uid "BFE0B637-4B6D-B4CF-D53F-71BA4C972A7E";
createNode groupId -n "groupId1";
	rename -uid "8F57CBEA-4799-D917-319C-DFB8482F8054";
	setAttr ".ihi" 0;
createNode groupId -n "groupId2";
	rename -uid "C269B2C5-484C-5E75-D73E-29A25367A7CE";
	setAttr ".ihi" 0;
createNode groupId -n "groupId3";
	rename -uid "1A096E01-440A-5F46-5091-E59B06665054";
	setAttr ".ihi" 0;
createNode blinn -n "Shiny";
	rename -uid "14699D04-4AD9-490C-3A57-BCBCA470F5F4";
createNode shadingEngine -n "ShinySG";
	rename -uid "DEA25095-4FC7-F2C0-752B-FD811ED59FE8";
	setAttr ".ihi" 0;
	setAttr ".ro" yes;
createNode materialInfo -n "materialInfo3";
	rename -uid "8C15FFA1-4554-1352-9BD4-A39D51310A1A";
createNode script -n "uiConfigurationScriptNode";
	rename -uid "51AA9F35-4EF5-7E25-2BBA-BEB00890DF72";
	setAttr ".b" -type "string" "// Maya Mel UI Configuration File.\n// No UI generated in batch mode.\n";
	setAttr ".st" 3;
createNode script -n "sceneConfigurationScriptNode";
	rename -uid "65947052-4599-3D3D-CE04-D9B93B8600E0";
	setAttr ".b" -type "string" "playbackOptions -min 1 -max 120 -ast 1 -aet 200 ";
	setAttr ".st" 6;
select -ne :time1;
	setAttr ".o" 1;
	setAttr ".unw" 1;
select -ne :hardwareRenderingGlobals;
	setAttr ".otfna" -type "stringArray" 22 "NURBS Curves" "NURBS Surfaces" "Polygons" "Subdiv Surface" "Particles" "Particle Instance" "Fluids" "Strokes" "Image Planes" "UI" "Lights" "Cameras" "Locators" "Joints" "IK Handles" "Deformers" "Motion Trails" "Components" "Hair Systems" "Follicles" "Misc. UI" "Ornaments"  ;
	setAttr ".otfva" -type "Int32Array" 22 0 1 1 1 1 1
		 1 1 1 0 0 0 0 0 0 0 0 0
		 0 0 0 0 ;
	setAttr ".fprt" yes;
	setAttr ".rtfm" 1;
select -ne :renderPartition;
	setAttr -s 5 ".st";
select -ne :renderGlobalsList1;
select -ne :defaultShaderList1;
	setAttr -s 9 ".s";
select -ne :postProcessList1;
	setAttr -s 2 ".p";
select -ne :defaultRenderUtilityList1;
select -ne :defaultRenderingList1;
select -ne :defaultTextureList1;
	setAttr -s 2 ".tx";
select -ne :standardSurface1;
	setAttr ".bc" -type "float3" 0.40000001 0.40000001 0.40000001 ;
	setAttr ".sr" 0.5;
select -ne :openPBR_shader1;
	setAttr ".bc" -type "float3" 0.40000001 0.40000001 0.40000001 ;
	setAttr ".sr" 0.5;
select -ne :initialShadingGroup;
	setAttr ".ro" yes;
select -ne :initialParticleSE;
	setAttr ".ro" yes;
select -ne :defaultRenderGlobals;
	addAttr -ci true -h true -sn "dss" -ln "defaultSurfaceShader" -dt "string";
	setAttr ".dss" -type "string" "openPBR_shader1";
select -ne :defaultResolution;
	setAttr ".pa" 1;
select -ne :defaultColorMgtGlobals;
	setAttr ".cfe" yes;
	setAttr ".cfp" -type "string" "<MAYA_RESOURCES>/OCIO-configs/Maya2022-default/config.ocio";
	setAttr ".vtn" -type "string" "ACES 1.0 SDR-video (sRGB)";
	setAttr ".vn" -type "string" "ACES 1.0 SDR-video";
	setAttr ".dn" -type "string" "sRGB";
	setAttr ".wsn" -type "string" "ACEScg";
	setAttr ".otn" -type "string" "ACES 1.0 SDR-video (sRGB)";
	setAttr ".potn" -type "string" "ACES 1.0 SDR-video (sRGB)";
select -ne :hardwareRenderGlobals;
	setAttr ".ctrs" 256;
	setAttr ".btrs" 512;
connectAttr "groupId1.id" "matboxShape.iog.og[0].gid";
connectAttr "RubberSG.mwc" "matboxShape.iog.og[0].gco";
connectAttr "groupId3.id" "matboxShape.iog.og[1].gid";
connectAttr "PaintSG.mwc" "matboxShape.iog.og[1].gco";
connectAttr "groupId2.id" "matboxShape.ciog.cog[0].cgid";
relationship "link" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" "PaintSG.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" "RubberSG.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" "ShinySG.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" "PaintSG.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" "RubberSG.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" "ShinySG.message" ":defaultLightSet.message";
connectAttr "layerManager.dli[0]" "defaultLayer.id";
connectAttr "renderLayerManager.rlmi[0]" "defaultRenderLayer.rlid";
connectAttr "paintTex.oc" "Paint.bc";
connectAttr "bump2d1.o" "Paint.n";
connectAttr ":defaultColorMgtGlobals.cme" "paintTex.cme";
connectAttr ":defaultColorMgtGlobals.cfe" "paintTex.cmcf";
connectAttr ":defaultColorMgtGlobals.cfp" "paintTex.cmcp";
connectAttr ":defaultColorMgtGlobals.wsn" "paintTex.ws";
connectAttr ":defaultColorMgtGlobals.cme" "paintNrm.cme";
connectAttr ":defaultColorMgtGlobals.cfe" "paintNrm.cmcf";
connectAttr ":defaultColorMgtGlobals.cfp" "paintNrm.cmcp";
connectAttr ":defaultColorMgtGlobals.wsn" "paintNrm.ws";
connectAttr "paintNrm.oa" "bump2d1.bv";
connectAttr "Paint.oc" "PaintSG.ss";
connectAttr "groupId3.msg" "PaintSG.gn" -na;
connectAttr "matboxShape.iog.og[1]" "PaintSG.dsm" -na;
connectAttr "PaintSG.msg" "materialInfo1.sg";
connectAttr "Rubber.oc" "RubberSG.ss";
connectAttr "matboxShape.iog.og[0]" "RubberSG.dsm" -na;
connectAttr "groupId1.msg" "RubberSG.gn" -na;
connectAttr "RubberSG.msg" "materialInfo2.sg";
connectAttr "Shiny.oc" "ShinySG.ss";
connectAttr "blinnboxShape.iog" "ShinySG.dsm" -na;
connectAttr "ShinySG.msg" "materialInfo3.sg";
connectAttr "PaintSG.pa" ":renderPartition.st" -na;
connectAttr "RubberSG.pa" ":renderPartition.st" -na;
connectAttr "ShinySG.pa" ":renderPartition.st" -na;
connectAttr "Paint.msg" ":defaultShaderList1.s" -na;
connectAttr "Rubber.msg" ":defaultShaderList1.s" -na;
connectAttr "Shiny.msg" ":defaultShaderList1.s" -na;
connectAttr "bump2d1.msg" ":defaultRenderUtilityList1.u" -na;
connectAttr "defaultRenderLayer.msg" ":defaultRenderingList1.r" -na;
connectAttr "paintTex.msg" ":defaultTextureList1.tx" -na;
connectAttr "paintNrm.msg" ":defaultTextureList1.tx" -na;
connectAttr "matboxShape.ciog.cog[0]" ":initialShadingGroup.dsm" -na;
connectAttr "groupId2.msg" ":initialShadingGroup.gn" -na;
dataStructure -fmt "raw" -as "name=externalContentTable:string=node:string=key:string=upath:uint32=upathcrc:string=rpath:string=roles";
applyMetadata -fmt "raw" -v "channel\nname externalContentTable\nstream\nname v1.0\nindexType numeric\nstructure externalContentTable\n0\n\"paintTex\" \"fileTextureName\" \"textures/paint_bc.png\" 564803918 \"\" \"sourceImages\"\n1\n\"paintNrm\" \"fileTextureName\" \"textures/paint_n.png\" 1989567061 \"\" \"sourceImages\"\nendStream\nendChannel\nendAssociations\n" 
		-scn;
// End of materials.ma
