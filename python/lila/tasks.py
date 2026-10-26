"""Background work for scripts: run a function on a worker thread, then use its result back on the main thread.

    from lila import tasks

    node = lila.node("Ship")
    points = node.points_array(base=True)      # main thread: a snapshot, safe to read anywhere

    def work(progress, pts):                    # worker thread: numpy, files, anything but the scene
        progress.set_status("Smoothing")
        return pts * 1.01

    def done(result):                           # main thread again: scene edits are safe here
        node.set_points(result)

    job = tasks.run_in_background(work, done, args=(points,), title="Inflate")

Scene reads and edits (lila.ops, Node attributes, Node.points_array and the other array getters, set_points) run on
the main thread only, and raise a CommandError on any other. Arrays they return are snapshots that later edits never
change, so any thread can read them. numpy and the array getters release the GIL while they crunch, so Python worker
threads (threading, concurrent.futures) really run side by side.
"""

import sys
import threading
import traceback
from concurrent.futures import Future

import _lila

__all__ = ["run_in_background", "call_on_main", "is_main_thread", "Job"]


def is_main_thread():
    """True on the thread that runs the scene and the UI."""
    return _lila.is_main_thread()


def _print_error(e):
    traceback.print_exception(type(e), e, e.__traceback__, file=sys.stderr)


def call_on_main(fn, *args, **kwargs):
    """Runs fn(*args, **kwargs) on the main thread when it is next idle and returns a concurrent.futures.Future of its
    result. Safe from any thread. Errors are logged and also set on the future. Never call .result() on that future
    from the main thread: it waits for the main thread itself."""
    future = Future()

    def run():
        if not future.set_running_or_notify_cancel():
            return
        try:
            future.set_result(fn(*args, **kwargs))
        except BaseException as e:
            _print_error(e)
            future.set_exception(e)

    _lila.post_to_main(run)
    return future


class _Progress:
    """What fn gets as its first argument when run_in_background has a title."""

    def __init__(self, job, scope):
        self._job = job
        self._scope = scope

    def step(self, n=1.0):
        """Advance by n units of total."""
        if self._scope is not None:
            self._scope.step(n)

    def set_status(self, text):
        if self._scope is not None:
            self._scope.set_status(text)

    @property
    def cancelled(self):
        """True once the user pressed Cancel or Job.cancel() was called. Check it and return early."""
        return self._job.cancelled or (self._scope is not None and self._scope.cancelled)


class Job:
    """One run_in_background call."""

    def __init__(self):
        self._worker_done = threading.Event()
        self._finished = threading.Event()
        self._cancel = False
        self._result = None
        self._error = None

    @property
    def running(self):
        """True until fn returned or raised."""
        return not self._worker_done.is_set()

    @property
    def finished(self):
        """True once fn ended and on_done or on_error ran on the main thread."""
        return self._finished.is_set()

    @property
    def cancelled(self):
        return self._cancel

    def cancel(self):
        """Asks fn to stop: its progress.cancelled turns True. fn decides when to return."""
        self._cancel = True

    def wait(self, timeout=None):
        """Blocks until finished. On the main thread it runs queued main-thread work while it waits, so on_done still
        runs: lilabatch scripts call this, since the batch ends with the script. Returns finished."""
        if not is_main_thread():
            return self._finished.wait(timeout)
        import time

        end = None if timeout is None else time.monotonic() + timeout
        while not self._finished.is_set():
            _lila.drain_main()
            if end is not None and time.monotonic() >= end:
                break
            self._worker_done.wait(0.01)
        _lila.drain_main()
        return self._finished.is_set()

    def result(self, timeout=None):
        """fn's return value after wait(); raises what fn raised."""
        if not self.wait(timeout):
            raise TimeoutError("lila.tasks job still running")
        if self._error is not None:
            raise self._error
        return self._result


def run_in_background(fn, on_done=None, *, args=(), kwargs=None, on_error=None, title=None, total=0.0):
    """Runs fn(*args, **kwargs) on a new worker thread and returns a Job. Then on the main thread: on_done(result), or
    on_error(exception) (default: log the traceback).

    With a title, the job shows as a background progress indicator with a Cancel button, and fn is called as
    fn(progress, *args, **kwargs): progress.step(n) out of total, progress.set_status(text), progress.cancelled.

    fn must not touch the scene: pass it arrays and plain data, and apply its result in on_done. The scene may have
    changed by then (the user kept working), so check that what on_done edits still exists."""
    job = Job()
    kwargs = dict(kwargs or {})

    def finish():
        try:
            if job._error is not None:
                (on_error or _print_error)(job._error)
            elif on_done is not None:
                on_done(job._result)
        finally:
            job._finished.set()

    def worker():
        try:
            if title is None:
                job._result = fn(*args, **kwargs)
            elif getattr(_lila, "Progress", None) is None:
                job._result = fn(_Progress(job, None), *args, **kwargs)
            else:
                with _lila.Progress(title, total=total, cancellable=True, background=True) as scope:
                    job._result = fn(_Progress(job, scope), *args, **kwargs)
        except BaseException as e:
            job._error = e
        finally:
            job._worker_done.set()
            _lila.post_to_main(finish)

    threading.Thread(target=worker, name=f"lila.tasks: {title or getattr(fn, '__name__', 'job')}", daemon=True).start()
    return job
