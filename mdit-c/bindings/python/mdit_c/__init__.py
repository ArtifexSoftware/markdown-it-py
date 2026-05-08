"""mdit_c — Python bindings for the C port of markdown-it-py.

This package re-exports the ``MarkdownIt`` class implemented in the
``_mdit_c`` extension module. The intent is for ``import mdit_c`` to
behave as a drop-in alternative to ``import markdown_it`` for the
subset of the API that the C engine currently supports.

Today, that subset is:

* ``MarkdownIt(preset="default", options=None)``
* ``MarkdownIt.render(src) -> str``
* ``MarkdownIt.parse(src) -> list[Token]``
* ``Token.as_dict(as_upstream=True)``
* ``MarkdownIt.options`` (read/write subset of upstream options)
* ``MarkdownIt.enable(names, ignoreInvalid=False)`` and
  ``MarkdownIt.disable(...)``

``SyntaxTreeNode`` and the richer plugin/ruler Python API are not
exposed yet.
"""

from __future__ import annotations

from ._mdit_c import MarkdownIt, Token, __version__

__all__ = ["MarkdownIt", "Token", "__version__"]
