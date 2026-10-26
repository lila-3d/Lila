"""Writes an OBJ where every face has its own vertices (an exploded mesh), for the re-weld benchmark.

    python make_soup.py <in.obj> <out.obj>
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
v, vt, faces = [], [], []
for line in open(src):
    if line.startswith("v "):
        v.append(line[2:].strip())
    elif line.startswith("vt "):
        vt.append(line[3:].strip())
    elif line.startswith("f "):
        faces.append([c.split("/") for c in line.split()[1:]])
with open(dst, "w") as o:
    o.write("g soup\n")
    for f in faces:
        for c in f:
            o.write("v " + v[int(c[0]) - 1] + "\n")
            o.write("vt " + (vt[int(c[1]) - 1] if len(c) > 1 and c[1] else "0 0") + "\n")
    n = 0
    for f in faces:
        o.write("f " + " ".join(f"{n + k + 1}/{n + k + 1}" for k in range(len(f))) + "\n")
        n += len(f)
print(f"{len(faces)} faces, {n} vertices")
