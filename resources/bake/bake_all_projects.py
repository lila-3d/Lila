"""Bakes every bake project in each scene of a batch run.

ctx.args:
    changedOnly  skip projects whose textures already match their inputs (default True)
    assign       wire the textures into the lows' materials and save the scene (default False)
"""

changed_only = bool(ctx.args.get("changedOnly", True))
assign = bool(ctx.args.get("assign", False))

results = bakeAll(changedOnly=changed_only, assign=assign)
if not results:
    ctx.skip("no bake projects")

ctx.results["projects"] = len(results)
ctx.results["baked"] = sum(1 for r in results if r.get("baked"))
ctx.results["textures"] = sum(int(r.get("outputs", 0)) for r in results)
ctx.results["misses"] = sum(int(r.get("misses", 0)) for r in results)
ctx.save = assign

failed = [r for r in results if str(r.get("reason", "")).startswith("failed")]
if failed:
    ctx.fail("; ".join(f"{r['project']}: {r['reason']}" for r in failed))
