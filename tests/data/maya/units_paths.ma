//Maya ASCII 2026 scene
//Name: units_paths.ma
// Handmade: metres and radians, two nodes with the same short name told apart by DAG path, a namespace, a custom attribute.
requires maya "2026";
currentUnit -l meter -a radian -t pal;
createNode transform -n "a";
	setAttr ".t" -type "double3" 1 0 0 ;
createNode transform -n "b";
	setAttr ".r" -type "double3" 0 1.5707963267948966 0 ;
createNode transform -n "leaf" -p "a";
	setAttr ".ty" 2;
createNode transform -n "leaf" -p "b";
	setAttr ".tz" 3;
createNode transform -n "kid" -p "|b|leaf";
createNode transform -n "props:barrel";
	addAttr -ci true -sn "hp" -ln "health" -dv 5 -at "double";
	setAttr -k on ".hp" 7;
createNode polyCube -n "polyCube1";
	setAttr ".w" 2;
	setAttr ".h" 4;
	setAttr ".d" 6;
createNode transform -n "props:box" -p "props:barrel";
createNode mesh -n "props:boxShape" -p "props:box";
createNode animCurveTL -n "a_translateY";
	setAttr -s 3 ".ktv[0:2]"  0 0 25 2 50 0;
connectAttr "polyCube1.out" "props:boxShape.i";
connectAttr "a_translateY.o" "a.ty";
// End of units_paths.ma
