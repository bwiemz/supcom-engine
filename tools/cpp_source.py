#!/usr/bin/env python3
"""Read C++ source as the compiler does: its comments and literals found.

The source checks in tools/ look for code, so each must skip what a comment
or a literal holds; this is the one reader they share. scan(text) gives

- `code`: the text with every comment, and every string and character
  literal's contents, blanked to spaces. Its length and line breaks are the
  text's, so an offset or a line in it is the same in the text. A literal
  keeps its prefix and quotes (`u8R"..."`, `'.'`), its contents blank
  (a raw string's delimiters too).
- `literals`: each string literal, with its contents as written.

The rules are C++'s: a `//` or `/*` inside a literal is no comment, and a
quote inside a comment opens no literal. A literal takes an encoding prefix
(u8, u, U, L) and R for a raw string, `R"delim( ... )delim"` (no escapes,
line breaks kept), but only as a whole word: `FOR"a"` is the identifier
FOR and the string "a". A quote within a number is a digit separator
(1'000, 0xFF'FF), else it opens a character literal. A plain literal ends
at its closing quote or, unterminated, at the line's end.

Usage: cpp_source.py --self-test
"""

from __future__ import annotations

import bisect
import re
import sys
from dataclasses import dataclass, field

_IDENT = re.compile(r"[A-Za-z_]\w*")
# A pp-number: digits, letters, '.', digit separators, and an exponent's sign
_NUMBER = re.compile(r"\.?\d(?:[eEpP][+-]|['\w.])*")
_RAW_DELIM = re.compile(r'"([^()\\\s]{0,16})\(')
_STRING_PREFIXES = {"u8", "u", "U", "L", "R", "u8R", "uR", "UR", "LR"}
_CHAR_PREFIXES = {"u8", "u", "U", "L"}


@dataclass(frozen=True)
class Literal:
    """A string literal: `text[start:end]` is all of it, prefix and quotes
    included; `body` its contents as written."""

    start: int
    end: int
    body: str
    line: int
    raw: bool


@dataclass
class Source:
    text: str
    code: str
    literals: list[Literal] = field(default_factory=list)
    _breaks: list[int] = field(default_factory=list)
    _blank: list[tuple[int, int]] = field(default_factory=list)

    def line_of(self, offset: int) -> int:
        """The 1-based line `offset` is on."""
        return bisect.bisect_left(self._breaks, offset) + 1

    def in_code(self, offset: int) -> bool:
        """Whether `offset` lies in code: not in a comment, nor in a literal's
        contents (its quotes and prefix are code)."""
        k = bisect.bisect_right(self._blank, (offset, len(self.text) + 1)) - 1
        return k < 0 or not self._blank[k][0] <= offset < self._blank[k][1]


def _blank(text: str, start: int, end: int) -> str:
    return re.sub(r"[^\n]", " ", text[start:end])


def scan(text: str) -> Source:
    """`text` read as C++: see the module's docstring."""
    out: list[str] = []
    literals: list[Literal] = []
    blank: list[tuple[int, int]] = []
    n = len(text)
    i = 0
    done = 0  # text[:done] is in `out`

    def hide(start: int, end: int) -> None:
        nonlocal done
        out.append(text[done:start])
        out.append(_blank(text, start, end))
        blank.append((start, end))
        done = end

    def plain(quote_at: int, quote: str) -> tuple[int, int]:
        """The plain literal whose quote is at `quote_at`: where its contents
        end, and the index after it."""
        j = quote_at + 1
        while j < n and text[j] != quote and text[j] != "\n":
            j += 2 if text[j] == "\\" else 1
        j = min(j, n)
        return j, j + 1 if j < n and text[j] == quote else j

    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            hide(i, j)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            hide(i, j)
            i = j
        elif c.isalpha() or c == "_":
            m = _IDENT.match(text, i)
            assert m is not None  # it starts with a letter or '_'
            word, j = m.group(0), m.end()
            if j < n and text[j] == '"' and word in _STRING_PREFIXES:
                raw = _RAW_DELIM.match(text, j) if word.endswith("R") else None
                if raw is not None:
                    close = text.find(")" + raw.group(1) + '"', raw.end())
                    if close < 0:  # unterminated: the rest of the text
                        literals.append(Literal(i, n, text[raw.end() :], 0, True))
                        hide(j + 1, n)
                        i = n
                    else:
                        end = close + len(raw.group(1)) + 2
                        literals.append(Literal(i, end, text[raw.end() : close], 0, True))
                        hide(j + 1, end - 1)
                        i = end
                else:
                    body_end, end = plain(j, '"')
                    literals.append(Literal(i, end, text[j + 1 : body_end], 0, False))
                    hide(j + 1, body_end)
                    i = end
            elif j < n and text[j] == "'" and word in _CHAR_PREFIXES:
                body_end, end = plain(j, "'")
                hide(j + 1, body_end)
                i = end
            else:
                i = j
        elif c.isdigit() or (c == "." and i + 1 < n and text[i + 1].isdigit()):
            m = _NUMBER.match(text, i)
            i = m.end() if m else i + 1
        elif c == '"':
            body_end, end = plain(i, '"')
            literals.append(Literal(i, end, text[i + 1 : body_end], 0, False))
            hide(i + 1, body_end)
            i = end
        elif c == "'":
            body_end, end = plain(i, "'")
            hide(i + 1, body_end)
            i = end
        else:
            i += 1
    out.append(text[done:])
    breaks = [k for k, ch in enumerate(text) if ch == "\n"]
    src = Source(text, "".join(out), [], breaks, blank)
    src.literals = [
        Literal(lit.start, lit.end, lit.body, src.line_of(lit.start), lit.raw) for lit in literals
    ]
    return src


def self_test() -> int:
    failed = 0

    def expect(what: str, got: object, want: object) -> None:
        nonlocal failed
        if got != want:
            print(f"self-test: {what}: got {got!r}, want {want!r}")
            failed += 1

    def bodies(text: str) -> list[str]:
        return [lit.body for lit in scan(text).literals]

    cases = [
        'int a; // b "c"\n/* d \'e\n f */ g("h//i", \'j\', "k/*l");\n',
        'auto s = u8R"x(a)b\n"c)x"; f(LR"(q)", R"d(e)d");\n',
        "x = 1'000 + 0xFF'FF'u + 1e+5; c = L'\"'; w = u8'x';\n",
        'int FOR"a"; return"//"; "esc\\"aped"; "line\\\ncont";\n',
        '"unterminated\n"next"; /* open',
        "",
    ]
    for text in cases:
        src = scan(text)
        expect("the length is kept", len(src.code), len(text))
        expect(
            "the line breaks are kept",
            [k for k, ch in enumerate(src.code) if ch == "\n"],
            [k for k, ch in enumerate(text) if ch == "\n"],
        )
    expect("a comment opener in a string", bodies('g("h//i"); k("/*l");'), ["h//i", "/*l"])
    code = scan('int a; // b "c"\n/* d\n e */ g("h//i");\n').code
    expect(
        "comments and contents blank",
        code,
        "int a;" + " " * 9 + "\n" + " " * 4 + "\n" + " " * 6 + 'g("    ");\n',
    )
    expect("raw strings and their prefixes", bodies(cases[1]), ['a)b\n"c', "q", "e"])
    expect("a raw string blanks whole", scan('R"x(a)x";').code, 'R"     ";')
    expect("digit separators and wide chars", bodies(cases[2]), [])
    expect("an identifier ending in R, a keyword", bodies(cases[3])[:2], ["a", "//"])
    expect("escapes and line continuations", bodies(cases[3])[2:], ['esc\\"aped', "line\\\ncont"])
    expect("an unterminated literal", bodies(cases[4]), ["unterminated", "next"])
    src = scan('a\n"b"\nc /* d */ e')
    expect("literal lines", [lit.line for lit in src.literals], [2])
    expect("line_of", [src.line_of(k) for k in (0, 1, 2, 6, len(src.text))], [1, 1, 2, 3, 3])
    expect(
        "in_code",
        [src.in_code(k) for k in (0, 2, 3, 4, 8, 10, 15)],
        [True, True, False, True, False, False, True],
    )
    if failed:
        return 1
    print("self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
