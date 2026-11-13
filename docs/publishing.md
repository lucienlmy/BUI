# Packaging and publishing (`blenderui` on PyPI)

| | |
|---|---|
| PyPI name | **`blenderui`** (`pip install blenderui`) |
| Import names | `blenderui` (recommended) and `bui` (the same modules), plus the extension `buipy` |
| Build backend | [scikit-build-core](https://scikit-build-core.readthedocs.io) driving the project's CMake (`pyproject.toml`) |
| Version | `bindings/python/bui/_version.py`, the single source (CMake reads it too) |
| Python | 3.11+ (CPython) |
| Console script | `blenderui-tk app.py`, which is the same as `python -m bui.tksyntax app.py` |

> The import name `bui` is kept for existing code. An unrelated project owns
> `bui` on PyPI, so published code should use `import blenderui`. The two
> can't both be installed under the same import name.

## What a wheel contains

```
site-packages/
  buipy.<abi>.pyd|.so     the C kit + raster engine (one extension module)
  bui/                     tksyntax/, qtsyntax.py, components/, host.py ...
  blenderui/               alias package: blenderui.X is bui.X
  blenderui-<v>.dist-info/
```

On Windows the extension is built with MinGW-w64 GCC (UCRT) and statically
linked against the GCC runtime. It depends only on system DLLs and
`python3xx.dll`. A wheel is about 0.4 MB compressed; the sdist is about
1.7 MB (it excludes `reference/` and build trees).

## Build locally

```sh
python -m pip install build
python -m build                    # sdist + wheel in dist/

# Windows with MSYS2 MinGW (the tested toolchain):
set CMAKE_GENERATOR=MinGW Makefiles
set CC=gcc
set CXX=g++
python -m build
```

Test the wheel in a clean environment:

```sh
python -m venv /tmp/bui-venv
/tmp/bui-venv/bin/pip install dist/blenderui-*.whl
/tmp/bui-venv/bin/python -c "import blenderui, blenderui.tksyntax, blenderui.components; print(blenderui.__version__)"
```

The test suites (`bindings/python/tests/`) pass against an installed wheel.
Remove the source tree from `sys.path` and keep `examples/` on it for
`test_components`.

## CI (`.github/workflows/wheels.yml`)

| Job | Does |
|---|---|
| `wheels` | `cibuildwheel` for Linux (manylinux x86_64), Windows (MinGW, x86_64) and macOS (arm64 + x86_64), CPython 3.11–3.14; each wheel is imported as a smoke test |
| `sdist` | `python -m build --sdist` |
| `tests` | CMake build + `ctest` (all C and Python suites) on Windows |
| `publish` | On a `v*` tag: upload everything to PyPI with trusted publishing. Manual run with `testpypi`: upload to TestPyPI. |

## One-time PyPI setup

1. Create the `blenderui` project on PyPI. Register it as a
   [pending trusted publisher](https://docs.pypi.org/trusted-publishers/)
   with this repository, workflow `wheels.yml`, environment `pypi`.
2. Do the same on TestPyPI with environment `testpypi`.
3. In the GitHub repository settings, create the environments `pypi` and
   `testpypi` (optionally with required reviewers).

`pyproject.toml` `[project.urls]` already points at
`github.com/ppmpreetham/BUI`.

No API tokens are stored anywhere.

## Releasing everything at once

One command does the whole release:

```sh
python tools/release.py minor              # or patch, major, X.Y.Z; add --npm patch for the npm packages
```

It checks you are on a clean, up-to-date `main`, bumps every version and pushes that commit, waits
for the `ci` and `wheels` workflows to go green on it, tags and pushes the tag, waits for the
publish, then checks crates.io, npm, LuaRocks and PyPI for the new version. It never tags a red commit.
`--dry-run` shows the plan; if a publish step fails, fix it, use "Re-run failed jobs" on the run
(no new tag needed) and run `python tools/release.py --resume`, which only waits and checks.
GitHub's anonymous API allows 60 requests an hour (enough for one release); set `GITHUB_TOKEN`
for more.

The two steps it runs, if you want them by hand: `tools/bump_version.py` changes every version in one go
(the Python package, the Rust workspace and the sibling dependency versions of every crate, the LuaRocks rockspec
(renamed to the new version), the DUB package and its `kit.d` version constants, the changelog, the lock files, and
with `--npm` the npm packages) and runs `tools/check_versions.py` on the result:

```sh
python tools/bump_version.py minor --commit --push   # 0.2.0 -> 0.3.0, commits only those files, pushes main
# ... wait for CI to go green on that commit ...
python tools/bump_version.py --tag --push            # tags v0.3.0 and pushes the tag: the release starts
```

`minor` can be `major`, `patch` or an exact `X.Y.Z`; add `--npm patch` to move the npm
packages too (they are versioned on their own), and `--dry-run` to see what would change. The
tag step refuses a tag that already exists and a branch that isn't pushed yet. Doing it by hand
is the same thing: `git tag v0.3.0 && git push origin v0.3.0`.

| Workflow | Publishes |
|---|---|
| `wheels.yml` | the Python wheels and sdist to PyPI (trusted publishing, no secret) |
| `release.yml` | the Rust crates to crates.io, `@blender-ui/native` and `@blender-ui/react` to npm, and a GitHub Release |

`release.yml` first checks that the tag matches the Python, Rust and D versions
(`python tools/check_versions.py v0.2.0` runs the same check by hand; the C header's
`BUI_KIT_VERSION_*` macros and the DUB package's copies are part of it) and that
`THIRD_PARTY_NOTICES.md` is up to date
(`python tools/gen_notices.py` regenerates it when a dependency changes), then runs the whole
`ci.yml` suite on every platform, then publishes. A version that is already on a registry is
skipped (crates.io, npm and PyPI alike: `wheels.yml` builds nothing when PyPI already has the
version), so only what is missing runs: a run that failed halfway (say, crates.io timing out on the big
`blenderui-sys` upload) can simply be re-run. The npm packages are versioned on their own; bump
them in their `package.json` when they change.

One-time setup:

- **crates.io:** a repository secret `CARGO_REGISTRY_TOKEN` (Settings > Secrets and variables >
  Actions): a crates.io API token with the scopes `publish-new` and `publish-update`.
- **npm:** no secret. On npmjs.com, open each of `@blender-ui/native` and `@blender-ui/react`
  > Settings > Trusted Publisher > GitHub Actions, and enter the owner `ppmpreetham`, the
  repository `BUI` and the workflow filename `release.yml` (leave the environment empty).
  The workflow publishes with OIDC and npm adds provenance by itself.
- **A new npm package** (a new framework binding, such as `@blender-ui/svelte`) can't have a trusted
  publisher before it exists, and a first `npm publish` from CI has no permission yet (the 404
  "could not be found or you do not have permission"). Publish it once by hand, after
  `@blender-ui/native` is out (it depends on it): `cd bindings/svelte && npm publish --access public`,
  logged in as a member of the `@blender-ui` organization. Then add the trusted publisher (the same
  form as above). The same goes for `@blender-ui/vue` (`cd bindings/vue && npm publish --access public`). Until then `release.yml` skips it with a warning and `tools/release.py` shows it
  as MANUAL, not as a failure.
- **LuaRocks:** a repository secret `LUAROCKS_API_KEY`: on luarocks.org, log in with the account that
  owns the package (the repository owner's name), Settings > API keys > create one. The `luarocks` job uploads
  `bindings/lua/blenderui-<version>-1.rockspec`; the rock itself is built from the tag by whoever runs
  `luarocks install blenderui` (it needs CMake and a C compiler). The first upload creates the package.
- **PyPI:** no secret, see above.

Try it without publishing: Actions > release > Run workflow, enter the tag and leave
"publish" unticked. That runs the checks and the CI suite, then `cargo publish --dry-run` and
`npm publish --dry-run`.

## Release checklist

1. Bump `bindings/python/bui/_version.py` (semantic versioning).
2. Add the release to `CHANGELOG.md`.
3. `ctest --test-dir build` passes locally. Optionally run the workflow by
   hand with `testpypi` and `pip install -i https://test.pypi.org/simple blenderui`.
4. `git tag v0.2.0 && git push --tags`. CI builds, tests and publishes (see above).

## Known gaps before 1.0

- Linux wheels get the native X11 window host: `[tool.cibuildwheel.linux]
  before-all` installs the X11 headers (`libX11-devel` / `libx11-dev`), and
  libX11 itself is loaded at run time, so the wheels still import on servers
  without it. (This used to be a gap; it's fixed.)
- macOS: the native Cocoa window host is new and has been tried by hand on
  three Macs (two Apple Silicon, one Intel) so far. With `BUI_HOST=tk` the wheels use the stdlib Tk window instead, which
  needs `tkinter` (for example from python.org or `brew install python-tk`).
- MSVC builds work (the Rust crate and the CMake build use Visual Studio's
  toolchain), but the published Windows wheels are built with MinGW.
- No `musllinux` or 32-bit wheels yet (skipped in `[tool.cibuildwheel]`).
- The README's images use absolute `raw.githubusercontent.com` URLs, so
  PyPI shows them.

# Publishing the Rust crates (crates.io)

`blenderui` is on [crates.io](https://crates.io/crates/blenderui)
(`cargo add blenderui`). The workspace in `bindings/rust/` has eight crates;
all but the three JavaScript crates are published to crates.io, in
dependency order (a `v*` tag does it, see above), and those three stay in the
repository:

| Crate | What it is | Depends on | crates.io |
|---|---|---|---|
| `blenderui-tailwind` | Tailwind v4 class compiler (std only) | none | Published |
| `blenderui-macros` | `rsx!` / `view!`, `#[component]`, `css!`, `tw!` | `blenderui-tailwind` | Published |
| `blenderui-sys` | FFI over the C kit; builds it with CMake | none (`cmake` at build time) | Published |
| `blenderui-paint` | shared painting types: geometry, colors, `PaintSink`, the text cache | none | Published |
| `blenderui-table` | the data table's core | `blenderui-paint` | Published |
| `blenderui-vlist` | the variable-height list's core | `blenderui-paint` | Published |
| `blenderui` | the framework | the five above | Published |
| `blenderui-imgui` | Dear ImGui on BUI (`blender-widgets` feature for Blender's widgets) | `blenderui-sys` (`imgui` feature) | Published |
| `blenderui-jsview` | The JavaScript view of a window, shared by the Node.js addon and the QuickJS runtime | `blenderui` | `publish = false` |
| `blenderui-node` | The napi-rs addon behind `@blender-ui/native` | `blenderui`, `blenderui-jsview` | `publish = false` (ships on npm) |
| `blenderui-qjs` | `bui-runtime`: BUI with embedded QuickJS, for `bui build` | `blenderui`, `blenderui-jsview` | `publish = false` (ships on npm) |

`blenderui-sys` must carry the C sources, because a published crate can't
reach `../../..`. Vendor them first. They're git-ignored and listed in the
crate's `include`:

```sh
python bindings/rust/scripts/vendor_c.py          # copies ~7 MB into blenderui-sys/csrc/
cd bindings/rust
cargo package --workspace --allow-dirty           # builds each packaged crate in isolation
```

`build.rs` looks for the C sources in three places, in order:
1. `BUI_SOURCE_DIR`, if set;
2. the surrounding repository;
3. `csrc/`, the vendored copy (what a downloaded crate has).

## Release

1. Bump `version` in `bindings/rust/Cargo.toml` (`[workspace.package]`). The
   crates share one version. Then update the `version = "..."` of the path
   dependencies in `blenderui/Cargo.toml`, `blenderui-macros/Cargo.toml` and
   `blenderui-imgui/Cargo.toml`.
2. `cargo login` once with a crates.io API token.
3. Vendor, then publish in order. `cargo publish --workspace` does the
   ordering itself on Cargo 1.90+:

```sh
python bindings/rust/scripts/vendor_c.py
cd bindings/rust
cargo publish --workspace --allow-dirty   # or, one by one:
# cargo publish -p blenderui-tailwind
# cargo publish -p blenderui-macros
# cargo publish -p blenderui-sys --allow-dirty
# cargo publish -p blenderui
```

`cargo publish --workspace` skips the `publish = false` crates. To publish
`blenderui-imgui` too (once it's ready), run `cargo publish -p blenderui-imgui`
after `blenderui-sys`.

`--allow-dirty` is needed because `csrc/` is untracked. Users need CMake and
a C compiler (MSVC, MinGW, gcc or clang) to build `blenderui-sys`.
`bench/` is excluded from the workspace and is never published.
