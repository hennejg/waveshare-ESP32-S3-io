"""Lifting pieces of firmware source into a host test, verbatim.

Both generators here build a test program out of the real C file rather than
out of a copy of it, so the test cannot drift away from the code it guards.
Everything they take -- functions, types, #defines -- comes through this one
module, because the thing that goes wrong is always the same: something is
retyped by hand, the firmware changes, and the test keeps asserting the old
shape while still compiling. Anything not found stops the build.
"""
import pathlib, re


class Source:
    """One firmware file, and the pieces that can be cut out of it."""

    def __init__(self, path: pathlib.Path):
        self.path = path
        self.text = path.read_text()

    def _fail(self, what):
        raise SystemExit("%s: %s not found in %s" % (__name__, what, self.path.name))

    def defines(self, *names):
        """The #define lines themselves, so no number is ever retyped."""
        out = []
        for n in names:
            m = re.search(r"^#define\s+%s\b.*$" % re.escape(n), self.text, re.M)
            if not m:
                self._fail("#define %s" % n)
            out.append(m.group(0))
        return "\n".join(out) + "\n"

    def func(self, signature):
        """The function whose definition starts with signature, braces balanced.

        A signature that has been renamed or had an argument taken off it
        stops the build here rather than quietly leaving the function out."""
        a = self.text.find(signature)
        if a < 0:
            self._fail(signature)
        b = self.text.index("{", a) + 1
        depth = 1
        while depth:
            depth += (self.text[b] == "{") - (self.text[b] == "}")
            b += 1
        return self.text[a:b] + "\n"

    def type(self, ends_with):
        """A whole type declaration, found from its last line and balanced
        backwards, so every field comes across unread.

        ends_with is the text it closes with, e.g. "} conn_t;"."""
        b = self.text.find(ends_with)
        if b < 0:
            self._fail("type ending %r" % ends_with)
        b += len(ends_with)

        j = self.text.rindex("}", 0, b)
        depth = 1
        while depth:
            j -= 1
            depth += (self.text[j] == "}") - (self.text[j] == "{")
        a = self.text.rindex("\n", 0, j) + 1
        return self.text[a:b] + "\n"

    def typedef_enum(self, name):
        m = re.search(r"^typedef enum \{[^}]*\} %s;$" % re.escape(name), self.text, re.M)
        if not m:
            self._fail("typedef enum %s" % name)
        return m.group(0) + "\n"

    def span(self, first, last):
        """From the start of first to the end of last -- for declarations that
        only make sense together with the comment that explains them."""
        a = self.text.find(first)
        if a < 0:
            self._fail(first)
        b = self.text.find(last, a)
        if b < 0:
            self._fail(last)
        return self.text[a:b + len(last)] + "\n"
