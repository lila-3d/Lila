## Lila

This folder is a Lila workspace. Lila is a Maya-style 3D DCC for Unreal game assets, and it is running: do 3D work in it through its `lila` MCP server, not in Blender, Maya or by writing mesh files.

- A model is one connected mesh grown out of one primitive with symmetry, extrude, inset and edge loops; never separate primitives pushed together. Read the MCP resource `lila://guide/modeling` (or the `read_guide` tool) first; the `lila-modeling` skill has the workflow.
- Drive Lila with the `execute_python` tool (`cmds` is `lila.cmds`), a whole stage per call. Select components with `selectComponentsWhere` / `findComponents`, never by guessed ids.
- Check with `mesh_check` (must be OK, 1 shell) and look with `capture_turntable` (front, right, top, persp; shaded).
- Headless scripts: `lilabatch --batch script.py`.
- If the `lila` tools are missing, the MCP server is off (status bar MCP button) or this agent was started outside Lila's terminal: Lila's terminal sets `LILA_MCP_URL` and `LILA_MCP_TOKEN`, which `.mcp.json` reads.
