// Site-wide settings. Change the URL here (or with SITE_URL at build time).

export const site = {
  name: "BUI",
  title: "BUI: Blender's interface, for your app",
  description:
    "BUI is Blender's user interface as a standalone library. Drive it from tkinter, Qt-style Python, Blender components, Rust, JavaScript and React, Dear ImGui, C or D.",
  repo: "https://github.com/ppmpreetham/BUI",
  /** Base for "Edit this page" links: the docs live in website/src/content/docs. */
  editBase: "https://github.com/ppmpreetham/BUI/edit/main/website/src/content/docs/",
  /** Base for links to files in the repository (use `repoFile()` in content). */
  blobBase: "https://github.com/ppmpreetham/BUI/blob/main/",
  pypi: "https://pypi.org/project/blenderui/",
  npm: "https://www.npmjs.com/package/@blender-ui/react",
  crates: "https://crates.io/crates/blenderui",
  discord: "https://discord.gg/MWXZfdyM3D",
  license: "GPL-3.0-or-later",
} as const;
