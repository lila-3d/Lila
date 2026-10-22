//Maya ASCII 2026 scene
//Name: with_reference.ma
// Handmade: references cube_hier.ma into the "set" namespace and a missing file.
file -rdi 1 -ns "set" -rfn "setRN" -op "v=0;" -typ "mayaAscii" "cube_hier.ma";
file -r -ns "set" -dr 1 -rfn "setRN" -op "v=0;" -typ "mayaAscii" "cube_hier.ma";
file -r -ns "gone" -dr 1 -rfn "goneRN" -typ "mayaAscii" "missing.ma";
requires maya "2026";
currentUnit -l centimeter -a degree -t film;
createNode transform -n "stage";
createNode reference -n "setRN";
// End of with_reference.ma
