"""Release BUI in one command: bump, push, wait for CI, tag, watch the publish, verify.

    python tools/release.py minor                 # 0.2.0 -> 0.3.0 (also patch, major, X.Y.Z)
    python tools/release.py patch --npm patch     # and bump the npm packages too
    python tools/release.py --resume              # a bump is already pushed: wait for CI, tag, publish
    python tools/release.py minor --dry-run       # show the plan, change nothing

What it does, in order (it stops at the first problem and never tags a red commit):

1. checks you are on a clean `main` that is not behind `origin`;
2. `tools/bump_version.py <version> --commit --push`: every version, the changelog and the lock
   files, committed on their own and pushed (anything you committed before goes out with it);
3. waits for the `ci` and `wheels` workflows on that commit to finish, and stops if either fails;
4. `tools/bump_version.py --tag --push`: tags `vX.Y.Z` and pushes the tag, which starts
   `release.yml` (crates.io, npm, LuaRocks, GitHub Release) and `wheels.yml` (PyPI);
5. waits for those two runs, then checks every registry for the new version.

If a publish step fails (a registry timeout, a secret), nothing needs re-tagging: fix it and use
"Re-run failed jobs" on the run (the link is printed). Versions already published are skipped.

GitHub's anonymous API allows 60 requests an hour, enough for one release. Set GITHUB_TOKEN (any
token that can read this public repo) for more.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from check_versions import npm_packages

ROOT = Path(__file__).resolve().parent.parent
BUMP = ROOT / "tools" / "bump_version.py"
PY_VERSION = ROOT / "bindings" / "python" / "bui" / "_version.py"

POLL = 30 if (os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")) else 90  # seconds between checks (60 anonymous requests an hour)
CI_TIMEOUT = 60 * 60
PUBLISH_TIMEOUT = 75 * 60


def sh(*cmd: str, check: bool = True) -> str:
    r = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, check=False)
    if check and r.returncode != 0:
        sys.exit(f"`{' '.join(cmd)}` failed:\n{r.stdout}{r.stderr}")
    return r.stdout.strip()


def bump(*args: str) -> int:
    return subprocess.run([sys.executable, str(BUMP), *args], cwd=ROOT).returncode


def current_version() -> str:
    m = re.search(r'__version__\s*=\s*"([^"]+)"', PY_VERSION.read_text(encoding="utf-8"))
    if not m:
        sys.exit("cannot read the version from _version.py")
    return m.group(1)


def repo_slug() -> str:
    url = sh("git", "remote", "get-url", "origin")
    m = re.search(r"github\.com[:/](.+?)(?:\.git)?$", url)
    if not m:
        sys.exit(f"origin is not a GitHub remote: {url}")
    return m.group(1)


def http_json(url: str) -> tuple[int, dict]:
    headers = {"User-Agent": "bui-release-script", "Accept": "application/vnd.github+json"}
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token and "api.github.com" in url:
        headers["Authorization"] = f"Bearer {token}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}
    except (urllib.error.URLError, TimeoutError):
        return 0, {}


def http_status(url: str) -> int:
    """The status of a GET (a page that is not JSON, like a rockspec); 0 when the registry can't be reached."""
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "bui-release-script"}), timeout=30) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except (urllib.error.URLError, TimeoutError):
        return 0


def runs_for(slug: str, sha: str) -> list[dict]:
    code, body = http_json(f"https://api.github.com/repos/{slug}/actions/runs?head_sha={sha}&per_page=50")
    if code == 403 and "rate limit" in str(body.get("message", "")).lower():
        print("  GitHub's rate limit is hit (set GITHUB_TOKEN to raise it); waiting")
        return []
    return body.get("workflow_runs", []) if code == 200 else []


def wait_for(slug: str, sha: str, names: set[str], timeout: int, what: str, branch: str) -> bool:
    """Wait until every workflow in `names` has a finished run of `branch` (a branch or a tag) on `sha`.
    True when all succeeded."""
    print(f"waiting for {what} ({', '.join(sorted(names))}) on {sha[:7]} ...")
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        latest: dict[str, dict] = {}
        for r in runs_for(slug, sha):
            if r["name"] in names and r["head_branch"] == branch:
                if r["name"] not in latest or r["run_number"] > latest[r["name"]]["run_number"]:
                    latest[r["name"]] = r
        state = " | ".join(f"{n}: {latest[n]['status']}/{latest[n]['conclusion'] or '-'}" if n in latest else f"{n}: not started"
                           for n in sorted(names))
        if state != last:
            print(f"  {state}")
            last = state
        if len(latest) == len(names) and all(r["status"] == "completed" for r in latest.values()):
            bad = [r for r in latest.values() if r["conclusion"] != "success"]
            for r in bad:
                print(f"  FAILED {r['name']}: {r['html_url']}")
            return not bad
        time.sleep(POLL)
    print(f"  gave up after {timeout // 60} minutes")
    return False


def crates() -> list[str]:
    """The crates release.yml publishes (read from it, so there is one list)."""
    text = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
    m = re.search(r"for crate in ([^;]+); do", text)
    return m.group(1).split() if m else []


def registry_status(version: str) -> dict[str, bool | None]:
    """True: published. False: missing. None: a new npm package, whose first publish is by hand."""
    out: dict[str, bool | None] = {}
    for c in crates():
        out[f"crates.io  {c}"] = http_json(f"https://crates.io/api/v1/crates/{c}/{version}")[0] == 200
    for pkg in npm_packages():
        meta = json.loads((ROOT / "bindings" / pkg / "package.json").read_text(encoding="utf-8"))
        name = meta["name"].replace("/", "%2F")
        label = f"npm        {meta['name']}@{meta['version']}"
        if http_json(f"https://registry.npmjs.org/{name}")[0] == 404:
            out[label] = None  # not on npm at all: trusted publishing needs the package to exist first
        else:
            out[label] = http_json(f"https://registry.npmjs.org/{name}/{meta['version']}")[0] == 200
    # LuaRocks serves a user's rockspecs at /manifests/<user>/; the user is the repository's owner
    owner = repo_slug().split("/")[0].lower()
    out[f"LuaRocks   blenderui {version}-1"] = http_status(f"https://luarocks.org/manifests/{owner}/blenderui-{version}-1.rockspec") == 200
    out[f"PyPI       blenderui {version}"] = http_json(f"https://pypi.org/pypi/blenderui/{version}/json")[0] == 200
    return out


def verify(version: str) -> bool:
    print("checking the registries ...")
    deadline = time.time() + 10 * 60  # indexes lag a little behind a successful publish
    while True:
        status = registry_status(version)
        if all(v is not False for v in status.values()) or time.time() > deadline:
            break
        time.sleep(POLL)
    for k, ok in status.items():
        print(f"  {'ok     ' if ok else 'MANUAL ' if ok is None else 'MISSING'} {k}")
    if None in status.values():
        print("  MANUAL = a new npm package: publish it once by hand, then add its trusted publisher "
              "(docs/publishing.md, 'A new npm package')")
    return all(v is not False for v in status.values())


def preflight(allow_dirty: bool) -> None:
    if sh("git", "rev-parse", "--abbrev-ref", "HEAD") != "main":
        sys.exit("release from the main branch")
    if not allow_dirty and sh("git", "status", "--porcelain", "--untracked-files=no"):
        sys.exit("you have uncommitted changes to tracked files: commit or stash them (or --allow-dirty)")
    sh("git", "fetch", "origin")
    behind = sh("git", "rev-list", "--count", "HEAD..origin/main")
    if behind != "0":
        sys.exit(f"main is {behind} commit(s) behind origin: git pull first")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", nargs="?", help="major, minor, patch or X.Y.Z")
    ap.add_argument("--npm", metavar="V", help="also bump the npm packages: patch, minor, major or X.Y.Z")
    ap.add_argument("--resume", action="store_true", help="the bump is already committed: wait for CI, tag, publish")
    ap.add_argument("--dry-run", action="store_true", help="show the plan and stop")
    ap.add_argument("--yes", action="store_true", help="do not ask before starting")
    ap.add_argument("--allow-dirty", action="store_true")
    a = ap.parse_args()
    if not a.version and not a.resume:
        ap.error("give a version (major, minor, patch, X.Y.Z) or --resume")

    slug = repo_slug()
    preflight(a.allow_dirty)

    if a.resume:
        version = current_version()
        if a.dry_run:
            print(f"would wait for CI on HEAD, then tag v{version}")
            return 0
    else:
        extra = ["--npm", a.npm] if a.npm else []
        if bump(a.version, *extra, "--dry-run") != 0:
            return 1
        if a.dry_run:
            return 0
        if not a.yes and input("release this? [y/N] ").strip().lower() != "y":
            print("cancelled; nothing changed")
            return 1
        if bump(a.version, *extra, "--commit", "--push") != 0:
            return 1
        version = current_version()

    sha = sh("git", "rev-parse", "HEAD")
    if sh("git", "rev-list", "--count", "@{u}..HEAD") != "0":
        sh("git", "push", "origin", "HEAD")
    if not wait_for(slug, sha, {"ci", "wheels"}, CI_TIMEOUT, "CI", branch="main"):
        print(f"\nCI is not green, so v{version} was NOT tagged. Fix it, push, then: python tools/release.py --resume")
        return 1

    tag = f"v{version}"
    if sh("git", "ls-remote", "--tags", "origin", tag):
        print(f"{tag} is already on origin; not tagging again")
    elif bump("--tag", "--push") != 0:
        return 1
    print(f"\n{tag} is tagged. Watching the publish ...")
    ok = wait_for(slug, sha, {"release", "wheels"}, PUBLISH_TIMEOUT, "the publish", branch=tag)
    done = verify(version)
    if ok and done:
        print(f"\nv{version} is published everywhere. https://github.com/{slug}/releases/tag/v{version}")
        return 0
    print(f"\nNot everything is published. Open https://github.com/{slug}/actions , use \"Re-run failed jobs\" on the "
          f"failed run (no new tag needed), then run: python tools/release.py --resume   (it only checks and waits)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
