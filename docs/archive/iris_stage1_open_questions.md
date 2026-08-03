# Stage 1 — Open Questions

> **Status:** Closed. `docs/iris_stage1_decision_doc.md` closed the original eight questions
> (§1). `docs/iris_stage1_decision_doc_pt2.md` closed the eight that the pivot itself surfaced
> (§2). Nothing here currently blocks starting Stage 1 implementation. Kept as a historical
> record of what was asked and resolved — see `docs/iris_core_spec.md` for the settled
> language reference, not this doc.

---

## 1. Original questions — closed by the architectural pivot

Per `docs/iris_stage1_decision_doc.md` §11:

| # | Question | Resolution |
| --- | --- | --- |
| 1 | Compiler implementation language | C++23 — consistent with the rest of the Umbra ecosystem |
| 2 | `list<T>` generics vs. element-start ambiguity | Moot — generics live in host-language code outside `render { }`; `<` inside a `render` block is always an element start |
| 3 | Expression grammar depth | Moot — no Iris expression grammar; all expressions are C++23 inside `{ }` escape hatches |
| 4 | Reserved keyword list | `render`, `import`, `key`, `class` — see `docs/iris_core_spec.md` §7 |
| 5 | Optional/default prop values | C++23 struct default member initializers handle this natively |
| 6 | `if`/`else` | Host language handles all control flow |
| 7 | Numeric literal syntax | C++23 literal syntax; Iris never sees numerics |
| 8 | Block comments | C++23 `//` and `/* */` work natively, outside `render { }` |

## 2. Pivot-surfaced questions — closed by `iris_stage1_decision_doc_pt2.md`

Per its summary table (questions 2/3 and 5/6 were collapsed there, since each pair turned out to
be the same underlying question):

| # | Question | Resolution |
| --- | --- | --- |
| 1 | List/loop rendering mechanism | A `{ }` escape hatch may return `std::vector<Component>`; container primitives' children API gains a matching overload. See `docs/iris_core_spec.md` §1.5. |
| 2/3 | `render {` detection robustness / brace balancing | Both solved by one `IHostLanguageTokenizer` abstraction (string/char/comment-aware), pluggable per file extension (`CppTokenizer` for `.iris`, future `NyxTokenizer` for `.irisx`). See §1.3. |
| 4 | Error source mapping | Ships day one. Every `Token` carries a `SourceLocation`; the preprocessor emits `#line` directives into generated output so host-compiler errors point at the original `.iris` file. See §6. |
| 5/6 | `key` struct-field enforcement / missing `key` in loops | Dropped entirely as compile-time checks — `key` uniqueness/presence is a runtime reconciler warning only, the same posture React takes. See §2.3. |
| 7 | Dynamic `class` values | `class={expr}` is valid, symmetric with every other prop including `key`. See §1.4. |
| 8 | Comments in the element tree | Valid between elements inside `render { }`; stripped silently by the preprocessor (free, since the tokenizer already needs comment-awareness for #2/3). See §1.6. |

---

## Deferred — unrelated to Stage 1, still not this doc's problem

Unchanged: Lustre cascade rules, the full Penumbra asset pipeline, the `umbra-engine` primitive
set beyond `Model3d`, `Grid` layout props, event-prop vocabulary extensibility, and implicit
children-forwarding for wrapper components. None of these affect what the Stage 1 preprocessor
needs to accept — see `docs/iris_core_spec.md` §8 for the live list.
