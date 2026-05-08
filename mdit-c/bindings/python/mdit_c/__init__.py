"""mdit_c — Python bindings for the C port of markdown-it-py.

This package re-exports the ``MarkdownIt`` class implemented in the
``_mdit_c`` extension module. The intent is for ``import mdit_c`` to
behave as a drop-in alternative to ``import markdown_it`` for the
subset of the API that the C engine currently supports.

Today, that subset is:

* ``MarkdownIt(preset="default", options=None)``
* ``MarkdownIt.render(src) -> str``
* ``MarkdownIt.options`` (read/write subset of upstream options)
* ``MarkdownIt.enable(names, ignoreInvalid=False)`` and
  ``MarkdownIt.disable(...)``

Token-stream access (``parse``, ``Token``, ``SyntaxTreeNode``) is
deliberately NOT exposed yet — it requires a stable token-accessor C
API that lands in a follow-up slice. Calling ``parse`` here will raise
``AttributeError`` so importers fail loudly instead of silently
diverging from the upstream Python API.
"""

from __future__ import annotations

from ._mdit_c import MarkdownIt, __version__

__all__ = ["MarkdownIt", "__version__"]
