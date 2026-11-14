//! Builds the C kit (kit + gpu + core) with the project's own CMake files and
//! links it statically. Set `BUI_SOURCE_DIR` to build from another checkout;
//! published crates carry the sources in `csrc/`.

use std::path::PathBuf;

/// One row of features.json (one feature per line): its name and CMake option.
fn feature_row(line: &str) -> Option<(&str, &str)> {
    let field = |key: &str| {
        let at = line.find(&format!("\"{key}\": \""))? + key.len() + 5;
        line[at..].split('"').next()
    };
    Some((field("name")?, field("option")?))
}

fn main() {
    // docs.rs: rustdoc links nothing, so skip the C build (its sandbox may lack
    // the toolchain, and the build takes long)
    if std::env::var_os("DOCS_RS").is_some() {
        return;
    }
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    // BUI_SOURCE_DIR, else the repository around this crate, else the sources
    // vendored into the published crate (`csrc/`, see scripts/vendor_c.py)
    let root = std::env::var("BUI_SOURCE_DIR").map(PathBuf::from).unwrap_or_else(|_| {
        let repo = manifest.join("../../..");
        if repo.join("kit/include/BUI_kit.h").exists() { repo } else { manifest.join("csrc") }
    });
    let root = root.canonicalize().expect("BUI source tree not found (set BUI_SOURCE_DIR)");
    // Windows canonical paths are \\?\C:\...; compilers' /FI and -include can't take them.
    let root = PathBuf::from(root.to_string_lossy().trim_start_matches(r"\\?\").to_string());

    // Text: the portable engine (embedded Inter, GPU Slug glyphs) everywhere;
    // the `gdi-text` feature or BUI_TEXT_GDI=1 selects GDI on Windows.
    let gdi = std::env::var("CARGO_FEATURE_GDI_TEXT").is_ok()
        || std::env::var("BUI_TEXT_GDI").map(|v| v == "1").unwrap_or(false);
    println!("cargo:rerun-if-env-changed=BUI_TEXT_GDI");
    // `imgui`: Dear ImGui on BUI + its C API (C++), built by the same CMake project
    let imgui = std::env::var("CARGO_FEATURE_IMGUI").is_ok();
    let mut config = cmake::Config::new(&root);
    // The optional parts (features.json, the one table): each cargo feature is
    // its CMake option, ON exactly when the feature is.
    let table = std::fs::read_to_string(root.join("features.json")).expect("features.json");
    let manifest_toml = std::fs::read_to_string(manifest.join("Cargo.toml")).expect("Cargo.toml");
    for (name, option) in table.lines().filter_map(feature_row) {
        assert!(
            manifest_toml.lines().any(|l| l.starts_with(name) && l[name.len()..].trim_start().starts_with('=')),
            "features.json has `{name}`: add it to blenderui-sys's (and blenderui's) [features]"
        );
        let on = std::env::var_os(format!("CARGO_FEATURE_{}", name.to_uppercase().replace('-', "_"))).is_some();
        config.define(option, if on { "ON" } else { "OFF" });
    }
    println!("cargo:rerun-if-changed={}", root.join("features.json").display());
    let dst = config
        .define("BUI_TEXT_GDI", if gdi { "ON" } else { "OFF" })
        .define("BUI_SLUG", if std::env::var("CARGO_FEATURE_SLUG").is_ok() { "ON" } else { "OFF" })
        .define("BUI_BUILD_TESTS", "OFF")
        .define("BUI_BUILD_EXAMPLES", "OFF")
        .define("BUI_BUILD_PYTHON_BINDINGS", "OFF")
        .define("BUI_BUILD_UI", "OFF")
        .define("BUI_BUILD_QT_BACKEND", "OFF")
        .define("BUI_IMGUI_BLENDER_WIDGETS", if std::env::var("CARGO_FEATURE_IMGUI_BLENDER_WIDGETS").is_ok() { "ON" } else { "OFF" })
        // Always optimized; on MSVC also the release C runtime Rust links.
        .profile("Release")
        .build_target(if imgui { "bui_cimgui" } else { "bui_kit" })
        .build();

    let build = dst.join("build");
    if imgui {
        for sub in ["", "Release"] {
            println!("cargo:rustc-link-search=native={}", build.join("bindings/imgui").join(sub).display());
        }
        for lib in ["bui_cimgui", "bui_imgui"] {
            println!("cargo:rustc-link-lib=static={lib}");
        }
    }
    for lib in ["kit", "gpu", "core"] {
        for sub in ["", "Release"] {
            println!("cargo:rustc-link-search=native={}", build.join(lib).join(sub).display());
        }
    }
    for lib in ["bui_kit", "bui_gpu", "bui_core"] {
        println!("cargo:rustc-link-lib=static={lib}");
    }
    let target = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("msvc") {
        // the C++ runtime: the kit and core carry Blender C++ sources (string
        // search, expressions, ...), and Dear ImGui with `imgui` (MSVC links its own)
        println!("cargo:rustc-link-lib={}", if target == "macos" { "c++" } else { "stdc++" });
    }
    if target == "windows" {
        for lib in ["user32", "gdi32", "imm32"] {
            println!("cargo:rustc-link-lib={lib}");
        }
    } else {
        // No libX11 / libGL link: the X11 window host (kit_window_x11.c) and the
        // OpenGL renderer (gpu_gl.c) dlopen() them at run time, so programs start
        // (headless) without X11 installed. dlopen itself is in libdl before glibc 2.34.
        if target == "linux" {
            println!("cargo:rustc-link-lib=dl");
        }
        if target == "macos" {
            // the Cocoa window host (kit/intern/kit_window_cocoa.m): static
            // libraries don't carry CMake's framework links to rustc
            for fw in ["Cocoa", "CoreGraphics"] {
                println!("cargo:rustc-link-lib=framework={fw}");
            }
            // video (movie_avf.m) and its sound (device_coreaudio.c). `rustc-link-lib`, not
            // `rustc-link-arg`: only a link-lib reaches the crates that depend on this one (a
            // link-arg stays with this package's own targets, and `blenderui`'s test binaries
            // then fail to link). All four frameworks ship with every macOS the kit supports
            // (11 and newer): so these link strongly here, where the C build (kit/CMakeLists.txt)
            // links them weakly (cargo has no stable weak-framework link-lib).
            if std::env::var_os("CARGO_FEATURE_VIDEO").is_some() {
                for fw in ["AVFoundation", "CoreMedia", "CoreVideo", "AudioToolbox"] {
                    println!("cargo:rustc-link-lib=framework={fw}");
                }
            }
            println!("cargo:rustc-link-lib=objc");
        }
        println!("cargo:rustc-link-lib=m");
        println!("cargo:rustc-link-lib=pthread");
    }
    for dir in ["kit", "gpu", "core", "CMakeLists.txt", "bindings/imgui"] {
        println!("cargo:rerun-if-changed={}", root.join(dir).display());
    }
    println!("cargo:rerun-if-env-changed=BUI_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=BUI_TEXT_STB");
    println!("cargo:include={}", root.join("kit/include").display());
    if imgui {
        println!("cargo:imgui_include={}", root.join("bindings/imgui/c").display());
    }
}
