"""mdit_c — Python bindings for the C port of markdown-it-py.

This package re-exports the ``MarkdownIt`` class implemented in the
``_mdit_c`` extension module. The intent is for ``import mdit_c`` to
behave as a drop-in alternative to ``import markdown_it`` for the
subset of the API that the C engine currently supports.

Today, that subset is:

* ``MarkdownIt(config="commonmark", options=None)``. Preset names
  ``default`` / ``js-default``, ``commonmark``, ``zero``,
  ``gfm-like``, and ``gfm-like2`` are all recognised. The legacy
  ``preset=`` keyword alias is accepted for compatibility.
* ``MarkdownIt.render(src) -> str``
* ``MarkdownIt.parse(src, env=None) -> list[Token]``
* ``Token`` with the upstream field layout (``type``, ``tag``,
  ``nesting``, ``attrs``, ``map``, ``children``, ``content``,
  ``markup``, ``info``, ``meta``, ``block``, ``hidden``), constructor,
  ``as_dict(...)``, ``from_dict``, ``copy``, ``attr*`` helpers.
* ``MarkdownIt.options`` — a live mapping where mutations propagate
  into the C engine on the next ``parse``/``render`` call. Mirrors
  upstream's ``OptionsDict``-backed ``options`` attribute.
* ``MarkdownIt.enable(names, ignoreInvalid=False)`` and
  ``MarkdownIt.disable(...)``.

``SyntaxTreeNode`` and the richer plugin/ruler Python API (including
custom render rules and Python-level ``highlight`` callables) are not
exposed yet — they're queued for follow-up slices of Phase 5.
"""

from __future__ import annotations

from ._mdit_c import MarkdownIt, Token, __version__

__all__ = ["MarkdownIt", "Token", "__version__"]
