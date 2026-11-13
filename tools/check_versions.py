"""Check that every published package agrees with a release tag before anything is published.

    python tools/check_versions.py v0.2.0

The Python package, the Rust workspace, the Rust crates' path dependencies, the DUB package
and its raw version constants share one version (the tag). The npm packages are versioned on
their own (the native addon can get a patch release the Rust crates don't), so they are only
listed, not compared; the release workflow skips an npm version that is already published.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LUA = ROOT / "bindings" / "lua"
RUST = ROOT / "bindings" / "rust"
D = ROOT / "bindings" / "d"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def rockspecs() -> list[Path]:
    """The LuaRocks package file (blenderui-<version>-1.rockspec): exactly one."""
    return sorted(LUA.glob("blenderui-*.rockspec"))


def npm_packages() -> list[str]:
    """The npm packages, in publish order: the workspaces of bindings/package.json (native first)."""
    return json.loads(read(ROOT / "bindings" / "package.json"))["workspaces"]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    want = sys.argv[1].removeprefix("v")
    found: dict[str, str] = {}

    found["bindings/python/bui/_version.py"] = re.search(
        r'__version__\s*=\s*"([^"]+)"', read(ROOT / "bindings/python/bui/_version.py")).group(1)
    found["bindings/rust/Cargo.toml [workspace.package]"] = re.search(
        r'^version\s*=\s*"([^"]+)"', read(RUST / "Cargo.toml"), re.M).group(1)

    header = read(ROOT / "kit" / "include" / "BUI_kit.h")
    parts = [re.search(rf"#define BUI_KIT_VERSION_{p}\s+(\d+)", header) for p in ("MAJOR", "MINOR", "PATCH")]
    string = re.search(r'#define BUI_KIT_VERSION_STRING\s+"([^"]+)"', header)
    if not all(parts) or not string:
        sys.exit("kit/include/BUI_kit.h: the BUI_KIT_VERSION_* macros are missing or malformed")
    found["kit/include/BUI_kit.h (numbers)"] = ".".join(p.group(1) for p in parts)
    found["kit/include/BUI_kit.h (string)"] = string.group(1)

    rocks = rockspecs()
    if len(rocks) != 1:
        sys.exit(f"bindings/lua: expected one blenderui-<version>-1.rockspec, found {[r.name for r in rocks]}")
    rock = read(rocks[0])
    found[f"bindings/lua/{rocks[0].name} (file name)"] = re.fullmatch(r"blenderui-(.+)-\d+\.rockspec", rocks[0].name).group(1)
    found["bindings/lua rockspec: version"] = re.search(r'^version\s*=\s*"([^"]+)-\d+"', rock, re.M).group(1)
    found["bindings/lua rockspec: source tag"] = re.search(r'tag\s*=\s*"v([^"]+)"', rock).group(1)

    # the DUB package and its hand-written copies of the kit version
    found["bindings/d/dub.json"] = json.loads(read(D / "dub.json"))["version"]
    dkit = read(D / "source" / "bui" / "kit.d")
    dparts = [re.search(rf"enum BUI_KIT_VERSION_{p}\s*=\s*(\d+);", dkit) for p in ("MAJOR", "MINOR", "PATCH")]
    dstring = re.search(r'enum BUI_KIT_VERSION_STRING\s*=\s*"([^"]+)";', dkit)
    if not all(dparts) or not dstring:
        sys.exit("bindings/d/source/bui/kit.d: the BUI_KIT_VERSION_* constants are missing or malformed")
    found["bindings/d/source/bui/kit.d (numbers)"] = ".".join(p.group(1) for p in dparts)
    found["bindings/d/source/bui/kit.d (string)"] = dstring.group(1)

    # the published crates name their siblings with an exact version
    for crate in ("blenderui", "blenderui-macros", "blenderui-imgui", "blenderui-table", "blenderui-vlist", "blenderui-paint"):
        text = read(RUST / crate / "Cargo.toml")
        for dep, ver in re.findall(r'^(blenderui[\w-]*)\s*=\s*\{[^}]*version\s*=\s*"([^"]+)"', text, re.M):
            found[f"bindings/rust/{crate}/Cargo.toml: {dep}"] = ver

    bad = {k: v for k, v in found.items() if v != want}
    for k, v in found.items():
        print(f"{'ok ' if v == want else 'BAD'} {v:8} {k}")
    for pkg in npm_packages():
        meta = json.loads(read(ROOT / "bindings" / pkg / "package.json"))
        print(f"npm {meta['version']:8} {meta['name']} (own version; published only if new)")
    if bad:
        print(f"\nThe tag says {want}, but {len(bad)} place(s) disagree.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
