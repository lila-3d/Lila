"""Batch script execution: what a script run over many scene files sees.

Each file opens in its own lilabatch worker, then the script runs with `ctx` (a BatchContext) in its globals:

    ctx.results["meshes"] = len(lila.ops.ls(type="mesh"))   # collected into the summary table and CSV
    if not ctx.results["meshes"]:
        ctx.skip("nothing to do")                             # counts as skipped, never saved

A script may instead define process(ctx); a dict it returns is merged into ctx.results. Every command is available as a
bare name, as in the script editor.
"""

import json
import sys
import traceback

__all__ = ["BatchContext", "BatchSkip"]


class BatchSkip(Exception):
    """Raised by ctx.skip(): the file is reported as skipped and not saved."""


class BatchContext:
    """The file being processed and where the script reports back.

    file        the scene that is open
    output      where it will be saved, "" when the run does not save
    output_dir  the run's output folder, "" when saving in place
    index       0-based position in the run; count is the run's size
    run_id      the run this file belongs to
    args        the run's user arguments (a dict)
    results     a dict the script fills; one column per key in the summary
    save        set False to leave this file unsaved
    """

    def __init__(self, data):
        self.file = data.get("file", "")
        self.output = data.get("output", "")
        self.output_dir = data.get("outputDir", "")
        self.index = int(data.get("index", 0))
        self.count = int(data.get("count", 1))
        self.run_id = int(data.get("runId", 0))
        self.args = data.get("args") or {}
        self.results = {}
        self.save = True

    def skip(self, reason=""):
        """Stops the script here; the file is reported as skipped and not saved."""
        raise BatchSkip(reason)

    def fail(self, reason):
        """Stops the script here and reports the file as failed with reason."""
        raise RuntimeError(reason)

    def __repr__(self):
        return f"BatchContext({self.file!r}, {self.index + 1}/{self.count})"


def _plain(value):
    try:
        json.dumps(value)
        return value
    except (TypeError, ValueError):
        if isinstance(value, dict):
            return {str(k): _plain(v) for k, v in value.items()}
        if isinstance(value, (list, tuple, set)):
            return [_plain(v) for v in value]
        return repr(value)


def _error_location(tb, filename):
    """The deepest frame in the batch script itself, else the deepest frame."""
    frames = traceback.extract_tb(tb)
    for frame in reversed(frames):
        if frame.filename == filename:
            return frame.filename, frame.lineno
    if frames:
        return frames[-1].filename, frames[-1].lineno
    return "", 0


def _run_worker(context_path, outcome_path):
    """Runs the batch script once for the open scene. Called by the lilabatch worker; never raises."""
    with open(context_path, encoding="utf-8") as f:
        data = json.load(f)
    ctx = BatchContext(data)
    filename = data.get("scriptFile") or "<batch script>"
    outcome = {"ok": False, "skipped": False, "message": "", "errorFile": "", "errorLine": 0, "traceback": ""}
    main = sys.modules.get("__main__")
    scope = dict(vars(main)) if main else {}
    scope.update({"__name__": "__main__", "__file__": filename, "ctx": ctx, "batch_context": ctx})
    try:
        exec(compile(data.get("script", ""), filename, "exec"), scope)
        process = scope.get("process")
        if callable(process):
            returned = process(ctx)
            if isinstance(returned, dict):
                ctx.results.update(returned)
            elif returned is not None:
                ctx.results["result"] = returned
        outcome["ok"] = True
    except BatchSkip as e:
        outcome["ok"] = True
        outcome["skipped"] = True
        outcome["message"] = str(e)
    except BaseException as e:  # SystemExit from the script included: it must not end the worker
        # From the script's own frames down; the runner's frame is noise.
        text = "".join(traceback.format_exception(type(e), e, e.__traceback__.tb_next)).rstrip()
        sys.stderr.write(text + "\n")
        outcome["message"] = f"{type(e).__name__}: {e}"
        outcome["traceback"] = text
        if isinstance(e, SyntaxError) and e.filename == filename:
            outcome["errorFile"], outcome["errorLine"] = filename, e.lineno or 0
        else:
            outcome["errorFile"], outcome["errorLine"] = _error_location(e.__traceback__, filename)
    outcome["save"] = bool(ctx.save)
    outcome["results"] = _plain(ctx.results)
    with open(outcome_path, "w", encoding="utf-8") as f:
        json.dump(outcome, f)
