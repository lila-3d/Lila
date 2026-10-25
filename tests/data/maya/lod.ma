//Maya ASCII 2026 scene
//Name: lod.ma
// Handmade: an LOD group switching by screen height percentage, levels out of name order, and one switching by distance.
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
fileInfo "application" "maya";
createNode lodGroup -n "rockLod";
	setAttr ".t" -type "double3" 10 0 0 ;
	setAttr -s 3 ".dl";
	setAttr ".ush" yes;
	setAttr -s 2 ".pth[0:1]"  40 15;
createNode transform -n "z_high" -p "rockLod";
createNode transform -n "a_mid" -p "rockLod";
createNode transform -n "m_low" -p "rockLod";
createNode lodGroup -n "treeLod";
	setAttr -s 2 ".th[0:1]"  500 2000;
createNode transform -n "treeHigh" -p "treeLod";
createNode transform -n "treeLow" -p "treeLod";
// End of lod.ma
