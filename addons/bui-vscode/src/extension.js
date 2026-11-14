'use strict';

const { utilities, variants } = require('./tailwind');

// Matches an unterminated class string up to the cursor:
//   class="a b|   className = "   tw!("a b|   (variant-prefixed forms like md:hover:class=" included).
const CLASS_CONTEXT = /(?:(?:^|[\s<>}"])(?:[\w\[\]*@&>=-]+:)*(?:class|className)\s*=\s*|\btw!\s*\(\s*)"([^"]*)$/;
const MACRO_OPEN = /\b(?:rsx|view|tw)!\s*[({[]/;
const LOOKBEHIND = 4000;

/** Returns the partial token being typed inside a class string, or null when not in one. */
function classTokenAt(textBefore) {
  const window = textBefore.slice(-LOOKBEHIND);
  if (!MACRO_OPEN.test(window)) return null;
  const m = CLASS_CONTEXT.exec(window);
  if (!m) return null;
  const typed = m[1];
  return typed.slice(typed.search(/\S*$/));
}

function activate(context) {
  // Required lazily so the pure helpers above stay testable outside the editor.
  const vscode = require('vscode');
  const utilityItems = utilities.map((u) => {
    const item = new vscode.CompletionItem(u, vscode.CompletionItemKind.Value);
    item.detail = 'Tailwind utility';
    return item;
  });
  const variantItems = variants.map((v) => {
    const item = new vscode.CompletionItem(`${v}:`, vscode.CompletionItemKind.Keyword);
    item.detail = 'Tailwind variant';
    item.command = { command: 'editor.action.triggerSuggest', title: '' };
    return item;
  });
  const provider = {
    provideCompletionItems(document, position) {
      const start = Math.max(0, document.offsetAt(position) - LOOKBEHIND);
      const before = document.getText(new vscode.Range(document.positionAt(start), position));
      const token = classTokenAt(before);
      if (token === null) return undefined;
      // Complete only the part after the last variant colon (outside [..] arbitrary variants).
      const afterColon = token.replace(/^(?:(?:\[[^\]]*\]|[^:[])+:)+/, '');
      const range = new vscode.Range(position.translate(0, -afterColon.length), position);
      return [...utilityItems, ...variantItems].map((i) => {
        const c = Object.assign(new vscode.CompletionItem(i.label, i.kind), i);
        c.range = range;
        return c;
      });
    },
  };
  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider({ language: 'rust' }, provider, '"', ' ', ':', '-'),
  );
}

function deactivate() {}

module.exports = { activate, deactivate, classTokenAt };
