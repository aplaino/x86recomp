#!/usr/bin/env python3
"""Land a topic branch on main: rebase, build and test what it touches,
fast-forward.

Run from the topic's worktree: tools/land.py [--dry-run]

1. Refuses a dirty worktree.
2. Fetches and rebases the topic onto origin/main.
3. Picks the presets the changed paths call for, builds each in
   out/land/<preset> and runs its ctest (niced, memory-capped). Docs
   (.md) and C/C++ files whose change is only to comments and whitespace
   build nothing.
4. Pushes the commit it rebased, built and tested to origin/main, a
   fast-forward only, after checking that the worktree is still clean and
   on that commit. If main moved in the meantime the push is rejected:
   rerun.
5. Fast-forwards the checkout that has main, when it is clean.

It does not run the game projects; it prints which ones the change calls
for. RECOMP_PYTHON (the oracle tests' venv Python, unresolved) and
DXVK_NATIVE_DIR are passed to every configure when set in the
environment.
"""
import argparse
import bisect
import os
import re
import subprocess
import sys

# Path prefix -> presets. The first matching prefix wins; a path no rule
# names builds every preset, as nothing says which ones use it.
EVERY = ["debug", "x86", "ppc", "a64", "a64-aarch64-qemu", "orbis",
         "xbox360", "horizon"]
RULES = [
    ("tools/land.py", []),
    ("src/runtime/", EVERY),
    ("cmake/", EVERY),
    ("CMakeLists.txt", EVERY),
    ("CMakePresets.json", EVERY),
    ("src/text_hash.h", ["ppc", "a64", "a64-aarch64-qemu", "xbox360", "horizon"]),
    ("tools/orbis_sce_elf.py", ["orbis"]),
    ("tests/x86/", ["x86", "debug", "orbis"]),
    ("tests/ppc/", ["ppc", "xbox360"]),
    ("tests/a64/", ["a64", "a64-aarch64-qemu", "horizon"]),
    ("tests/", EVERY),
    ("src/x86/", ["x86", "debug", "orbis"]),
    ("tools/lifter_x86/", ["x86", "debug", "orbis"]),
    ("src/ppc/", ["ppc", "xbox360"]),
    ("tools/lifter_ppc/", ["ppc", "xbox360"]),
    ("src/a64/", ["a64", "a64-aarch64-qemu", "horizon"]),
    ("tools/lifter_a64/", ["a64", "a64-aarch64-qemu", "horizon"]),
    ("platforms/win32/", ["debug"]),
    ("platforms/orbis/", ["orbis"]),
    ("platforms/xbox360/", ["xbox360"]),
    ("platforms/horizon/", ["horizon"]),
]

# Path prefix -> game projects (CLAUDE.md, Layout) to rebuild and test
# afterwards.
PC = "the Win32 (PC) games"
GAMES = [
    ("src/runtime/", "every game project"),
    ("cmake/", "every game project"),
    ("CMakeLists.txt", "every game project"),
    ("src/text_hash.h", "the Xbox 360 and Switch games (re-lift)"),
    ("src/x86/", PC + " and the PS4 games"),
    ("tools/lifter_x86/", PC + " and the PS4 games (re-lift)"),
    ("platforms/win32/", PC),
    ("platforms/orbis/", "the PS4 games"),
    ("src/ppc/", "the Xbox 360 games"),
    ("tools/lifter_ppc/", "the Xbox 360 games (re-lift)"),
    ("platforms/xbox360/", "the Xbox 360 games"),
    ("src/a64/", "the Switch games"),
    ("tools/lifter_a64/", "the Switch games (re-lift)"),
    ("platforms/horizon/", "the Switch games"),
]


def git(*args, check=True):
    return subprocess.run(["git", *args], check=check, text=True,
                          capture_output=True).stdout.strip()


def match(path, rules):
    for prefix, value in rules:
        if path == prefix or path.startswith(prefix):
            return value
    return None


C_FAMILY = (".c", ".cc", ".cpp", ".h", ".hpp", ".inl")
# Translation phase 2: a backslash ending a line (clang allows blanks
# after it) joins the next one to it, in comments and directives too.
SPLICE = re.compile(r"\\[ \t]*\r?\n")
# Phase 3's tokens, as far as telling code from comments needs: a raw
# string (from any token start, with its encoding prefix), a header name
# (in which // and /* are not comments), a string or character literal,
# a comment, a newline, other whitespace, then code: a pp-number (digit
# separators included), an identifier, a run of punctuation or any other
# single character.
_RAW = r'(?P<raw>(?:u8|[uUL])?R"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)")|'
_BLANK = r"(?:[ \t]|/\*.*?\*/)*"
_REST = (r"(?P<header>(?:#|%:)" + _BLANK + r"(?:include|include_next|import)" + _BLANK
         + r"<[^>\n]*>|__has_include(?:_next)?\s*\(\s*<[^>\n]*>)"
         r"""|(?P<literal>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')"""
         r"|(?P<comment>//[^\n]*|/\*.*?\*/)|(?P<newline>\n)|(?P<space>[^\S\n]+)"
         r"""|(?P<code>\.?\d(?:[eEpP][+-]|'\w|[\w.])*|[\w$]+|[^\w$\s"'/]+|.)""")
# C has no raw strings and a header can be either, so both readings.
TOKENS = {True: re.compile(_RAW + _REST, re.S), False: re.compile(_REST, re.S)}


def code_tokens(text, raw):
    """The code tokens of text, in order: comments dropped, whitespace
    (comments included) kept as one " " where it separates two tokens
    (stringizing and macro definitions see it), and the end of each
    preprocessing directive kept as a newline."""
    starts, originals, parts, at = [0], [0], [], 0
    for m in SPLICE.finditer(text):
        parts.append(text[at:m.start()])
        at = m.end()
        starts.append(starts[-1] + len(parts[-1]))
        originals.append(at)
    parts.append(text[at:])

    def original(i):  # i's offset in text, before the lines were joined
        k = bisect.bisect_right(starts, i) - 1
        return originals[k] + i - starts[k]

    tokens, space, line_start, directive = [], False, True, False
    for m in TOKENS[raw].finditer("".join(parts)):
        kind = m.lastgroup
        if kind == "newline" and directive:
            tokens.append("\n")
            space = directive = False
        elif kind in ("newline", "space", "comment"):
            space = bool(tokens) and tokens[-1] != "\n"
        else:
            token = m.group()
            if kind == "raw":  # phase 2 does not apply inside raw strings
                token = text[original(m.start()):original(m.end() - 1) + 1]
            if line_start:
                directive = token.startswith(("#", "%:"))
            tokens += [" ", token] if space else [token]
            space = False
        line_start = kind == "newline" or line_start and kind in ("space", "comment")
    return tokens


def same_code(old, new):
    return all(code_tokens(old, raw) == code_tokens(new, raw) for raw in (False, True))


def comment_only(path, head):
    """A C/C++ file whose change since origin/main is only to comments
    and whitespace (the code tokens are the same)."""
    if not path.endswith(C_FAMILY):
        return False
    old = subprocess.run(["git", "show", f"origin/main:{path}"], text=True,
                         capture_output=True)
    new = subprocess.run(["git", "show", f"{head}:{path}"], text=True,
                         capture_output=True)
    if old.returncode != 0 or new.returncode != 0:
        return False
    return same_code(old.stdout, new.stdout)


# (old, new, whether the change is to comments and whitespace only).
SELF_TEST = [
    ("int a; // one\n", "int a; // two\n", True),
    ("a();\nb();\n", "a();\n// why\nb();\n", True),
    ("/* a\n b */\nint x;\n", "/* c */\nint x;\n", True),
    ("#define X 1 /* one */\n", "#define X 1 /* two */\n", True),
    ("x = 1'000; // it's\n", "x = 1'000; // it is\n", True),
    ('f(R"(a)"); // x\n', 'f(R"(a)"); // y\n', True),
    # Raw strings wherever a token starts, with every prefix.
    *((f'{lead}{prefix}R"(" /* a */ ")";\n', f'{lead}{prefix}R"(" /* b */ ")";\n', False)
      for lead in ("f(", "s=", "s = ") for prefix in ("", "u8", "L", "u", "U")),
    ('s = R"d(")" // a\n)d";\n', 's = R"d(")" // b\n)d";\n', False),
    ('s = R"(a\\\nb)";\n', 's = R"(ab)";\n', False),
    # Newlines end directives; spliced lines do not end them.
    ("#define X a\nb;\n", "#define X a b;\n", False),
    ("#define X a \\\n b\nc;\n", "#define X a \\\n b c;\n", False),
    ("// note\nint x;\n", "// note \\\nint x;\n", False),
    ("/* a *\\\n/ int x; /* b */\n", "/* a *\\\n/ int y; /* b */\n", False),
    # Whitespace that separates tokens.
    ("#define F(x) x\n", "#define F (x) x\n", False),
    ("S(a+b);\n", "S(a + b);\n", False),
    # Header names.
    ("#include <a//b.h>\n", "#include <a//c.h>\n", False),
    ("#if __has_include(<a/*b.h>)\n", "#if __has_include(<a/*c.h>)\n", False),
]


def self_test():
    failed = [(old, new) for old, new, want in SELF_TEST if same_code(old, new) != want]
    for old, new in failed:
        print(f"land: self-test: {old!r} -> {new!r} classified wrongly")
    return not failed


def presets_for(paths):
    picked = set()
    for path in paths:
        if path.endswith(".md"):
            continue
        rule = match(path, RULES)
        picked.update(EVERY if rule is None else rule)
    return [p for p in EVERY if p in picked]


def run_preset(preset, root):
    build = os.path.join(root, "out", "land", preset)
    defs = [f"-D{v}={os.environ[v]}" for v in ("RECOMP_PYTHON", "DXVK_NATIVE_DIR")
            if os.environ.get(v)]
    steps = [
        ["cmake", "--preset", preset, "-B", build, *defs],
        ["cmake", "--build", build, "-j", str(max(1, os.cpu_count() - 4))],
        ["ctest", "--test-dir", build, "-j", "8", "--output-on-failure"],
    ]
    for step in steps:
        cmd = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=24G",
               "nice", "-n", "10", *step]
        print("  $", " ".join(step), flush=True)
        if subprocess.run(cmd, cwd=root).returncode != 0:
            return False
    return True


def main_checkout():
    for block in git("worktree", "list", "--porcelain").split("\n\n"):
        lines = dict(l.split(" ", 1) for l in block.splitlines() if " " in l)
        if lines.get("branch") == "refs/heads/main":
            return lines["worktree"]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true",
                    help="rebase and pick presets, build nothing, push nothing")
    ap.add_argument("--self-test", action="store_true",
                    help="check the comments-only classifier on known cases, then stop")
    args = ap.parse_args()
    if not self_test():
        sys.exit("land: the comments-only classifier fails its self-test")
    if args.self_test:
        print(f"land: self-test: {len(SELF_TEST)} cases pass")
        return

    root = git("rev-parse", "--show-toplevel")
    topic = git("symbolic-ref", "--short", "HEAD")
    if topic == "main":
        sys.exit("land: run from a topic branch's worktree, not main")
    if git("status", "--porcelain"):
        sys.exit("land: the worktree has uncommitted changes")

    git("fetch", "origin", "main")
    # A topic already on top of main is not rebased: that keeps a merge
    # commit's history as it is instead of flattening it.
    up_to_date = subprocess.run(
        ["git", "merge-base", "--is-ancestor", "origin/main", "HEAD"]).returncode == 0
    if not up_to_date and subprocess.run(["git", "rebase", "origin/main"]).returncode != 0:
        sys.exit("land: rebase stopped; resolve, `git rebase --continue`, rerun")

    # What is built, tested and pushed: a commit made in the worktree
    # while the presets build must not ride along untested.
    head = git("rev-parse", "HEAD")
    paths = git("diff", "--name-only", f"origin/main...{head}").splitlines()
    if not paths:
        sys.exit("land: nothing to land")
    comments = [p for p in paths if comment_only(p, head)]
    if comments:
        print("land: comments only, nothing to build:", ", ".join(comments))
    paths = [p for p in paths if p not in comments]
    commits = git("log", "--oneline", f"origin/main..{head}")
    print(f"land: {topic}, onto origin/main:\n{commits}\n")

    presets = presets_for(paths)
    print("land: presets:", ", ".join(presets) or "none (docs and comments only)")
    if args.dry_run:
        return
    for preset in presets:
        print(f"land: {preset}", flush=True)
        if not run_preset(preset, root):
            sys.exit(f"land: {preset} failed; nothing pushed")

    if git("status", "--porcelain") or git("rev-parse", "HEAD") != head:
        sys.exit(f"land: the worktree changed during the builds; nothing pushed "
                 f"(tested {head[:12]}); rerun")
    push = subprocess.run(["git", "push", "origin", f"{head}:refs/heads/main"])
    if push.returncode != 0:
        sys.exit("land: push failed (main moved, or the remote could not be "
                 "reached; see git's message); rerun to rebase and retest")

    checkout = main_checkout()
    if checkout:
        if git("-C", checkout, "status", "--porcelain"):
            print(f"land: {checkout} has uncommitted changes; main there "
                  "was left behind, run `git merge --ff-only origin/main`")
        else:
            git("-C", checkout, "merge", "--ff-only", head)

    games = sorted({g for p in paths if (g := match(p, GAMES))})
    if games:
        print("land: rebuild and test against main:", "; ".join(games))
    print(f"land: done. Delete the topic when finished: "
          f"git worktree remove {root} && git branch -d {topic}")


if __name__ == "__main__":
    main()
