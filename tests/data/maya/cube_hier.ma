//Maya ASCII 2026 scene
//Name: cube_hier.ma
// Handmade: a group holding a cube with uvs, a tweak, pivots and a rotate order; the top camera says Y up.
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
createNode transform -s -n "top";
	setAttr ".v" no;
	setAttr ".t" -type "double3" 0 1000.1 0 ;
	setAttr ".r" -type "double3" -90 0 0 ;
createNode camera -s -n "topShape" -p "top";
	setAttr -k off ".v" no;
	setAttr ".o" yes;
createNode transform -n "grp";
	setAttr ".t" -type "double3" 1 2 3 ;
	setAttr ".r" -type "double3" 0 90 0 ;
	setAttr ".ro" 2;
createNode transform -n "box" -p "grp";
	setAttr ".t" -type "double3" 0 1 0 ;
	setAttr ".tx" 5;
	setAttr ".s" -type "double3" 1 2 1 ;
	setAttr ".rp" -type "double3" 0.5 0 0 ;
	setAttr ".sp" -type "double3" 0 0.25 0 ;
createNode mesh -n "boxShape" -p "|grp|box";
	setAttr -k off ".v";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 14 ".uvst[0].uvsp[0:13]" -type "float2" 0.375 0 0.625 0 0.375 0.25
		 0.625 0.25 0.375 0.5 0.625 0.5 0.375 0.75 0.625 0.75 0.375 1 0.625 1 0.875 0 0.875 0.25
		 0.125 0 0.125 0.25;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".pt[1]" -type "float3"  0 0.5 0;
	setAttr -s 8 ".vt[0:7]"  -1 -1 1 1 -1 1 -1 1 1 1 1 1 -1 1 -1 1 1 -1 -1 -1 -1 1 -1 -1;
	setAttr -s 12 ".ed[0:11]"  0 1 0 2 3 0 4 5 0 6 7 0 0 2 0 1 3 0 2 4 0
		 3 5 0 4 6 0 5 7 0 6 0 0 7 1 1;
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
createNode transform -n "empty" -p "grp";
	setAttr ".v" no;
createNode transform -n "marker";
	setAttr ".t" -type "double3" 0 3 0 ;
createNode locator -n "markerShape" -p "marker";
	setAttr ".los" -type "double3" 2 2 2 ;
createNode transform -n "shotCam";
createNode camera -n "shotCamShape" -p "shotCam";
createNode nurbsCurve -n "curveShape1" -p "marker";
createNode polyBevel3 -n "orphanBevel";
connectAttr "boxShape.iog" ":initialShadingGroup.dsm" -na;
// End of cube_hier.ma
