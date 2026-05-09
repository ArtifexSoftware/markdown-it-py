"""mdit_c — Python bindings for the C port of markdown-it-py.

The compiled ``._mdit_c`` module owns the parser and renderer. This
package layers an upstream-shaped Python facade on top so most
``markdown_it.MarkdownIt`` user code keeps working without changes.

Supported today (Phase 5, slice 7):

* ``MarkdownIt(config="commonmark", options=None)`` with preset
  aliases ``default``/``js-default``, ``commonmark``, ``zero``,
  ``gfm-like``, ``gfm-like2``; ``preset=`` and ``options_update=``
  keyword aliases for compatibility with upstream tests.
* ``MarkdownIt.render(src, env=None)``,
  ``MarkdownIt.parse(src, env=None) -> list[Token]``,
  ``MarkdownIt.renderInline(src, env=None)``, and
  ``MarkdownIt.parseInline(src, env=None) -> list[Token]`` — when
  ``env`` is a ``MutableMapping``, link references and duplicates
  discovered during parsing are written back as
  ``env["references"]`` and ``env["duplicate_refs"]`` in the same
  shape upstream produces.
* ``MarkdownIt.options`` as a live dict that mirrors the C
  ``mdit_options`` struct on every parse/render call.
* ``MarkdownIt.enable(...)`` and ``MarkdownIt.disable(...)`` over
  every ruler chain at once, plus per-chain access through
  ``md.core.ruler``, ``md.block.ruler``, ``md.inline.ruler`` and
  ``md.inline.ruler2`` (``get_all_rules``, ``get_active_rules``,
  ``enable``, ``disable``, ``enableOnly``).
* ``MarkdownIt.reset_rules()`` context manager that snapshots the
  active rules and restores them on exit.
* ``MarkdownIt.add_render_rule(name, fn, fmt="html")`` registers a
  Python callable that the C renderer invokes for tokens of the
  given type. The callback receives ``self`` (the ``RendererHTML``
  facade), the live token list, the index, the options dict, and
  the ``env`` argument from ``render``. Returning a string emits it
  verbatim. ``self.renderToken``/``self.renderAttrs`` are available
  for delegation.
* ``MarkdownIt.use(plugin, *args, **kwargs)`` chainable plugin entry
  point.
* ``Token`` with the upstream field layout, constructor, ``as_dict``,
  ``from_dict``, ``copy``, equality, and ``attr*`` helpers.
* ``SyntaxTreeNode`` (also exposed as ``mdit_c.tree.SyntaxTreeNode``)
  — pure-Python tree wrapper around a parsed token stream. Mirrors
  ``markdown_it.tree.SyntaxTreeNode`` 1:1, including ``children``,
  ``parent``, ``walk``, ``to_tokens``, ``pretty``, ``next_sibling`` /
  ``previous_sibling``, and the ``Token`` property pass-through
  (``tag``, ``attrs``, ``map``, ``level``, ``content``, ``markup``,
  ``info``, ``meta``, ``block``, ``hidden``).
* Parser rule callbacks: ``md.core.ruler.before/after/at/push``,
  ``md.block.ruler.*``, ``md.inline.ruler.*``, ``md.inline.ruler2.*``
  accept Python callables. Core rules receive ``state`` (a read-only
  ``StateCore`` view); block rules receive ``(state, startLine,
  endLine, silent)``; inline rules receive ``(state, silent)``. State
  wrappers expose ``src``/``env``/``md`` plus a small set of
  chain-specific scalars (``inlineMode`` for core; ``line``/
  ``lineMax``/``blkIndent``/``level``/``tight``/``parentType`` for
  block; ``pos`` (writable)/``posMax``/``level``/``pendingLevel``/
  ``pending``/``linkLevel`` for inline). Mutating ``state.tokens``
  from a Python rule is not yet supported — see the port plan for
  follow-up scope.
"""

from __future__ import annotations

from contextlib import contextmanager
from typing import Any, Callable, Iterable, Iterator

from ._mdit_c import MarkdownIt as _MarkdownIt
from ._mdit_c import Token, __version__
from .tree import SyntaxTreeNode


def _escape_html(value: object) -> str:
    text = str(value)
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


class RendererHTML:
    """Small upstream-shaped renderer facade.

    The C renderer still performs the default rendering. This Python
    object exists so custom render rules receive a familiar ``self``
    argument with ``renderToken`` / ``renderAttrs`` helpers.
    """

    __output__ = "html"

    def __init__(self, parser: MarkdownIt | None = None):
        self.parser = parser
        self.rules: dict[str, Callable[..., str]] = {}

    def renderToken(
        self, tokens: list[Token], idx: int, options: dict[str, Any], env: Any
    ) -> str:
        del env
        token = tokens[idx]
        if token.hidden:
            return ""

        result = ""
        if token.block and token.nesting != -1 and idx and tokens[idx - 1].hidden:
            result += "\n"

        result += ("</" if token.nesting == -1 else "<") + token.tag
        result += self.renderAttrs(token)
        if token.nesting == 0 and options.get("xhtmlOut", False):
            result += " /"

        need_lf = False
        if token.block:
            need_lf = True
            if token.nesting == 1 and idx + 1 < len(tokens):
                next_token = tokens[idx + 1]
                if next_token.type == "inline" or next_token.hidden:
                    need_lf = False
                elif next_token.nesting == -1 and next_token.tag == token.tag:
                    need_lf = False

        return result + (">\n" if need_lf else ">")

    @staticmethod
    def renderAttrs(token: Token) -> str:
        return "".join(
            f' {_escape_html(key)}="{_escape_html(value)}"'
            for key, value in token.attrItems()
        )

    def renderInlineAsText(
        self, tokens: Iterable[Token] | None, options: dict[str, Any], env: Any
    ) -> str:
        del options, env
        result = ""
        for token in tokens or []:
            if token.type == "text":
                result += token.content
            elif token.type == "image" and token.children:
                result += self.renderInlineAsText(token.children, {}, None)
            elif token.type == "softbreak":
                result += "\n"
        return result


class Ruler:
    """Facade over a C ``mdit_ruler`` owned by a ``MarkdownIt`` instance."""

    def __init__(self, parser: MarkdownIt, chain: str):
        self._parser = parser
        self._chain = chain

    def get_all_rules(self) -> list[str]:
        return self._parser._ruler_get_all(self._chain)

    def get_active_rules(self) -> list[str]:
        return self._parser._ruler_get_active(self._chain)

    def enable(
        self, names: str | Iterable[str], ignoreInvalid: bool = False
    ) -> list[str]:
        return self._parser._ruler_enable(self._chain, names, ignoreInvalid)

    def disable(
        self, names: str | Iterable[str], ignoreInvalid: bool = False
    ) -> list[str]:
        return self._parser._ruler_disable(self._chain, names, ignoreInvalid)

    def enableOnly(
        self, names: str | Iterable[str], ignoreInvalid: bool = False
    ) -> list[str]:
        return self._parser._ruler_enable_only(self._chain, names, ignoreInvalid)

    def before(
        self,
        beforeName: str,
        ruleName: str,
        fn: Callable[..., Any],
        options: dict[str, Any] | None = None,
    ) -> None:
        if options:
            raise NotImplementedError(
                "rule options (e.g. alt chains) are not yet supported"
            )
        self._parser._ruler_install(
            self._chain, "before", ruleName, fn, beforeName
        )

    def after(
        self,
        afterName: str,
        ruleName: str,
        fn: Callable[..., Any],
        options: dict[str, Any] | None = None,
    ) -> None:
        if options:
            raise NotImplementedError(
                "rule options (e.g. alt chains) are not yet supported"
            )
        self._parser._ruler_install(
            self._chain, "after", ruleName, fn, afterName
        )

    def at(
        self,
        ruleName: str,
        fn: Callable[..., Any],
        options: dict[str, Any] | None = None,
    ) -> None:
        if options:
            raise NotImplementedError(
                "rule options (e.g. alt chains) are not yet supported"
            )
        self._parser._ruler_install(self._chain, "at", ruleName, fn)

    def push(
        self,
        ruleName: str,
        fn: Callable[..., Any],
        options: dict[str, Any] | None = None,
    ) -> None:
        if options:
            raise NotImplementedError(
                "rule options (e.g. alt chains) are not yet supported"
            )
        self._parser._ruler_install(self._chain, "push", ruleName, fn)


class _ParserFacade:
    def __init__(self, parser: MarkdownIt, chain: str):
        self.ruler = Ruler(parser, chain)


class _InlineParserFacade(_ParserFacade):
    def __init__(self, parser: MarkdownIt):
        super().__init__(parser, "inline")
        self.ruler2 = Ruler(parser, "inline2")


class MarkdownIt(_MarkdownIt):
    """Compatibility subclass over the compiled C parser."""

    def __init__(self, *args: Any, **kwargs: Any):
        renderer_cls = kwargs.pop("renderer_cls", RendererHTML)
        super().__init__(*args, **kwargs)
        self.renderer = renderer_cls(self)
        self.core = _ParserFacade(self, "core")
        self.block = _ParserFacade(self, "block")
        self.inline = _InlineParserFacade(self)

    def __getitem__(self, name: str) -> Any:
        return {
            "inline": self.inline,
            "block": self.block,
            "core": self.core,
            "renderer": self.renderer,
        }[name]

    def get_all_rules(self) -> dict[str, list[str]]:
        return {
            "core": self.core.ruler.get_all_rules(),
            "block": self.block.ruler.get_all_rules(),
            "inline": self.inline.ruler.get_all_rules(),
            "inline2": self.inline.ruler2.get_all_rules(),
        }

    def get_active_rules(self) -> dict[str, list[str]]:
        return {
            "core": self.core.ruler.get_active_rules(),
            "block": self.block.ruler.get_active_rules(),
            "inline": self.inline.ruler.get_active_rules(),
            "inline2": self.inline.ruler2.get_active_rules(),
        }

    @contextmanager
    def reset_rules(self) -> Iterator[None]:
        active = self.get_active_rules()
        try:
            yield
        finally:
            self.core.ruler.enableOnly(active["core"])
            self.block.ruler.enableOnly(active["block"])
            self.inline.ruler.enableOnly(active["inline"])
            self.inline.ruler2.enableOnly(active["inline2"])

    def add_render_rule(
        self, name: str, function: Callable[..., Any], fmt: str = "html"
    ) -> None:
        if self.renderer.__output__ != fmt:
            return
        get = getattr(function, "__get__", None)
        callback = get(self.renderer) if get is not None else function
        self.renderer.rules[name] = callback
        self._add_render_rule(name, callback)

    def use(self, plugin: Callable[..., None], *params: Any, **options: Any) -> MarkdownIt:
        plugin(self, *params, **options)
        return self


__all__ = [
    "MarkdownIt",
    "RendererHTML",
    "Ruler",
    "SyntaxTreeNode",
    "Token",
    "__version__",
]
