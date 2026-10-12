"""Lifting pieces of firmware source into a host test, verbatim.

Both generators here build a test program out of the real C file rather than
out of a copy of it, so the test cannot drift away from the code it guards.
Everything they take -- functions, types, #defines -- comes through this one
module, because the thing that goes wrong is always the same: something is
retyped by hand, the firmware changes, and the test keeps asserting the old
shape while still compiling. Anything not found stops the build.
"""
import os, pathlib, re, subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]


def show(rel):
    """A repository file as text: the working tree, or with OLD=1 as it was at
    OLDREV (default HEAD) -- the convention every generator's OLD run uses."""
    if os.environ.get("OLD") == "1":
        rev = os.environ.get("OLDREV", "HEAD")
        return subprocess.run(["git", "show", "%s:%s" % (rev, rel)], cwd=ROOT,
                              check=True, capture_output=True, text=True).stdout
    return (ROOT / rel).read_text()


class Source:
    """One firmware file, and the pieces that can be cut out of it."""

    def __init__(self, path: pathlib.Path):
        self.path = path
        self.text = path.read_text()

    @classmethod
    def at(cls, rel, *was):
        """The file at rel (from the repository root), honouring OLD/OLDREV.
        was: where the file lived before it moved, tried in turn when OLDREV
        does not have rel yet."""
        s = cls.__new__(cls)
        for r in (rel,) + was:
            try:
                s.text = show(r)
            except (subprocess.CalledProcessError, FileNotFoundError):
                if r == ((rel,) + was)[-1]:
                    raise
                continue
            s.path = ROOT / r
            return s

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


def kconfig_default(rel, *names):
    """#define CONFIG_<name> <default> for Kconfig ints, read from the Kconfig
    file at rel -- the value a build gets unless an sdkconfig says otherwise,
    so no number is retyped here either."""
    import re
    text = show(rel)
    out = []
    for n in names:
        m = re.search(r"^config %s\b.*?^\s+default\s+(\S+)" % re.escape(n), text, re.S | re.M)
        if not m:
            raise SystemExit("cutter: %s has no default in %s" % (n, rel))
        out.append("#define CONFIG_%s %s\n" % (n, m.group(1)))
    return "".join(out)


def json_exact_if_used(cut):
    """json_parse_exact() (components/board/json_exact.c) when cut code calls
    it -- the payload parsers take the whole payload or nothing since then --
    and nothing for code from before it existed."""
    if "json_parse_exact(" not in cut:
        return ""
    jx = Source.at("components/board/json_exact.c")
    return ("#include <string.h>\n"
            + (jx.func("static bool escapes_nul(const char *d, size_t n)") if "static bool escapes_nul(" in jx.text else "")
            + jx.func("cJSON *json_parse_exact(const char *data, size_t n)"))
