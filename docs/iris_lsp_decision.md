# iris-lsp — architecture decision

## 0. Scope

An LSP server for `.iris`/`.irisx` files (`tools/iris-lsp/`), built against `libiris`
directly rather than reimplementing any part of the language. v1 target, driven by an
explicit ask for Neovim support: diagnostics, completion, goto-definition. Syntax
highlighting is left to the editor side (a tree-sitter `cpp` injection over the whole
buffer is enough for most of a `.iris` file, since most of it *is* C++ — see §6) rather
than built into the server; LSP has no syntax-highlighting method of its own to serve it
from anyway (semantic tokens are the closest fit and are a fast-follow, not v1).

## 1. The one thing that makes this different from an ordinary language server

A `.iris` file is not one language. Per `CLAUDE.md`'s own framing: only the `render { }`
block grammar, `import` resolution, and the `key`/`class` reserved props belong to Iris.
Everything else — component declaration, props structs, `iris::Signal<T>` state, event
handlers, `if`/`for` control flow — is ordinary host-language (C++23 today) code that
passes through the preprocessor untouched. So every position in an open buffer is exactly
one of three things, and each has a different, non-overlapping owner:

1. Inside a `render { }` block → Iris's own business. Not host-language syntax at all —
   proxying this to clangd would be a category error, not just wasted effort.
2. On an `import Name` statement line → also Iris's own business (`ImportResolver`
   already exists for exactly this).
3. Everywhere else → ordinary host-language code Iris deliberately never parses
   (`CppTokenizer`'s own doc comment: "does not tokenize the full C++ grammar ...
   validating is the host C++ compiler's job, never Iris's"). Getting real
   completion/goto-definition here means asking a real language server for that host
   language.

## 2. The proxy, and why it doesn't need its own position-mapping scheme

The naive version of "ask a real C++ server about the host-language regions" needs a
generated C++ view of the buffer, plus a way to translate cursor positions between the
original `.iris` source and that generated view — the same problem Vue/Svelte-style LSPs
solve with virtual documents. The difference here: **Iris's own compiler already builds
that generated view**, as its normal job. `Iris::CompileFile` (`Driver.cpp`) splices each
`render { }` block's codegen'd expression back into the source as `return <expr>;`,
followed by a `#line` directive resyncing line numbers — already exactly the artifact a
proxy needs, produced by the compiler for an unrelated reason (making the generated header
report correct file/line on its *own* compiler errors) years before this tool existed.

`VirtualDocument` (`tools/iris-lsp/VirtualDocument.h`) does nothing but parse those
`#line` directives back out of `Result.Output` into an ordered list of line segments, and
use them to translate `.iris` positions to/from generated-buffer positions. It does not
hook into `Driver`'s internals — it treats `Output` as an opaque string and only reads the
`#line` directives already embedded in it, which is what keeps it decoupled from how
`Driver` is implemented, and portable to a hypothetical Nyx codegen that resyncs the same
way (§5).

Column mapping is exact, not approximate, for the only case that matters (host-language
lines, case 3 above): those lines are copied verbatim from source into the generated
output, so a line-level segment lookup plus an unchanged column is exact. Positions inside
a `render { }` block are never sent through this map at all — they're owned locally
(§1's case 1), so the fact that a spliced `return <expr>;` line has no sensible
column-for-column correspondence to the block it replaced never has to be resolved.

## 3. What's owned locally vs proxied

| Feature | Inside `render{}` | `import Name` line | Everywhere else |
| --- | --- | --- | --- |
| Diagnostics | `Iris::CompileFile`'s own `Diagnostics` list, always | same | same (clangd's own diagnostics are **not** merged in v1 — see §4) |
| Completion | tag names (`CorePrimitiveTagNames()`) or prop names (`PrimitivePropTypeNames()`), by a text heuristic on the cursor's own line (`ClassifyRenderCompletion`) | none (v1 gap) | proxied to `IHostLanguageServerProxy::Completion` |
| Goto-definition | none yet — jumping from a `<Foo>` *usage* to its declaration needs the same machinery as the import case, just triggered from inside the tree instead of a statement scan (v1 gap, not a design gap) | `ScanImports` + `ResolveImports`, then a text search for `Name(` in the target file | proxied to `IHostLanguageServerProxy::Definition`, position-translated both ways through `VirtualDocument` |

`PrimitivePropTypeNames()` moved from being file-local to `Codegen.cpp` into
`CorePrimitives.h`/`.cpp` (alongside the existing `CorePrimitiveTagNames()`) as part of
this work — completion needs the exact same table codegen already type-checks props
against, and duplicating it into iris-lsp would have created a second copy that could
silently drift from the real one.

## 4. `IHostLanguageServerProxy` — the portable seam

```cpp
class IHostLanguageServerProxy {
public:
    virtual bool Start(const std::string& ProjectRoot) = 0;
    virtual void SyncGeneratedDocument(const std::string& GeneratedPath, const std::string& GeneratedText) = 0;
    virtual std::vector<ProxyCompletionItem> Completion(const std::string& GeneratedPath, std::uint32_t Line, std::uint32_t Column) = 0;
    virtual std::optional<ProxyLocation> Definition(const std::string& GeneratedPath, std::uint32_t Line, std::uint32_t Column) = 0;
};
```

This mirrors `Iris::IHostLanguageTokenizer`'s own shape deliberately: "one concrete
implementation per host language, selected by file extension" (`CLAUDE.md`'s Architecture
section) is an existing, working precedent in this codebase — `CppTokenizer` for `.iris`
today, a `NyxTokenizer` deferred until Nyx exists. `CreateHostLanguageServerProxy(Extension)`
does the same selection: `.iris` → `ClangdProxy`, `.irisx` → `nullptr` (not a fallback to
`ClangdProxy` — Nyx is not C++, so that would be a wrong answer dressed up as a working
one). `Server.cpp` only ever calls through this interface; it has no `#include` on
anything clangd-specific. Adding real `.irisx` support later means a new `NyxLspProxy`
class plus one branch in the factory — not a rewrite of `Server.cpp`'s request dispatch,
`VirtualDocument`, or anything render{}-scoped.

`ClangdProxy` (`tools/iris-lsp/ClangdProxy.{h,cpp}`) is the concrete `.iris` implementation:
spawns `clangd --background-index=false` as a child process (POSIX `fork`/`pipe`/`exec` —
this tool targets this project's own Linux dev environment first; a Windows port would
replace this one file, nothing else) and speaks the *same* `JsonRpc` transport class to
it that iris-lsp itself speaks to its own client — clangd is, after all, just another LSP
server. `SyncGeneratedDocument` writes the compiled `Output` to
`<source>.iris.generated.h` next to the source (`.gitignore`d) and forwards it via
`textDocument/didOpen`/`didChange`; `Completion`/`Definition` forward the position-translated
request and map the response back into a backend-agnostic `ProxyCompletionItem`/
`ProxyLocation`.

**Known v1 gap:** clangd's own unprompted diagnostics (`textDocument/publishDiagnostics`
notifications it sends after every `didOpen`/`didChange`) are read and dropped by
`ClangdProxy::SendRequest`'s response loop, not forwarded to the client. iris-lsp's own
diagnostics (from `Iris::CompileFile`) already cover the `render{}`-and-import surface;
losing clangd's host-language diagnostics means a real C++ error in the escape-hatch code
outside a `render{}` block won't show up as a squiggle yet. Forwarding them is a
follow-up, not a redesign — it needs `Server` to own a persistent proxy-notification
listener rather than `ClangdProxy` synchronously blocking on its own request/response
pairs.

## 5. Nyx portability — what's actually decided vs deferred

Decided now, because it costs nothing to decide correctly up front and everything to
retrofit: the extension-keyed factory (§4), and `VirtualDocument`'s `#line`-only coupling
to `Driver` (§2) — neither hardcodes "C++" or "clangd" anywhere outside `ClangdProxy.cpp`
itself. `Server.cpp`'s render{}-local completion (tag/prop names) is already
host-language-agnostic; it only reads `IrisElementTag`/prop tables, which apply
identically to a `.irisx` file once Nyx targets exist for it (`IrisBuildTarget::UmbraEngine`
already threads through `VirtualDocument::Config()`).

Deferred, because there's nothing to design yet: `NyxLspProxy` itself. It depends on (a)
Nyx actually existing as a host language (`.irisx`, Stage 6, explicitly "deferred" per
`CLAUDE.md`'s phasing table) and (b) Nyx having its own LSP to proxy to in the first place.
Guessing its shape now would be exactly the kind of "illustrative forward-reference only"
`IrisElementTag.h`'s own `<Model3d>` comment already warns against doing for
undesigned Stage 6 concepts.

## 6. Editor wiring (Neovim)

No plugin — Neovim's built-in `vim.lsp.start()` needs nothing more than the command and a
root-dir detector (`.iris.json`, mirroring `FindProjectRoot`'s own convention):

```lua
vim.filetype.add({ extension = { iris = "iris", irisx = "iris" } })

vim.api.nvim_create_autocmd("FileType", {
  pattern = "iris",
  callback = function(args)
    vim.lsp.start({
      name = "iris-lsp",
      cmd = { "iris_lsp" }, -- must be on PATH, or use an absolute path to build/iris_lsp
      root_dir = vim.fs.root(args.buf, { ".iris.json" }),
    })
  end,
})
```

Syntax highlighting: no `.iris`-specific tree-sitter grammar exists yet (writing one from
scratch was the alternative considered and rejected for v1 — most of a `.iris` file is
already valid C++23, so a from-scratch grammar would be re-deriving `tree-sitter-cpp` for
little benefit). Cheapest real option, left to the user's own Neovim config rather than
shipped here: an injection query treating the whole buffer as `cpp`. The `render { }`
block's JSX-flavored tags won't highlight quite right under plain C++ rules (a bare `<Tag
prop="x">` doesn't parse as C++), but everything outside it — the actual bulk of most
files — will. Real per-token highlighting for the JSX bits is better served later by LSP
semantic tokens (`textDocument/semanticTokens`) than by hand-writing a second grammar,
since `Server.cpp` already knows exactly which spans are tag names vs prop names vs plain
text from the same `RenderBlockParser` tree it already builds for diagnostics.

## 7. Explicit non-goals for this pass

- Forwarding clangd's own diagnostics (§4).
- Goto-definition from a `<Foo>` *usage* inside `render{}` (only `import Foo` works today —
  §3).
- Hover, rename, find-references, semantic tokens — none implemented; nothing about the
  architecture blocks adding them later through the same local-vs-proxy split.
- A `tools/iris-lsp/tests/` suite — the behavior above was verified by hand-driving the
  server over its real stdio protocol (initialize → didOpen → completion/definition →
  shutdown) against a small throwaway project, not by an automated Cimmerian suite yet.
