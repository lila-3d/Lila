"""The user docs in the Docs panel, readable offline.

    import lila
    lila.docs.open("essentials")            # a page, as in the docs site's URL after #/
    lila.docs.open("curves?id=pipe")        # a heading on it
    lila.docs.open_api("lila.ops.polyCube") # the API page for a symbol, or the API home
    lila.docs.update()                      # download the latest docs now
"""

from . import ops

__all__ = ["open", "open_api", "update", "info"]


def open(page=""):
    """Show the Docs panel at a page id ("essentials", "python/", "curves?id=pipe"); empty for the docs home."""
    ops.openDocs(page=page)


def open_api(symbol=""):
    """Show the API docs page for a Python symbol, falling back to the API home and then the docs home."""
    if symbol:
        ops.openDocs(api=str(symbol))
    else:
        ops.openDocs(page="python/")


def update():
    """Download the latest docs for this version in the background; the result shows in the status line."""
    ops.docsUpdate()


def info():
    """Where the docs are read from: {root, source, ref, sha, refs, cacheRoot, autoUpdate}."""
    return ops.docsInfo()
