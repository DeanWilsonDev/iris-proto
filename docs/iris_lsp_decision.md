# iris-lsp — architecture decision

## 0. Scope

An LSP server for `.iris`/`.irisx` files (`tools/iris-lsp/`), built against `libiris`
directly rather than reimplementing any part of the language. Driven by an explicit ask
for Neovim support: diagnostics, completion, goto-definition, and syntax highlighting.
The last of those is split across two mechanisms — a tree-sitter `cpp` injection on the
editor side for everything outside `render{}` (§6), and `iris-lsp`'s own semantic tokens
(§7) for the JSX-flavored bits `cpp`'s grammar can't parse.

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
| Goto-definition | `TagNameAtPosition` finds the `<Name>`/`</Name>` under the cursor; a Core primitive tag (no declaration to jump to) is filtered out, otherwise it's resolved identically to the import case below via `Server::ResolveComponentDeclaration` | `ScanImports` + `ResolveImports`, then a text search for `Name(` in the target file | proxied to `IHostLanguageServerProxy::Definition`, position-translated both ways through `VirtualDocument` |

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

**clangd's own diagnostics are forwarded.** clangd pushes `textDocument/publishDiagnostics`
unprompted, on its own schedule — not as a reply to anything this proxy sent — so a single
blocking request/response exchange can't observe it. `ClangdProxy` owns a background
thread (`ReaderLoop`) for the whole lifetime of the child process: it's the only thing that
ever calls `JsonRpc::ReadMessage(ChildStdout_)`, and it dispatches every message it reads —
a reply to one of our own requests wakes whichever `SendRequest` call is blocked waiting on
it (via a `PendingResults_` map + condition variable); a `publishDiagnostics` notification
is translated into `ProxyDiagnostic`s and handed to `Server`'s registered
`ProxyDiagnosticsCallback`; a server-to-client *request* (clangd occasionally sends one,
e.g. `workspace/configuration`) gets a generic empty-result reply so clangd is never left
waiting on a message iris-lsp doesn't implement.

`Server::HandleClangdDiagnostics` receives the callback (on that background thread, not
the main stdin-reading thread — see the concurrency note below), looks `GeneratedPath` up
to find its owning document, translates every diagnostic's range through
`VirtualDocument::ToSource` (dropping one whose range doesn't map to any source line — it
points at the synthesized `#pragma once`/`#line` prologue, which has no source position by
construction), merges the result with that document's own `Iris::CompileFile` diagnostics,
and republishes — LSP's `publishDiagnostics` replaces a uri's previous set wholesale, so
this naturally supersedes the plain-Iris-only publish `RebuildDocument` already sent right
after the edit, with no explicit merge-vs-replace bookkeeping needed. Hand-verified: a
`.iris` file with `int x = "not an int";` outside its `render{}` block correctly surfaces
clangd's real type-mismatch diagnostic at the right `.iris` source line.

**Concurrency, as a result:** two threads now touch `Server`. `DocumentsMutex_` guards
every access to `Documents_`/`GeneratedPathToUri_`; `OutMutex_` guards every write to the
client connection (inside `Reply`/`ReplyError`/`Notify`) — kept as two separate mutexes
rather than one, specifically so a thread holding `DocumentsMutex_` can still call
`Notify()` without touching a mutex it might already hold. The one real hazard this
required designing around: the main thread must never hold `DocumentsMutex_` while
blocked inside a `Proxy_->Completion()`/`Definition()`/`Start()` call, because those block
waiting on `ClangdProxy`'s own reader thread — and if a `publishDiagnostics` notification
happens to arrive first, that same reader thread would need `DocumentsMutex_` inside
`HandleClangdDiagnostics` before it can loop back around to read the response the main
thread is actually waiting for. `HandleCompletion`/`HandleDefinition`/`RebuildDocument` all
copy what they need out of `Documents_` under a short-lived lock, release it, *then* call
into `Proxy_` — never the other way around. `Proxy_`/`ProxyStarted_` themselves need no
lock at all: every access to them happens from `RebuildDocument`/`HandleCompletion`/
`HandleDefinition`, all of which only ever run on the main thread (`Run()`'s own
single-threaded read loop) — `ClangdProxy`'s reader thread never touches `Proxy_` itself,
only calls the diagnostics callback.

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

Syntax highlighting: no `.iris`-specific tree-sitter grammar exists (writing one from
scratch was the alternative considered and rejected — most of a `.iris` file is already
valid C++23, so a from-scratch grammar would be re-deriving `tree-sitter-cpp` for little
benefit). Instead: register the `cpp` grammar for filetype `iris` (`editors/nvim/
iris-treesitter.lua`) for everything outside `render { }`, and let `iris-lsp`'s own
semantic tokens (§9) cover the JSX-flavored bits `cpp`'s grammar can't parse. Neovim's
built-in LSP client applies semantic tokens automatically for any attached client that
advertises `semanticTokensProvider` — no extra config beyond starting the client itself.

## 7. Semantic tokens for `render{}`

`cpp`'s tree-sitter grammar has no way to parse `<Tag prop="x">` — it's not C++ syntax —
so §6's injection approach leaves the exact region that most needs good highlighting
(the JSX-flavored tags) looking the worst (tree-sitter `ERROR` nodes). `iris-lsp` closes
that gap directly: it already builds a full `ElementNode` tree per `render{}` block for
diagnostics (`RenderBlockParser`), and that tree already carries real source locations
for everything worth highlighting — `textDocument/semanticTokens/full` was a matter of
walking it, not new parsing.

`SemanticTokens.h`/`.cpp` (`CollectRenderBlockSemanticTokens`) recursively walks every
`render{}` block's `ElementNode` (including nested elements reached through a `!{ }`
JSX-transform escape hatch's own `JsxSegments`) and emits one token per:

- **tag name** (`Frame` in `<Frame ...>`) → `type`. `ElementNode::Location` is the `<`
  itself (`RenderBlockParser`'s own convention), so the tag name's span starts exactly one
  column after it — no whitespace is ever allowed between `<` and a tag name.
- **prop name** (`class` in `class="a"`) → `property`. This needed a small core-library
  addition: `Prop` previously only carried `Value.Location` (the value's position, via
  `PropValue::Location`), not the name's own — `Prop::Location` was added
  (`include/Iris/ElementNode.h`, `RenderBlockParser.cpp`) specifically for this.
- **string-literal prop value** (`"a"` in `class="a"`, quotes included) → `string`. A
  `{ }` escape-hatch prop value gets no token at all — it's ordinary host-language code,
  left entirely to `cpp`'s own highlighting.

Only three token types are declared in the legend (`type`, `property`, `string`) — enough
to make the JSX shape read the way JSX reads elsewhere, not a general-purpose C++
semantic highlighter (clangd's own semantic tokens, if forwarded, would be a much larger
follow-up — not attempted here, same "don't merge clangd's own X" boundary diagnostics
forwarding already drew in §4).

Delta-encoding (LSP's wire format: `[deltaLine, deltaStartChar, length, tokenType,
tokenModifiers]` per token, relative to the previous one) happens in
`Server::HandleSemanticTokensFull`, not in the collector — `CollectRenderBlockSemanticTokens`
returns plain absolute `(Line, Column, Length, Type)` structs, sorted ascending, so it
stays independently testable without needing to reason about delta arithmetic.

Hand-verified against the real binary through a real Neovim session running the user's
own config (not just the raw stdio protocol): `vim.lsp.semantic_tokens.get_at_pos`
resolves `type`/`property`/`string` at exactly the right buffer positions once
`iris-lsp` attaches, no additional Neovim configuration needed beyond starting the
client — Neovim auto-enables semantic-token highlighting for any attached client
advertising the capability.

## 8. Explicit non-goals for this pass

- Hover, rename, find-references — none implemented; nothing about the architecture
  blocks adding them later through the same local-vs-proxy split. (Semantic tokens *are*
  implemented — §9.)
- clangd-backed behavior (host-language completion/definition/diagnostics, §4) has no
  automated coverage in `tools/iris-lsp/tests/` -- spawning a real clangd in a unit-test
  suite would trade determinism for coverage of code this repo doesn't own. Verified by
  hand-driving the server over its real stdio protocol instead (initialize → didOpen →
  completion/definition/diagnostics → shutdown) against a small throwaway project.

## 9. Test suite

`tools/iris-lsp/tests/` (a `iris_lsp_lib` static library split out of the `iris_lsp`
executable specifically so the tests can link the real implementation, mirroring how
`iris`/`iris_cc` are already split) covers everything that doesn't need clangd:

- `JsonRpcTests.cpp` -- Content-Length framing round-trips through an in-memory
  `std::tmpfile()`, including back-to-back messages on one stream and EOF handling.
- `VirtualDocumentTests.cpp` -- `IsInsideRenderBlock`, `ImportNameAtLine`, and a
  `ToGenerated`/`ToSource` round trip across a real render-block splice (asserting the
  generated line actually shifted, not just that round-tripping happens to be a no-op).
- `RenderTextHeuristicsTests.cpp` -- `ClassifyRenderCompletion`, `TagNameAtPosition`
  (§3's goto-def fix, including closing tags and picking the right one among several on a
  line), and `FindComponentDeclaration`, all pulled out of `Server.cpp`'s own anonymous
  namespace into `RenderTextHeuristics.h`/`.cpp` specifically so they're testable in
  isolation.
- `ServerTests.cpp` -- end-to-end through `Server::Run`'s real stdio protocol, using
  `std::tmpfile()` for both directions instead of real pipes/processes:
  `Server::DisableProxyForTesting()` (test-only, documented in `Server.h`) stops
  `RebuildDocument` from ever forking a real `clangd`, so these tests are deterministic
  and don't depend on clangd being installed. Covers Iris-level diagnostics,
  render{}-local tag/attribute completion, and both goto-definition paths (`import Name`
  and a `<Name>`/`</Name>` tag usage) landing on the same declaration, plus a Core
  primitive tag correctly reporting "nothing to jump to."
