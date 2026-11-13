// The one place that defines the docs' sections and their order.
//
// A page joins a section through its frontmatter (`section: "Rust"`), and the
// sidebar, breadcrumbs, prev/next links and search all follow this order.
// To add a section, add it here; to add a page, add an .mdx file.
// `icon` is any Phosphor icon name (https://phosphoricons.com).

export const sections = [
  { label: "Start here", blurb: "What BUI is and how to install it", icon: "compass" },
  { label: "Tutorials", blurb: "Build a first app, one language at a time", icon: "graduation-cap" },
  { label: "Python", blurb: "tkinter, Qt syntax and Blender components", icon: "file-py" },
  { label: "Rust", blurb: "HTML, CSS, Tailwind, rsx! and hooks, natively", icon: "file-rs" },
  { label: "JavaScript & React", blurb: "Node.js, Bun, React, Svelte, Vue and standalone apps", icon: "file-js" },
  { label: "Dear ImGui", blurb: "The real Dear ImGui, in Blender's theme", icon: "squares-four" },
  { label: "C & C++", blurb: "The C kit and embedding BUI in your host", icon: "file-c" },
  { label: "Lua", blurb: "The bui-lua host and the bui module", icon: "moon" },
  { label: "D", blurb: "A DUB package with raw C declarations and D wrappers", icon: "code" },
  { label: "Concepts", blurb: "How BUI works under the hood", icon: "lightbulb-filament" },
  { label: "Performance", blurb: "Numbers, and how to measure them yourself", icon: "gauge" },
  { label: "Reference", blurb: "Variables, options, matrices and terms", icon: "books" },
  { label: "Project", blurb: "Roadmap, contributing and credits", icon: "flag-banner" },
] as const;

export type SectionLabel = (typeof sections)[number]["label"];

export const sectionLabels = sections.map((s) => s.label) as unknown as [SectionLabel, ...SectionLabel[]];

/**
 * Languages a page can be about, and the labels <LangTabs> understands.
 * `id` is what gets stored when a reader picks a tab; `label` is what <Tab label="..."> uses.
 */
export const languages = [
  { id: "python", label: "Python" },
  { id: "rust", label: "Rust" },
  { id: "javascript", label: "JavaScript" },
  { id: "c", label: "C" },
  { id: "cpp", label: "C++" },
  { id: "imgui", label: "Dear ImGui" },
  { id: "d", label: "D" },
] as const;

export type LanguageId = (typeof languages)[number]["id"];
export const languageIds = languages.map((l) => l.id) as unknown as [LanguageId, ...LanguageId[]];

/**
 * The header's product-level links. They name areas of the site, not pages: the sidebar
 * lists the pages. `match` decides when a link shows as current: a docs section label, or
 * a page id.
 */
export const topLinks = [
  { label: "Docs", href: "docs/start/introduction/", match: {} },
  { label: "Tutorials", href: "docs/tutorials/first-app-python/", match: { sections: ["Tutorials"] } },
  { label: "Benchmarks", href: "docs/performance/benchmarks/", match: { sections: ["Performance"] } },
  { label: "Roadmap", href: "docs/project/roadmap/", match: { pages: ["project/roadmap"] } },
] as const;

export type TopLink = (typeof topLinks)[number];

/** Which top link is current for a docs page. */
export function currentTopLink(section: string, pageId: string): string | undefined {
  for (const l of topLinks) {
    const m = l.match as { sections?: readonly string[]; pages?: readonly string[] };
    if (m.pages?.includes(pageId)) return l.label;
    if (m.sections?.includes(section)) return l.label;
  }
  return "Docs";
}
