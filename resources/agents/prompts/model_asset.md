Model {{subject}} in Lila. Style: {{style}}. Budget: about {{budget}} triangles.

Work in Lila through its MCP tools only (no Blender or other apps). Follow this workflow:

1. Read lila://guide/modeling (or call read_guide with topic "modeling") if you have not yet. Call scene_summary to see what is already in the scene, and leave existing objects alone.
2. Plan in a few lines: the main masses and their sizes in cm (Y up, front faces +Z), which face of the base each part grows out of, and the silhouette features that make it read as {{subject}}.
3. Block out in one execute_python call: one polyCube split on x=0, setSymmetry axis x, then extrude the main parts out of its faces (selectComponentsWhere by normal, side="+x", box, extreme; never guessed ids). mesh_check, then capture_turntable with views ["front", "right", "top", "persp"] and display "shaded". Fix proportions before adding detail.
4. Features in one or two more calls: an edge loop where faces are needed, then the secondary features (for a character: snout, nose, eyes, ears, tail), every one extruded or inset from the same mesh. mesh_check and one capture.
5. Finish: materials for the main colour regions, a meaningful name, mesh_check OK with 1 shell and the triangle count near the budget, and a final capture.

Report the final face and triangle counts and anything you would improve with more time.
