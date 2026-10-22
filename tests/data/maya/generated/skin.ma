//Maya ASCII 2026 scene
//Name: skin.ma
//Last modified: Thu, Oct 22, 2026 08:31:41 AM
//Codeset: 1252
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
fileInfo "product" "Maya 2026";
fileInfo "version" "2026";
fileInfo "cutIdentifier" "202510291147-60ec9eda33";
fileInfo "osv" "Windows 11 Pro v2009 (Build: 26200)";
fileInfo "UUID" "EC5143FC-4A93-87D3-0D06-C9896E9E84DE";
createNode transform -s -n "persp";
	rename -uid "933A8C2A-40FB-8199-8601-ED8094261920";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 28 21 28 ;
	setAttr ".r" -type "double3" -27.938352729602379 44.999999999999972 -5.172681101354183e-14 ;
createNode camera -s -n "perspShape" -p "persp";
	rename -uid "B5354AAA-4AE9-9D9F-3FCD-8EAD69270BB0";
	setAttr -k off ".v" no;
	setAttr ".fl" 34.999999999999993;
	setAttr ".coi" 44.82186966202994;
	setAttr ".imn" -type "string" "persp";
	setAttr ".den" -type "string" "persp_depth";
	setAttr ".man" -type "string" "persp_mask";
	setAttr ".hc" -type "string" "viewSet -p %camera";
createNode transform -s -n "top";
	rename -uid "8F99CA37-4460-2D26-F0E5-3A95DCBA312A";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 1000.1 0 ;
	setAttr ".r" -type "double3" -90 0 0 ;
createNode camera -s -n "topShape" -p "top";
	rename -uid "DC5C4ACA-4715-DE91-137E-D1B6B9D62167";
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
	rename -uid "36335F64-4FEA-1601-310E-38B9C785D44B";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 0 1000.1 ;
createNode camera -s -n "frontShape" -p "front";
	rename -uid "1B17E9E3-4FF4-19F4-DAD6-5C95F6DAB99F";
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
	rename -uid "74FEC3D2-4420-9E2D-142B-AFB6819B808A";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 1000.1 0 0 ;
	setAttr ".r" -type "double3" 0 90 0 ;
createNode camera -s -n "sideShape" -p "side";
	rename -uid "47749032-4403-BCC2-F1C7-7BA3B99F95A6";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "side";
	setAttr ".den" -type "string" "side_depth";
	setAttr ".man" -type "string" "side_mask";
	setAttr ".hc" -type "string" "viewSet -s %camera";
	setAttr ".o" yes;
createNode transform -n "skinbox";
	rename -uid "77BA1615-4774-BE26-55C0-4CABAF7F1A10";
	setAttr -l on ".tx";
	setAttr -l on ".ty";
	setAttr -l on ".tz";
	setAttr -l on ".rx";
	setAttr -l on ".ry";
	setAttr -l on ".rz";
	setAttr -l on ".sx";
	setAttr -l on ".sy";
	setAttr -l on ".sz";
createNode mesh -n "skinboxShape" -p "skinbox";
	rename -uid "59896C4D-4F8C-2C8B-D046-80A1F36DCFF7";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
	setAttr ".vcs" 2;
createNode mesh -n "skinboxShapeOrig" -p "skinbox";
	rename -uid "E018C29E-43F3-F6FB-4A17-80BB9986C338";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[5:8]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[9]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0:3]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[14:17]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[10:13]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[4]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 32 ".uvst[0].uvsp[0:31]" -type "float2" 0.33000001 0 0.66333336
		 0 0.33000001 0.0625 0.66333336 0.0625 0.33000001 0.125 0.66333336 0.125 0.33000001
		 0.1875 0.66333336 0.1875 0.33000001 0.25 0.66333336 0.25 0.33000001 0.5 0.66333336
		 0.5 0.33000001 0.5625 0.66333336 0.5625 0.33000001 0.625 0.66333336 0.625 0.33000001
		 0.6875 0.66333336 0.6875 0.33000001 0.75 0.66333336 0.75 0.33000001 1 0.66333336
		 1 1 0 1 0.0625 1 0.125 1 0.1875 1 0.25 0 0 0 0.0625 0 0.125 0 0.1875 0 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 20 ".vt[0:19]"  -0.5 -2 0.5 0.5 -2 0.5 -0.5 -1 0.5 0.5 -1 0.5
		 -0.5 0 0.5 0.5 0 0.5 -0.5 1 0.5 0.5 1 0.5 -0.5 2 0.5 0.5 2 0.5 -0.5 2 -0.5 0.5 2 -0.5
		 -0.5 1 -0.5 0.5 1 -0.5 -0.5 0 -0.5 0.5 0 -0.5 -0.5 -1 -0.5 0.5 -1 -0.5 -0.5 -2 -0.5
		 0.5 -2 -0.5;
	setAttr -s 36 ".ed[0:35]"  0 1 0 2 3 1 4 5 1 6 7 1 8 9 0 10 11 0 12 13 1
		 14 15 1 16 17 1 18 19 0 0 2 0 1 3 0 2 4 0 3 5 0 4 6 0 5 7 0 6 8 0 7 9 0 8 10 0 9 11 0
		 10 12 0 11 13 0 12 14 0 13 15 0 14 16 0 15 17 0 16 18 0 17 19 0 18 0 0 19 1 0 17 3 1
		 15 5 1 13 7 1 16 2 1 14 4 1 12 6 1;
	setAttr -s 18 -ch 72 ".fc[0:17]" -type "polyFaces" 
		f 4 0 11 -2 -11
		mu 0 4 0 1 3 2
		f 4 1 13 -3 -13
		mu 0 4 2 3 5 4
		f 4 2 15 -4 -15
		mu 0 4 4 5 7 6
		f 4 3 17 -5 -17
		mu 0 4 6 7 9 8
		f 4 4 19 -6 -19
		mu 0 4 8 9 11 10
		f 4 5 21 -7 -21
		mu 0 4 10 11 13 12
		f 4 6 23 -8 -23
		mu 0 4 12 13 15 14
		f 4 7 25 -9 -25
		mu 0 4 14 15 17 16
		f 4 8 27 -10 -27
		mu 0 4 16 17 19 18
		f 4 9 29 -1 -29
		mu 0 4 18 19 21 20
		f 4 -30 -28 30 -12
		mu 0 4 1 22 23 3
		f 4 -31 -26 31 -14
		mu 0 4 3 23 24 5
		f 4 -32 -24 32 -16
		mu 0 4 5 24 25 7
		f 4 -33 -22 -20 -18
		mu 0 4 7 25 26 9
		f 4 28 10 -34 26
		mu 0 4 27 0 2 28
		f 4 33 12 -35 24
		mu 0 4 28 2 4 29
		f 4 34 14 -36 22
		mu 0 4 29 4 6 30
		f 4 35 16 18 20
		mu 0 4 30 6 8 31;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode joint -n "root";
	rename -uid "6AF32512-4897-D77F-DF57-37A009FACDD3";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".t" -type "double3" 0 -2 0 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0 0 90 ;
	setAttr ".bps" -type "matrix" 0 1 0 0 -1 0 0 0 0 0 1 0 0 -2 0 1;
	setAttr ".radi" 0.7;
createNode joint -n "mid" -p "root";
	rename -uid "459D9EE3-4C37-3B6F-310D-F6AAA28CA456";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".t" -type "double3" 2 0 0 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".bps" -type "matrix" 0 1 0 0 -1 0 0 0 0 0 1 0 0 0 0 1;
createNode joint -n "tip" -p "mid";
	rename -uid "E499C76D-410B-7C88-D4B5-6C9B22B3D0D5";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".t" -type "double3" 2 0 0 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0 0 -90 ;
	setAttr ".bps" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 2 0 1;
createNode lightLinker -s -n "lightLinker1";
	rename -uid "867B015A-4E35-9708-90A4-24905E090471";
	setAttr -s 2 ".lnk";
	setAttr -s 2 ".slnk";
createNode displayLayerManager -n "layerManager";
	rename -uid "0DB6892C-4F81-5B28-3DC9-13A5B11AFC43";
createNode displayLayer -n "defaultLayer";
	rename -uid "097423F9-48CB-EFBA-96D5-EE89715C3F76";
	setAttr ".ufem" -type "stringArray" 0  ;
createNode renderLayerManager -n "renderLayerManager";
	rename -uid "885877A8-4E79-ED5D-09D2-CE9479823717";
createNode renderLayer -n "defaultRenderLayer";
	rename -uid "464F4EB8-42D0-4A87-503D-7C89048224B9";
	setAttr ".g" yes;
createNode skinCluster -n "skin1";
	rename -uid "55E2785D-4B28-19E1-38A4-7B8B7E0BA493";
	setAttr -s 20 ".wl";
	setAttr ".wl[0:19].w"
		3 0 0.98690967676971098 1 0.012184070083576687 2 0.00090625314671231492
		3 0 0.98690967676971098 1 0.012184070083576687 2 0.00090625314671231492
		3 0 0.89776181265542965 1 0.099751312517270027 2 0.0024868748273003611
		3 0 0.89776181265542965 1 0.099751312517270027 2 0.0024868748273003611
		3 0 0.49693251533742339 1 0.49693251533742328 2 0.0061349693251533787
		3 0 0.49693251533742339 1 0.49693251533742328 2 0.0061349693251533787
		3 0 0.090909090909090953 1 0.81818181818181812 2 0.090909090909090953
		3 0 0.090909090909090953 1 0.81818181818181812 2 0.090909090909090953
		3 0 0.0061349693251533787 1 0.49693251533742328 2 0.49693251533742328
		3 0 0.0061349693251533787 1 0.49693251533742328 2 0.49693251533742328
		3 0 0.0061349693251533787 1 0.49693251533742328 2 0.49693251533742328
		3 0 0.0061349693251533787 1 0.49693251533742328 2 0.49693251533742328
		3 0 0.090909090909090953 1 0.81818181818181812 2 0.090909090909090953
		3 0 0.090909090909090953 1 0.81818181818181812 2 0.090909090909090953
		3 0 0.49693251533742339 1 0.49693251533742328 2 0.0061349693251533787
		3 0 0.49693251533742339 1 0.49693251533742328 2 0.0061349693251533787
		3 0 0.89776181265542965 1 0.099751312517270027 2 0.0024868748273003611
		3 0 0.89776181265542965 1 0.099751312517270027 2 0.0024868748273003611
		3 0 0.98690967676971098 1 0.012184070083576687 2 0.00090625314671231492
		3 0 0.98690967676971098 1 0.012184070083576687 2 0.00090625314671231492;
	setAttr -s 3 ".pm";
	setAttr ".pm[0]" -type "matrix" 0 -1 0 -0 1 0 -0 0 -0 -0 1 -0 2 0 -0 1;
	setAttr ".pm[1]" -type "matrix" 0 -1 0 -0 1 0 -0 0 0 -0 1 -0 -0 0 -0 1;
	setAttr ".pm[2]" -type "matrix" 1 -0 0 -0 -0 1 -0 0 0 -0 1 -0 -0 -2 -0 1;
	setAttr ".gm" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
	setAttr -s 3 ".ma";
	setAttr -s 3 ".dpf[0:2]"  4 4 4;
	setAttr -s 3 ".lw";
	setAttr -s 3 ".lw";
	setAttr ".mi" 3;
	setAttr ".bm" 1;
	setAttr ".ucm" yes;
	setAttr -s 3 ".ifcl";
	setAttr -s 3 ".ifcl";
createNode dagPose -n "bindPose1";
	rename -uid "D185C725-464D-015F-2244-9286A0F94560";
	setAttr -s 3 ".wm";
	setAttr -s 3 ".xm";
	setAttr ".xm[0]" -type "matrix" "xform" 1 1 1 0 0 0 0 0 -2 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0.70710678118654757 0.70710678118654757 1 1 1 yes;
	setAttr ".xm[1]" -type "matrix" "xform" 1 1 1 0 0 0 0 2 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[2]" -type "matrix" "xform" 1 1 1 0 0 0 0 2 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 -0.70710678118654757 0.70710678118654757 1 1 1 yes;
	setAttr -s 3 ".m";
	setAttr -s 3 ".p";
	setAttr ".bp" yes;
createNode animCurveTA -n "mid_rotateZ";
	rename -uid "5938D032-4A41-689C-632E-279779F6330F";
	setAttr ".tan" 18;
	setAttr ".wgt" no;
	setAttr -s 2 ".ktv[0:1]"  1 0 10 45;
	setAttr -s 2 ".kit[1]"  2;
	setAttr -s 2 ".kot[1]"  3;
createNode animCurveTL -n "root_translateX";
	rename -uid "D8632527-4BE3-9F2B-B274-068E43B55D5F";
	setAttr ".tan" 9;
	setAttr ".wgt" no;
	setAttr -s 2 ".ktv[0:1]"  1 0 5 2;
	setAttr -s 2 ".kit[1]"  10;
	setAttr -s 2 ".kot[0:1]"  5 18;
createNode script -n "uiConfigurationScriptNode";
	rename -uid "647AF257-4751-EB05-E9F9-C89AE1E93C4A";
	setAttr ".b" -type "string" "// Maya Mel UI Configuration File.\n// No UI generated in batch mode.\n";
	setAttr ".st" 3;
createNode script -n "sceneConfigurationScriptNode";
	rename -uid "FB30E31C-49EB-984C-295E-0BB4DB93E3FF";
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
	setAttr -s 2 ".st";
select -ne :renderGlobalsList1;
select -ne :defaultShaderList1;
	setAttr -s 6 ".s";
select -ne :postProcessList1;
	setAttr -s 2 ".p";
select -ne :defaultRenderingList1;
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
connectAttr "skin1.og[0]" "skinboxShape.i";
connectAttr "root_translateX.o" "root.tx";
connectAttr "root.s" "mid.is";
connectAttr "mid_rotateZ.o" "mid.rz";
connectAttr "mid.s" "tip.is";
relationship "link" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
connectAttr "layerManager.dli[0]" "defaultLayer.id";
connectAttr "renderLayerManager.rlmi[0]" "defaultRenderLayer.rlid";
connectAttr "skinboxShapeOrig.w" "skin1.ip[0].ig";
connectAttr "skinboxShapeOrig.o" "skin1.orggeom[0]";
connectAttr "bindPose1.msg" "skin1.bp";
connectAttr "root.wm" "skin1.ma[0]";
connectAttr "mid.wm" "skin1.ma[1]";
connectAttr "tip.wm" "skin1.ma[2]";
connectAttr "root.liw" "skin1.lw[0]";
connectAttr "mid.liw" "skin1.lw[1]";
connectAttr "tip.liw" "skin1.lw[2]";
connectAttr "root.obcc" "skin1.ifcl[0]";
connectAttr "mid.obcc" "skin1.ifcl[1]";
connectAttr "tip.obcc" "skin1.ifcl[2]";
connectAttr "root.msg" "bindPose1.m[0]";
connectAttr "mid.msg" "bindPose1.m[1]";
connectAttr "tip.msg" "bindPose1.m[2]";
connectAttr "bindPose1.w" "bindPose1.p[0]";
connectAttr "bindPose1.m[0]" "bindPose1.p[1]";
connectAttr "bindPose1.m[1]" "bindPose1.p[2]";
connectAttr "root.bps" "bindPose1.wm[0]";
connectAttr "mid.bps" "bindPose1.wm[1]";
connectAttr "tip.bps" "bindPose1.wm[2]";
connectAttr "defaultRenderLayer.msg" ":defaultRenderingList1.r" -na;
connectAttr "skinboxShape.iog" ":initialShadingGroup.dsm" -na;
// End of skin.ma
