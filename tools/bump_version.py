"""Bump the release version everywhere in one command, then tag it once CI is green.

    python tools/bump_version.py minor                  # 0.2.0 -> 0.3.0 (or patch, major, or 1.0.0)
    python tools/bump_version.py patch --npm patch      # also move the npm packages
    python tools/bump_version.py minor --commit --push  # bump, commit only those files, push main
    python tools/bump_version.py --tag --push           # later, when CI is green: tag and push

A release is two steps, so CI runs on the commit before anything is published:

1. `bump_version.py <new> --commit --push` edits every version (below), runs
   `tools/check_versions.py`, commits just those files and pushes the branch.
2. When CI is green, `bump_version.py --tag --push` tags the current version and pushes the
   tag. The tag starts `release.yml` (crates.io, npm, GitHub Release) and `wheels.yml` (PyPI).
   Both skip what is already published, so a re-run only does what is missing.

Where the version lives (one source of truth per registry, all moved together):

* Python: `bindings/python/bui/_version.py` (the wheel reads it).
* Rust: `[workspace.package]` in `bindings/rust/Cargo.toml`, and the `version = "..."` of every
  sibling path dependency (`blenderui-sys = { path = "...", version = "..." }`).
* D: `bindings/d/dub.json` and the hand-written `BUI_KIT_VERSION_*` constants in
  `bindings/d/source/bui/kit.d` (they mirror `kit/include/BUI_kit.h`).
* npm (only with `--npm`): every already-published package in `bindings/package.json`'s workspaces
  moves to the same version; a package not on npm yet keeps its own first version. All of them
  follow `@blender-ui/native` in their dependency range. npm versions are otherwise independent of the tag.
* LuaRocks: `bindings/lua/blenderui-<version>-1.rockspec` (its version, its source tag and its file
  name move; the old file is removed, as a rename).
* `CHANGELOG.md`: `[Unreleased]` becomes a dated section and a fresh `[Unreleased]` opens.
* The lock files (`Cargo.lock`, `bindings/package-lock.json`) are refreshed (best effort).

`--dry-run` shows what would change and touches nothing.
"""

import argparse
import datetime
import json
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

from check_versions import npm_packages, rockspecs

ROOT = Path(__file__).resolve().parent.parent
RUST = ROOT / "bindings" / "rust"
PY_VERSION = ROOT / "bindings" / "python" / "bui" / "_version.py"
SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


def read(path: Path) -> str:
    with open(path, encoding="utf-8", newline="") as f:  # keep the file's line endings
        return f.read()


def write(path: Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def run(cmd: list[str], cwd: Path = ROOT, check: bool = True) -> subprocess.CompletedProcess:
    exe = shutil.which(cmd[0])  # npm is npm.cmd on Windows, which CreateProcess cannot find by name
    if exe is None:
        return subprocess.CompletedProcess(cmd, 127, "", f"{cmd[0]}: not found")
    return subprocess.run([exe, *cmd[1:]], cwd=cwd, text=True, capture_output=True, check=check)


def current_version() -> str:
    m = re.search(r'__version__\s*=\s*"([^"]+)"', read(PY_VERSION))
    if not m:
        sys.exit(f"cannot read the version from {PY_VERSION}")
    return m.group(1)


def next_version(cur: str, what: str) -> str:
    if SEMVER.match(what):
        return what
    major, minor, patch = (int(x) for x in cur.split("."))
    if what == "major":
        return f"{major + 1}.0.0"
    if what == "minor":
        return f"{major}.{minor + 1}.0"
    if what == "patch":
        return f"{major}.{minor}.{patch + 1}"
    sys.exit(f"'{what}' is not major, minor, patch or X.Y.Z")


def on_npm(name: str) -> bool:
    """Whether the package is already published. A new package keeps its own first version."""
    try:
        with urllib.request.urlopen(f"https://registry.npmjs.org/{name.replace('/', '%2F')}", timeout=20):
            return True
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return False
        raise


def plan_edits(new: str, npm: str | None) -> tuple[dict[Path, str], list[Path]]:
    """Every file's new text, keyed by path, and the files that go away (a rename's old name). Nothing is written here."""
    edits: dict[Path, str] = {}
    gone: list[Path] = []

    def put(path: Path, text: str) -> None:
        if text != read(path):
            edits[path] = text

    put(PY_VERSION, re.sub(r'(__version__\s*=\s*")[^"]+(")', rf"\g<1>{new}\g<2>", read(PY_VERSION)))

    header = ROOT / "kit" / "include" / "BUI_kit.h"
    htext = read(header)
    for part, value in zip(("MAJOR", "MINOR", "PATCH"), new.split(".")):
        htext = re.sub(rf"(#define BUI_KIT_VERSION_{part}\s+)\d+", rf"\g<1>{value}", htext)
    htext = re.sub(r'(#define BUI_KIT_VERSION_STRING\s+")[^"]+(")', rf"\g<1>{new}\g<2>", htext)
    put(header, htext)

    dub = ROOT / "bindings" / "d" / "dub.json"
    text = read(dub)
    text = re.sub(r'("version"\s*:\s*")[^"]+(")', rf"\g<1>{new}\g<2>", text, count=1)
    put(dub, text)

    dkit = ROOT / "bindings" / "d" / "source" / "bui" / "kit.d"
    text = read(dkit)
    for part, value in zip(("MAJOR", "MINOR", "PATCH"), new.split(".")):
        text = re.sub(rf"(enum BUI_KIT_VERSION_{part}\s*=\s*)\d+", rf"\g<1>{value}", text)
    text = re.sub(r'(enum BUI_KIT_VERSION_STRING\s*=\s*")[^"]+(")', rf"\g<1>{new}\g<2>", text)
    put(dkit, text)

    ws = RUST / "Cargo.toml"
    text = read(ws)
    m = re.search(r"\[workspace\.package\](.*?)(?=^\[|\Z)", text, re.S | re.M)
    if not m:
        sys.exit("no [workspace.package] in bindings/rust/Cargo.toml")
    section = re.sub(r'(^version\s*=\s*")[^"]+(")', rf"\g<1>{new}\g<2>", m.group(1), count=1, flags=re.M)
    put(ws, text[: m.start(1)] + section + text[m.end(1):])

    # sibling path dependencies of the published crates: `blenderui-x = { path = "..", version = ".." }`
    for toml in sorted(RUST.glob("*/Cargo.toml")):
        lines = read(toml).splitlines(keepends=True)
        out = []
        for line in lines:
            if re.match(r"\s*blenderui[\w-]*\s*=\s*\{", line) and "path" in line and "version" in line:
                line = re.sub(r'(version\s*=\s*")[^"]+(")', rf"\g<1>{new}\g<2>", line)
            out.append(line)
        put(toml, "".join(out))

    # LuaRocks: the version is in the file name too, so the rockspec is renamed
    (rock,) = rockspecs()
    text = re.sub(r'(^version\s*=\s*")[^"]+(")', rf"\g<1>{new}-1\g<2>", read(rock), count=1, flags=re.M)
    text = re.sub(r'(tag\s*=\s*"v)[^"]+(")', rf"\g<1>{new}\g<2>", text, count=1)
    renamed = rock.with_name(f"blenderui-{new}-1.rockspec")
    if renamed != rock:
        edits[renamed] = text
        gone.append(rock)

    if npm:
        # `npm` is patch, minor, major or X.Y.Z. Each already-published package moves by that kind
        # from its own version (native 0.3.0 -> 0.3.1, svelte 0.1.0 -> 0.1.1); a package that isn't
        # on npm yet keeps its first version. Everyone follows native in their dependency range.
        native_now = json.loads(read(ROOT / "bindings" / "node" / "package.json"))["version"]
        native_new = next_version(native_now, npm)
        for pkg in npm_packages():
            path = ROOT / "bindings" / pkg / "package.json"
            text = read(path)
            meta = json.loads(text)
            if on_npm(meta["name"]):
                text = re.sub(r'("version"\s*:\s*")[^"]+(")', rf"\g<1>{next_version(meta['version'], npm)}\g<2>", text, count=1)
            text = re.sub(r'("@blender-ui/native"\s*:\s*"\^)[^"]+(")', rf"\g<1>{native_new}\g<2>", text)
            put(path, text)

    log = ROOT / "CHANGELOG.md"
    text = read(log)
    if f"## [{new}]" not in text and "## [Unreleased]" in text:
        nl = "\r\n" if "\r\n" in text else "\n"
        today = datetime.date.today().isoformat()
        text = text.replace("## [Unreleased]", f"## [Unreleased]{nl}{nl}## [{new}] - {today}", 1)
        put(log, text)
    return edits, gone


def refresh_locks(npm: bool) -> list[Path]:
    """Best effort: a lock file that can't be refreshed is reported, not fatal."""
    touched = []
    r = run(["cargo", "update", "--workspace", "--offline"], cwd=RUST, check=False)
    if r.returncode != 0:
        r = run(["cargo", "update", "--workspace"], cwd=RUST, check=False)
    if r.returncode == 0:
        touched.append(RUST / "Cargo.lock")
    else:
        print("warning: Cargo.lock was not refreshed (run `cargo update --workspace` in bindings/rust)")
    if npm:
        r = run(["npm", "install", "--package-lock-only", "--ignore-scripts"], cwd=ROOT / "bindings", check=False)
        if r.returncode == 0:
            touched.append(ROOT / "bindings" / "package-lock.json")
        else:
            print("warning: package-lock.json was not refreshed (run `npm install --package-lock-only` in bindings)")
    return touched


def git(*args: str) -> str:
    return run(["git", *args]).stdout.strip()


def tag_and_push(version: str, push: bool) -> None:
    tag = f"v{version}"
    if run([sys.executable, str(ROOT / "tools" / "check_versions.py"), tag], check=False).returncode != 0:
        sys.exit("the versions do not match the tag: run the bump first")
    if git("tag", "--list", tag):
        sys.exit(f"{tag} already exists locally")
    if git("ls-remote", "--tags", "origin", tag):
        sys.exit(f"{tag} already exists on origin")
    if git("status", "--porcelain", "--untracked-files=no"):
        print("warning: the working tree has uncommitted changes; the tag points at the last commit")
    if push:
        ahead = git("rev-list", "--count", "@{u}..HEAD")
        if ahead != "0":
            sys.exit(f"{ahead} commit(s) are not pushed yet: push them and wait for CI before tagging")
    git("tag", tag)
    print(f"tagged {tag} at {git('rev-parse', '--short', 'HEAD')}")
    if push:
        git("push", "origin", tag)
        print(f"pushed {tag}: release.yml (crates.io, npm, GitHub Release) and wheels.yml (PyPI) are starting")
    else:
        print(f"push it with: git push origin {tag}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", nargs="?", help="major, minor, patch or X.Y.Z (leave out with --tag)")
    ap.add_argument("--npm", metavar="V", help="also bump the npm packages: patch, minor, major or X.Y.Z")
    ap.add_argument("--commit", action="store_true", help="commit only the files this changed")
    ap.add_argument("--tag", action="store_true", help="tag the current version (after CI is green)")
    ap.add_argument("--push", action="store_true", help="push the branch (with --commit) or the tag (with --tag)")
    ap.add_argument("--dry-run", action="store_true", help="show what would change and stop")
    ap.add_argument("--no-lock", action="store_true", help="do not refresh Cargo.lock and package-lock.json")
    a = ap.parse_args()

    cur = current_version()
    if a.version is None:
        if not a.tag:
            ap.error("give a version (major, minor, patch, X.Y.Z), or --tag")
        tag_and_push(cur, a.push)
        return 0

    new = next_version(cur, a.version)
    if new == cur:
        sys.exit(f"already at {cur}")
    edits, gone = plan_edits(new, a.npm)
    print(f"{cur} -> {new}" + (f"   (npm packages: {a.npm})" if a.npm else ""))
    for path in edits:
        print("  edit", path.relative_to(ROOT).as_posix())
    for path in gone:
        print("  remove", path.relative_to(ROOT).as_posix())
    if a.dry_run:
        return 0
    for path, text in edits.items():
        write(path, text)
    for path in gone:
        path.unlink()
    touched = list(edits) + gone
    if not a.no_lock:
        touched += refresh_locks(bool(a.npm))

    if run([sys.executable, str(ROOT / "tools" / "check_versions.py"), f"v{new}"], check=False).returncode != 0:
        print(run([sys.executable, str(ROOT / "tools" / "check_versions.py"), f"v{new}"], check=False).stdout)
        print("check_versions.py is not happy: a version is missing from this script's list")
        return 1
    print(f"all versions agree on {new}")

    if a.commit:
        files = [p.relative_to(ROOT).as_posix() for p in touched]
        git("add", "--", *files)
        git("commit", "-m", f"release: v{new}", "--", *files)
        print(f"committed release: v{new}")
        if a.push:
            git("push", "origin", "HEAD")
            print("pushed the branch: wait for CI to go green, then: python tools/bump_version.py --tag --push")
        else:
            print("push it, wait for CI, then: python tools/bump_version.py --tag --push")
    else:
        print("review the diff, then add --commit (or commit by hand)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
