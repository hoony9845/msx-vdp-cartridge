"""Minimal S-expression reader for KiCad library files."""
import re

_TOKEN = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+')


def parse(text):
    stack = [[]]
    for tok in _TOKEN.findall(text):
        if tok == "(":
            stack.append([])
        elif tok == ")":
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append(tok)
    return stack[0]


def unquote(tok):
    if isinstance(tok, str) and tok.startswith('"'):
        return tok[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return tok


def find_block(text, head):
    """Return the raw text of the top-level block that starts with `head`."""
    start = text.find(head)
    if start < 0:
        raise KeyError(head)
    depth = 0
    i = start
    in_str = False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise ValueError("unbalanced block for " + head)


def find_all(node, name):
    """Yield all sub-lists (recursively) whose first element is `name`."""
    if isinstance(node, list):
        if node and node[0] == name:
            yield node
        for child in node:
            yield from find_all(child, name)
