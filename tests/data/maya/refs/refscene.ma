//Maya ASCII 2026 scene
//Name: refscene.ma
//Last modified: Thu, Oct 22, 2026 10:26:24 AM
//Codeset: 1252
file -rdi 1 -ns "rig" -rfn "rigRN" -typ "mayaAscii" "C:/Users/Jared/AppData/Local/Temp/claude/X--Lila/3291b024-cb09-4fe4-b0a5-f05240762e01/scratchpad/maya/refs/skeleton.ma";
file -rdi 1 -ns "rigb" -rfn "rigbRN" -op "VERS|2026|UVER|undef|MADE|undef|CHNG|Thu, Oct 22, 2026 10:26:24 AM|ICON|undef|INFO|undef|OBJN|23|INCL|undef(|LUNI|cm|TUNI|film|AUNI|deg|TDUR|141120000|"
		 -typ "mayaBinary" "C:/Users/Jared/AppData/Local/Temp/claude/X--Lila/3291b024-cb09-4fe4-b0a5-f05240762e01/scratchpad/maya/refs/skeleton.mb";
file -r -ns "rig" -dr 1 -rfn "rigRN" -typ "mayaAscii" "C:/Users/Jared/AppData/Local/Temp/claude/X--Lila/3291b024-cb09-4fe4-b0a5-f05240762e01/scratchpad/maya/refs/skeleton.ma";
file -r -ns "rigb" -dr 1 -rfn "rigbRN" -op "VERS|2026|UVER|undef|MADE|undef|CHNG|Thu, Oct 22, 2026 10:26:24 AM|ICON|undef|INFO|undef|OBJN|23|INCL|undef(|LUNI|cm|TUNI|film|AUNI|deg|TDUR|141120000|"
		 -typ "mayaBinary" "C:/Users/Jared/AppData/Local/Temp/claude/X--Lila/3291b024-cb09-4fe4-b0a5-f05240762e01/scratchpad/maya/refs/skeleton.mb";
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
fileInfo "product" "Maya 2026";
fileInfo "version" "2026";
fileInfo "cutIdentifier" "202510291147-60ec9eda33";
fileInfo "osv" "Windows 11 Pro v2009 (Build: 26200)";
fileInfo "UUID" "93A19044-4DB7-DB67-4336-0282F77BED43";
createNode transform -s -n "persp";
	rename -uid "315010C4-40DC-2398-2AAE-BDABC2CF79FE";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 28 21 28 ;
	setAttr ".r" -type "double3" -27.938352729602379 44.999999999999972 -5.172681101354183e-14 ;
createNode camera -s -n "perspShape" -p "persp";
	rename -uid "55ECE0FB-4D80-5326-051E-4EA04BF85BDB";
	setAttr -k off ".v" no;
	setAttr ".fl" 34.999999999999993;
	setAttr ".coi" 44.82186966202994;
	setAttr ".imn" -type "string" "persp";
	setAttr ".den" -type "string" "persp_depth";
	setAttr ".man" -type "string" "persp_mask";
	setAttr ".hc" -type "string" "viewSet -p %camera";
createNode transform -s -n "top";
	rename -uid "719172CF-4974-D42C-5D1E-DEAEC829E687";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 1000.1 0 ;
	setAttr ".r" -type "double3" -90 0 0 ;
createNode camera -s -n "topShape" -p "top";
	rename -uid "320938A0-4D36-0D7A-6212-64A8392857EA";
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
	rename -uid "3BA70A00-4066-6170-FC2E-3EB085C6A31F";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 0 1000.1 ;
createNode camera -s -n "frontShape" -p "front";
	rename -uid "C0612E22-4C52-1F78-DCE1-D88D86BBED18";
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
	rename -uid "DA1B9BF4-4165-0B07-BD6A-AB8E8E88E674";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 1000.1 0 0 ;
	setAttr ".r" -type "double3" 0 90 0 ;
createNode camera -s -n "sideShape" -p "side";
	rename -uid "AE64DC4E-453E-05E4-8F29-7FB346FD575D";
	setAttr -k off ".v" no;
	setAttr ".rnd" no;
	setAttr ".coi" 1000.1;
	setAttr ".ow" 30;
	setAttr ".imn" -type "string" "side";
	setAttr ".den" -type "string" "side_depth";
	setAttr ".man" -type "string" "side_mask";
	setAttr ".hc" -type "string" "viewSet -s %camera";
	setAttr ".o" yes;
createNode transform -n "stage";
	rename -uid "7D5B0978-4A21-C128-F7DE-28AFA4A020C2";
	setAttr ".t" -type "double3" 5 0 -5 ;
createNode lightLinker -s -n "lightLinker1";
	rename -uid "392BC9E5-4B6E-62E9-3CB1-87BCC1D8A703";
	setAttr -s 2 ".lnk";
	setAttr -s 2 ".slnk";
createNode displayLayerManager -n "layerManager";
	rename -uid "CF8BBB2C-4E0D-7A66-7C88-16ABAAE5E59C";
createNode displayLayer -n "defaultLayer";
	rename -uid "F02A9A71-4D68-E340-CD29-63ADD01CC383";
	setAttr ".ufem" -type "stringArray" 0  ;
createNode renderLayerManager -n "renderLayerManager";
	rename -uid "FAEFA7E3-4050-ABA5-8415-D0BBBF814192";
createNode renderLayer -n "defaultRenderLayer";
	rename -uid "9E28D780-43D7-315E-2491-148FF7D4C43C";
	setAttr ".g" yes;
createNode reference -n "rigRN";
	rename -uid "D7B14813-4F84-FB90-7A1D-58B961F0D2BB";
	setAttr ".ed" -type "dataReferenceEdits" 
		"rigRN"
		"rigRN" 0
		"rigRN" 13
		0 "|rig:props" "|stage" "-s -r "
		1 "|rig:root|rig:spine|rig:arm|rig:hand" "grip" "grip" " -ci 1 -dv 1 -at \"double\""
		
		2 "|rig:root" "translate" " -type \"double3\" 1 12 3"
		2 "|rig:root" "rotate" " -type \"double3\" 10 20 29.99999999999999645"
		2 "|rig:root|rig:spine" "rotate" " -type \"double3\" 0 0 14.99999999999999822"
		
		2 "|rig:root|rig:spine" "scale" " -type \"double3\" 1.5 1.5 1.5"
		2 "|rig:root|rig:spine|rig:neck" "visibility" " 0"
		2 "|rig:root|rig:spine|rig:arm" "rotate" " -type \"double3\" 0 33 0"
		2 "|rig:root|rig:spine|rig:arm" "jointOrient" " -type \"double3\" 0 0 -20"
		
		2 "|rig:root|rig:spine|rig:arm|rig:hand" "translate" " -type \"double3\" 7 0 0"
		
		2 "|rig:root|rig:spine|rig:arm|rig:hand" "grip" " 1"
		2 "|stage|rig:props" "translate" " -type \"double3\" -5 0 5"
		2 "|stage|rig:props|rig:marker" "translate" " -type \"double3\" -4 2 3";
lockNode -l 1 ;
createNode reference -n "rigbRN";
	rename -uid "52CCAC2D-4B93-D861-5065-E8AC6DCF5053";
	setAttr ".ed" -type "dataReferenceEdits" 
		"rigbRN"
		"rigbRN" 0
		"rigbRN" 13
		0 "|rigb:props" "|stage" "-s -r "
		1 "|rigb:root|rigb:spine|rigb:arm|rigb:hand" "grip" "grip" " -ci 1 -dv 1 -at \"double\""
		
		2 "|rigb:root" "translate" " -type \"double3\" 1 12 3"
		2 "|rigb:root" "rotate" " -type \"double3\" 10 20 29.99999999999999645"
		2 "|rigb:root|rigb:spine" "rotate" " -type \"double3\" 0 0 14.99999999999999822"
		
		2 "|rigb:root|rigb:spine" "scale" " -type \"double3\" 1.5 1.5 1.5"
		2 "|rigb:root|rigb:spine|rigb:neck" "visibility" " 0"
		2 "|rigb:root|rigb:spine|rigb:arm" "rotate" " -type \"double3\" 0 33 0"
		2 "|rigb:root|rigb:spine|rigb:arm" "jointOrient" " -type \"double3\" 0 0 -20"
		
		2 "|rigb:root|rigb:spine|rigb:arm|rigb:hand" "translate" " -type \"double3\" 7 0 0"
		
		2 "|rigb:root|rigb:spine|rigb:arm|rigb:hand" "grip" " 1"
		2 "|stage|rigb:props" "translate" " -type \"double3\" -5 0 5"
		2 "|stage|rigb:props|rigb:marker" "translate" " -type \"double3\" -4 2 3";
lockNode -l 1 ;
createNode script -n "uiConfigurationScriptNode";
	rename -uid "6E512DAF-4875-728F-01F6-B984598AE65D";
	setAttr ".b" -type "string" "// Maya Mel UI Configuration File.\n// No UI generated in batch mode.\n";
	setAttr ".st" 3;
createNode script -n "sceneConfigurationScriptNode";
	rename -uid "864283DC-49FA-15FB-4703-7A9637747ED7";
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
	setAttr -s 3 ".r";
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
relationship "link" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
connectAttr "layerManager.dli[0]" "defaultLayer.id";
connectAttr "renderLayerManager.rlmi[0]" "defaultRenderLayer.rlid";
connectAttr "defaultRenderLayer.msg" ":defaultRenderingList1.r" -na;
dataStructure -fmt "raw" -as "name=externalContentTable:string=node:string=key:string=upath:uint32=upathcrc:string=rpath:string=roles";
applyMetadata -fmt "raw" -v "channel\nname externalContentTable\nstream\nname v1.0\nindexType numeric\nstructure externalContentTable\n0\n\"rigRN\" \"\" \"C:/Users/Jared/AppData/Local/Temp/claude/X--Lila/3291b024-cb09-4fe4-b0a5-f05240762e01/scratchpad/maya/refs/skeleton.ma\" 844861996 \"C:/Users/Jared/AppData/Local/Temp/claude/X--Lila/3291b024-cb09-4fe4-b0a5-f05240762e01/scratchpad/maya/refs/skeleton.ma\" \"FileRef\"\n1\n\"rigbRN\" \"\" \"C:/Users/Jared/AppData/Local/Temp/claude/X--Lila/3291b024-cb09-4fe4-b0a5-f05240762e01/scratchpad/maya/refs/skeleton.mb\" 2874327958 \"C:/Users/Jared/AppData/Local/Temp/claude/X--Lila/3291b024-cb09-4fe4-b0a5-f05240762e01/scratchpad/maya/refs/skeleton.mb\" \"FileRef\"\nendStream\nendChannel\nendAssociations\n" 
		-scn;
// End of refscene.ma
