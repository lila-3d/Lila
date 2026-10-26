Dress {{area}} in Lila as {{brief}}.

Work in Lila through its MCP tools only. Follow this workflow:

1. Read lila://guide/set-dressing (or call read_guide with topic "setDressing") if you have not yet. Call scene_summary to see what is there; leave existing objects alone unless the brief says otherwise.
2. Plan in a few lines: which surfaces get what (shelves, tables, floor), roughly how many of each, and how messy.
3. Find assets with librarySearch. If something is missing, say so and use the closest asset rather than modeling one.
4. Place and arrange in as few execute_python calls as you can: fillShelf, scatterProps, pileProps, place(..., on=...). Use realize=True wherever you will push or check single items.
5. Make it believable: wiggle or push a few things, let piles settle, keep some order and some mess.
6. dressCheck every placed mesh and fix each issue until ok is True, then capture_view the area from two angles.

Report what you placed (counts per asset), the dressCheck result and anything the library lacked.
