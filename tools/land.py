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
4. Pushes the topic to origin/main, a fast-forward only. If main moved in
   the meantime the push is rejected: rerun.
5. Fast-forwards the checkout that has main, when it is clean.

It does not run the game projects; it prints which ones the change calls
for. RECOMP_PYTHON (the oracle tests' venv Python, unresolved) and
DXVK_NATIVE_DIR are passed to every configure when set in the
environment.
"""
import argparse
import os
import re
import subprocess
import sys

# Path prefix -> presets. The first matching prefix wins; anything else
# builds the default preset.
EVERY = ["debug", "x86", "ppc", "a64", "a64-aarch64-qemu", "orbis",
         "xbox360", "horizon"]
RULES = [
    ("tools/land.py", []),
    ("src/runtime/", EVERY),
    ("cmake/", EVERY),
    ("CMakeLists.txt", EVERY),
    ("CMakePresets.json", EVERY),
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
# A string or character literal (raw strings included), a comment, or a
# run of anything else; comments are dropped, literals kept whole.
TOKEN = re.compile(r"""R"([^(\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'"""
                   r"""|(//[^\n]*|/\*.*?\*/)|[^\s"'/]+|/""", re.S)


def code_tokens(text):
    return [m.group(0) for m in TOKEN.finditer(text) if not m.group(2)]


def comment_only(path):
    """A C/C++ file whose change since origin/main is only to comments
    and whitespace (the code tokens are the same)."""
    if not path.endswith(C_FAMILY):
        return False
    old = subprocess.run(["git", "show", f"origin/main:{path}"], text=True,
                         capture_output=True)
    new = subprocess.run(["git", "show", f"HEAD:{path}"], text=True,
                         capture_output=True)
    if old.returncode != 0 or new.returncode != 0:
        return False
    return code_tokens(old.stdout) == code_tokens(new.stdout)


def presets_for(paths):
    picked = set()
    for path in paths:
        if path.endswith(".md"):
            continue
        rule = match(path, RULES)
        picked.update(["debug"] if rule is None else rule)
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
    args = ap.parse_args()

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

    paths = git("diff", "--name-only", "origin/main...HEAD").splitlines()
    if not paths:
        sys.exit("land: nothing to land")
    comments = [p for p in paths if comment_only(p)]
    if comments:
        print("land: comments only, nothing to build:", ", ".join(comments))
    paths = [p for p in paths if p not in comments]
    commits = git("log", "--oneline", "origin/main..HEAD")
    print(f"land: {topic}, onto origin/main:\n{commits}\n")

    presets = presets_for(paths)
    print("land: presets:", ", ".join(presets) or "none (docs and comments only)")
    if args.dry_run:
        return
    for preset in presets:
        print(f"land: {preset}", flush=True)
        if not run_preset(preset, root):
            sys.exit(f"land: {preset} failed; nothing pushed")

    push = subprocess.run(["git", "push", "origin", "HEAD:main"])
    if push.returncode != 0:
        sys.exit("land: push failed (main moved, or the remote could not be "
                 "reached; see git's message); rerun to rebase and retest")

    checkout = main_checkout()
    if checkout:
        if git("-C", checkout, "status", "--porcelain"):
            print(f"land: {checkout} has uncommitted changes; main there "
                  "was left behind, run `git merge --ff-only origin/main`")
        else:
            git("-C", checkout, "merge", "--ff-only", "origin/main")

    games = sorted({g for p in paths if (g := match(p, GAMES))})
    if games:
        print("land: rebuild and test against main:", "; ".join(games))
    print(f"land: done. Delete the topic when finished: "
          f"git worktree remove {root} && git branch -d {topic}")


if __name__ == "__main__":
    main()
