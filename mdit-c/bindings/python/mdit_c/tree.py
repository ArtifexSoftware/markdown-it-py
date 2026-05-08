"""A tree representation of a linear ``mdit_c`` token stream.

This is a near-verbatim port of ``markdown_it.tree.SyntaxTreeNode`` —
the upstream module only depends on a ``Token``'s public surface
(``type``, ``nesting``, ``children``, the attribute helpers, and the
property fields), all of which the ``_mdit_c.Token`` class exposes.
The shape of the public API matches upstream exactly, so existing
test code can be re-pointed at this implementation without changes.
"""

from __future__ import annotations

from collections.abc import Generator, Sequence
import textwrap
from typing import Any, NamedTuple, TypeVar, overload

from ._mdit_c import Token


class _NesterTokens(NamedTuple):
    opening: Token
    closing: Token


_NodeType = TypeVar("_NodeType", bound="SyntaxTreeNode")


class SyntaxTreeNode:
    """A Markdown syntax tree node.

    A class that can be used to construct a tree representation of a
    linear ``mdit_c`` token stream. Each node represents either:

    * the root of the Markdown document,
    * a single un-nested ``Token``, or
    * a ``Token`` ``_open`` / ``_close`` pair plus any tokens in
      between.
    """

    def __init__(
        self, tokens: Sequence[Token] = (), *, create_root: bool = True
    ) -> None:
        """Initialize a ``SyntaxTreeNode`` from a token stream.

        If ``create_root`` is True, build a root node for the document.
        """
        # Only nodes representing an un-nested token have ``self.token``.
        self.token: Token | None = None

        # Only containers have nester tokens.
        self.nester_tokens: _NesterTokens | None = None

        # Root node does not have ``self.parent``.
        self._parent: Any = None

        # Empty list unless a non-empty container, or un-nested token
        # that has children (i.e. ``inline`` or ``image``).
        self._children: list[Any] = []

        if create_root:
            self._set_children_from_tokens(tokens)
            return

        if not tokens:
            raise ValueError(
                "Can only create root from empty token sequence."
                " Set `create_root=True`."
            )
        elif len(tokens) == 1:
            inline_token = tokens[0]
            if inline_token.nesting:
                raise ValueError(
                    "Unequal nesting level at the start and end of token stream."
                )
            self.token = inline_token
            if inline_token.children:
                self._set_children_from_tokens(inline_token.children)
        else:
            self.nester_tokens = _NesterTokens(tokens[0], tokens[-1])
            self._set_children_from_tokens(tokens[1:-1])

    def __repr__(self) -> str:
        return f"{type(self).__name__}({self.type})"

    @overload
    def __getitem__(self: _NodeType, item: int) -> _NodeType: ...

    @overload
    def __getitem__(self: _NodeType, item: slice) -> list[_NodeType]: ...

    def __getitem__(
        self: _NodeType, item: int | slice
    ) -> _NodeType | list[_NodeType]:
        return self.children[item]

    def to_tokens(self: _NodeType) -> list[Token]:
        """Recover the linear token stream."""

        def recursive_collect_tokens(
            node: _NodeType, token_list: list[Token]
        ) -> None:
            if node.type == "root":
                for child in node.children:
                    recursive_collect_tokens(child, token_list)
            elif node.token:
                token_list.append(node.token)
            else:
                assert node.nester_tokens
                token_list.append(node.nester_tokens.opening)
                for child in node.children:
                    recursive_collect_tokens(child, token_list)
                token_list.append(node.nester_tokens.closing)

        tokens: list[Token] = []
        recursive_collect_tokens(self, tokens)
        return tokens

    @property
    def children(self: _NodeType) -> list[_NodeType]:
        return self._children

    @children.setter
    def children(self: _NodeType, value: list[_NodeType]) -> None:
        self._children = value

    @property
    def parent(self: _NodeType) -> _NodeType | None:
        return self._parent

    @parent.setter
    def parent(self: _NodeType, value: _NodeType | None) -> None:
        self._parent = value

    @property
    def is_root(self) -> bool:
        """Is the node a special root node?"""
        return not (self.token or self.nester_tokens)

    @property
    def is_nested(self) -> bool:
        """Is this node nested?

        Returns ``True`` if the node represents a ``Token`` pair plus
        the tokens between them.
        """
        return bool(self.nester_tokens)

    @property
    def siblings(self: _NodeType) -> Sequence[_NodeType]:
        """Get siblings of the node, including ``self``."""
        if not self.parent:
            return [self]
        return self.parent.children

    @property
    def type(self) -> str:
        """Get a string type for the represented syntax.

        - ``"root"`` for root nodes.
        - ``Token.type`` if the node represents an un-nested token.
        - ``Token.type`` of the opening token, with ``"_open"`` suffix
          stripped, if the node represents a nester token pair.
        """
        if self.is_root:
            return "root"
        if self.token:
            return self.token.type
        assert self.nester_tokens
        return self.nester_tokens.opening.type.removesuffix("_open")

    @property
    def next_sibling(self: _NodeType) -> _NodeType | None:
        """Get the next node in the sequence of siblings, or ``None``."""
        self_index = self.siblings.index(self)
        if self_index + 1 < len(self.siblings):
            return self.siblings[self_index + 1]
        return None

    @property
    def previous_sibling(self: _NodeType) -> _NodeType | None:
        """Get the previous node in the sequence of siblings, or ``None``."""
        self_index = self.siblings.index(self)
        if self_index - 1 >= 0:
            return self.siblings[self_index - 1]
        return None

    def _add_child(self, tokens: Sequence[Token]) -> None:
        """Make a child node for ``self``."""
        child = type(self)(tokens, create_root=False)
        child.parent = self
        self.children.append(child)

    def _set_children_from_tokens(self, tokens: Sequence[Token]) -> None:
        """Convert the token stream to a tree and attach the resulting
        nodes as children of ``self``."""
        reversed_tokens = list(reversed(tokens))
        while reversed_tokens:
            token = reversed_tokens.pop()

            if not token.nesting:
                self._add_child([token])
                continue
            if token.nesting != 1:
                raise ValueError("Invalid token nesting")

            nested_tokens = [token]
            nesting = 1
            while reversed_tokens and nesting:
                token = reversed_tokens.pop()
                nested_tokens.append(token)
                nesting += token.nesting
            if nesting:
                raise ValueError(f"unclosed tokens starting {nested_tokens[0]}")

            self._add_child(nested_tokens)

    def pretty(
        self,
        *,
        indent: int = 2,
        show_text: bool = False,
        _current: int = 0,
    ) -> str:
        """Create an XML-style string of the tree."""
        prefix = " " * _current
        text = prefix + f"<{self.type}"
        if not self.is_root and self.attrs:
            text += " " + " ".join(f"{k}={v!r}" for k, v in self.attrs.items())
        text += ">"
        if (
            show_text
            and not self.is_root
            and self.type in ("text", "text_special")
            and self.content
        ):
            text += "\n" + textwrap.indent(self.content, prefix + " " * indent)
        for child in self.children:
            text += "\n" + child.pretty(
                indent=indent, show_text=show_text, _current=_current + indent
            )
        return text

    def walk(
        self: _NodeType, *, include_self: bool = True
    ) -> Generator[_NodeType, None, None]:
        """Recursively yield all descendant nodes (depth first)."""
        if include_self:
            yield self
        for child in self.children:
            yield from child.walk(include_self=True)

    # ------------------------------------------------------------------
    # Property pass-through.
    #
    # The values below map directly to properties of the underlying
    # ``Token``. A root node does not translate to a ``Token`` and so
    # raises ``AttributeError`` if any of these are accessed.
    # ``Token.nesting`` is intentionally not surfaced — ``is_nested``
    # provides the same information and works for the root node too.
    # ------------------------------------------------------------------

    def _attribute_token(self) -> Token:
        if self.token:
            return self.token
        if self.nester_tokens:
            return self.nester_tokens.opening
        raise AttributeError("Root node does not have the accessed attribute")

    @property
    def tag(self) -> str:
        """HTML tag name, e.g. ``"p"``."""
        return self._attribute_token().tag

    @property
    def attrs(self) -> dict[str, str | int | float]:
        """HTML attributes."""
        return self._attribute_token().attrs

    def attrGet(self, name: str) -> None | str | int | float:
        """Get the value of attribute ``name``, or ``None``."""
        return self._attribute_token().attrGet(name)

    @property
    def map(self) -> tuple[int, int] | None:
        """Source-map info as ``(line_begin, line_end)`` or ``None``."""
        map_ = self._attribute_token().map
        if map_:
            return tuple(map_)  # type: ignore[return-value]
        return None

    @property
    def level(self) -> int:
        """Nesting level (same as ``state.level``)."""
        return self._attribute_token().level

    @property
    def content(self) -> str:
        """Self-closing-tag content (code, fence, html, …)."""
        return self._attribute_token().content

    @property
    def markup(self) -> str:
        """``'*'`` or ``'_'`` for emphasis, fence string for fence, …"""
        return self._attribute_token().markup

    @property
    def info(self) -> str:
        """Fence infostring."""
        return self._attribute_token().info

    @property
    def meta(self) -> dict[Any, Any]:
        """Plugin metadata bag."""
        return self._attribute_token().meta

    @property
    def block(self) -> bool:
        """True for block-level tokens, false for inline tokens."""
        return self._attribute_token().block

    @property
    def hidden(self) -> bool:
        """If true, ignore this element when rendering (tight lists)."""
        return self._attribute_token().hidden


__all__ = ["SyntaxTreeNode"]
