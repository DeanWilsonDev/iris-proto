# Iris — `import` Codegen / Header-Generation Decision

> **Status:** Closed and implemented. Records how `import Name` (not valid C++23) gets
> turned into something the host compiler can actually resolve `Name` against, closing the
> one gap `docs/iris_next_steps.md` flagged after the preprocessor driver/CLI first landed.

---

## The problem

`import Name` is one of Iris's two real keywords (§1.2), but it isn't valid C++23 syntax, so
it can't survive unchanged into generated output. The driver (`Iris::CompileFile`,
`docs/iris_core_spec.md` §6) needs to turn it into *something* the host compiler accepts —
but what that something is was left genuinely open when the driver first landed: making
`Name` visible to the file that imported it (a struct `NameProps` and a function `Name`)
is an ordinary C++ multi-translation-unit visibility problem, and Iris had no answer for it
yet.

## Why the obvious answer doesn't work

The obvious C++ answer — generate a `.h`/`.cpp` pair per component, the header carrying
forward declarations of `NameProps` and `Name()` — runs straight into a boundary the Stage 1
pivot already committed to and CLAUDE.md restates: **Iris does not parse component
declarations, props structs, or function signatures at all** (`docs/iris_core_spec.md` §2.1:
"component declaration ... props ... Iris does not define grammar for any of these — they're
whatever C++23 already defines"). A forward declaration requires knowing the shape of the
thing being declared. Iris genuinely doesn't have that — a props struct's fields, a
function's parameter list, none of it is ever parsed. Synthesizing a forward declaration
would mean quietly growing a second, informal grammar for exactly the host-language
constructs the pivot deliberately gave up ownership of.

## Decision

Every `.iris`/`.irisx` file compiles to **one self-contained, header-only file** —
conventionally `<original-path>.h` (`Button.iris` → `Button.iris.h`) — rather than a
declaration/definition pair. `import Name` becomes an `#include` of that resolved import's
own generated header, path computed relative to `ProjectRoot` (the same root
`Config.SearchPaths` are already relative to — a consuming build is expected to add
`-I <ProjectRoot>`).

Since nothing is split into "just the declaration," there's nothing to parse or
reconstruct — the entire rewritten source (struct definitions, the component function,
everything, with only `render { }` blocks touched) becomes the header body, wrapped in
`#pragma once`.

**Rejected alternatives:**
- **Real forward-declaration headers** (the originally favored option) — rejected per the
  above: not achievable without Iris parsing struct/function signatures, which §2.1
  explicitly rules out. Revisiting that boundary is a much bigger architectural question
  than a header-generation decision and isn't reopened here.
- **Unity/single-translation-unit build** — don't generate headers at all; require a
  consuming project to concatenate or `#include` every generated `.iris` output into one
  TU so ordinary declaration-before-use resolves it. Rejected as the default because it
  pushes a real requirement onto every consuming project's build setup instead of solving
  it once in the driver, even though it remains a valid thing a project could still do with
  header-only output (headers work under unity builds too, trivially).

**The ODR tradeoff, and who owns it:** a header-only component definition needs `inline` to
stay one-definition-rule-safe once the header is `#include`d by more than one translation
unit. Iris does **not** inject `inline` itself — doing so would mean parsing the function
signature it's committed not to touch. This is a convention the component's author applies
themselves (`inline IrisComponent Button(ButtonProps props) { ... }`), the same way any
other host-language detail already passes through Iris untouched. Documented here rather
than silently assumed.

## Implementation

`src/Iris/Driver.cpp`:
- `ToHeaderPath(SourcePath)` — `SourcePath + ".h"`. Deliberately simple: appending rather
  than replacing the extension keeps `.iris.h` and `.irisx.h` both self-describing without
  a lookup table, and needs no per-target-backend branching.
- Every `import Name` statement is now a full-span **replacement** (not the earlier
  insert-a-comment approach), computed via `ImportStatementEndOffset` — the byte offset one
  past `Name`, found by skipping plain whitespace after the literal `import` keyword. A
  comment between `import` and `Name` (`import /* x */ Button`) would defeat this; a known,
  accepted limitation, the same class as `CppTokenizer.h`'s own documented lexical
  heuristics elsewhere in this codebase.
- `Output` now always starts with `#pragma once` ahead of the leading `#line` directive.
- The `#include` path is computed via `std::filesystem::relative(HeaderPath, ProjectRoot)`,
  rendered with `.generic_string()` for portable forward slashes regardless of host OS.

`include/Iris/Driver.h`'s doc comment was rewritten to describe this header-only model in
full, replacing the earlier "comments the import line out" description.

## Verification

`tests/DriverTests.cpp`'s `TestImportLineBecomesIncludeOfGeneratedHeader` and the full spec
§9 `PartyScreen` end-to-end test both assert the `#include`/`#pragma once` shape. Beyond
that, this is the first time in the project a *multi-file* Iris project was verified to
actually compile: `iris_cc` was run three times against an on-disk fixture
(`StartMenu.iris` importing `Button.iris`/`SettingsPage.iris`, each marked `inline` per the
convention above) to produce three real `.iris.h` files, and a `main.cpp` that only
`#include`s `StartMenu.iris.h` — no manual glue, no hand-written declarations — compiled
successfully with `g++ -std=c++23`.

## What is now unblocked

Per `docs/iris_next_steps.md`'s suggested order, this was the one concrete gap standing
between `iris_cc` and actually compiling a real, multi-file project. With it closed:

1. **Stage 2 walker in `iris-penumbra-backend`** — consuming codegen's output, now
   confirmed reachable via a real multi-file build, not just a single-file library call.
2. **Stage 3 reactive runtime** — already fully spec'd.
3. **Stage 4 (Lustre)**, **Stage 5 (first real consumer)** — as before.

## What remains deliberately deferred

- **Revisiting §2.1's no-parsing boundary** to enable real forward declarations someday —
  explicitly not reopened by this decision; would need its own decision doc if a real
  consumer's build times or link-model needs make header-only definitions genuinely costly.
- **`IrisElementTag::None`'s reconciler contract** (how a walker/backend should treat a
  `<Slot>` callable returning nothing) — orthogonal to this decision, deferred to Stage 3's
  own design pass per `docs/iris_next_steps.md`.
- **A comment between `import` and `Name`** breaking `ImportStatementEndOffset` — no
  example in the spec or this codebase needs this; revisit if a real one does.
