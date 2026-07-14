# Iris — Stage 1 Decision Log

> **Status:** Post-planning. Records decisions made during Stage 1 scoping conversations.
> Intended as a handoff to the agent maintaining `iris_handoff.md`,
> `iris_core_spec.md`, and `iris_stage1_open_questions.md` — update those documents to
> reflect the decisions below.
>
> All eight questions from `iris_stage1_open_questions.md` §2 are closed here. The
> original eight questions from §1 were already closed by `iris_stage1_decision_doc.md`.

______________________________________________________________________

## Summary

| # | Question | Resolution |
|---|---|---|
| 1 | List/loop rendering mechanism | `std::vector<IrisComponent>` escape hatch. See §1. |
| 2 | `render {` detection robustness | `IHostLanguageTokenizer` abstract interface. See §2. |
| 3 | Brace balancing robustness | Resolved by the same tokenizer as question 2. Collapse with §2 in the doc. |
| 4 | Error source mapping | Day one — `Token` carries `SourceLocation`, preprocessor emits `#line` directives. See §4. |
| 5 | `key` struct field enforcement | Dropped — runtime reconciler responsibility only. See §5. |
| 6 | Missing `key` in loop enforcement | Dropped for the same reason as question 5. Collapse with §5 in the doc. |
| 7 | Dynamic `class` values | Valid — `class={expr}` accepted, consistent with every other prop. See §7. |
| 8 | Comments in element tree | Valid — stripped silently by the preprocessor. See §8. |

______________________________________________________________________

## 1. List/loop rendering — `std::vector<IrisComponent>`

**Decision:** A `{ }` escape hatch that needs to produce multiple sibling components
returns `std::vector<IrisComponent>`. `Frame`'s children API gains an overload accepting
`std::vector<IrisComponent>`.

**Example:**

```cpp
render {
    <Frame class="item-list">
        { [&]() -> std::vector<IrisComponent> {
            std::vector<IrisComponent> result;
            for (auto& item : props.items) {
                result.push_back(<Item key={item.id} label={item.name} />);
            }
            return result;
        }() }
    </Frame>
}
```

**Reasoning:** The rejected alternative was a runtime helper `iris::each(container, lambda)`. This was cleaner at the call site but set a precedent of baking C++23-specific
ergonomic helpers into the Iris runtime — helpers that exist purely to compensate for
C++23's verbosity rather than to serve Iris's architecture.

The principle is: **Iris provides the mechanism, the host language provides the
ergonomics.** In C++23 that means verbosity is accepted. When Nyx exists, `.irisx` files
will express this as a clean `props.items.map()` call — the Iris mechanism is identical,
the host language is just less verbose. Baking `iris::each()` into the runtime would be
a C++23 polyfill that Nyx makes redundant, which is the wrong kind of coupling.

**Implication for the runtime:** `IrisComponent` children API must support both a single
child and a `std::vector<IrisComponent>`. This affects the runtime library design, not
the preprocessor.

______________________________________________________________________

## 2. `render {` detection robustness and brace balancing (questions 2 and 3)

**Decision:** The preprocessor uses an `IHostLanguageTokenizer` abstract interface to
tokenise the outer layer of `.iris`/`.irisx` files. The concrete implementation is
selected by file extension at startup. The preprocessor core contains no host-language-
specific lexical rules.

**Interface:**

```cpp
struct SourceLocation {
    std::string_view FilePath;
    std::uint32_t Line;
    std::uint32_t Column;
};

struct Token {
    TokenKind Kind;
    std::string_view Lexeme;
    SourceLocation Location;
};

class IHostLanguageTokenizer {
public:
    virtual Token NextToken() = 0;
    virtual ~IHostLanguageTokenizer() = default;
};

// Concrete implementations
class CppTokenizer : public IHostLanguageTokenizer { ... };  // .iris files
class NyxTokenizer : public IHostLanguageTokenizer { ... };  // .irisx files — when Nyx exists
```

**Reasoning:** A naive text scan for `render {` risks false positives inside string
literals, char literals, comments, or unrelated identifiers. A minimal tokenizer that
understands these constructs eliminates the risk entirely. The same token stream is used
for both `render {` detection and `{ }` escape-hatch brace balancing — these are the same
problem at different depths, solved by the same abstraction.

The tokenizer is hotswappable by design because Iris is not a C++23-only tool. When Nyx
exists, `.irisx` files use `NyxTokenizer` — the preprocessor core is completely untouched.
Making the tokenizer a plugin concern from day one avoids a retrofit when Nyx arrives.

**Questions 2 and 3 are the same question.** The agent updating the open questions doc
should collapse them into a single entry.

______________________________________________________________________

## 3. Error source mapping — day one

**Decision:** Source mapping ships on day one. Every token emitted by
`IHostLanguageTokenizer` carries a `SourceLocation`. The preprocessor emits `#line`
directives into the generated `.cpp` throughout, so that phase 2 C++23 compiler errors
point at the original `.iris` file, not the generated output.

**Reasoning:** Without source mapping, every error inside a `{ }` escape hatch points at
generated code the developer never wrote:

```
// Without source mapping
generated_HealthBar.cpp:47:12: error: no member named 'lable' in 'HealthBarProps'

// With source mapping
HealthBar.iris:8:32: error: no member named 'lable' in 'HealthBarProps'
```

Source position tracking is nearly free when built in from the start — `SourceLocation`
is already on every `Token` from the `IHostLanguageTokenizer` interface (see §2), so the
preprocessor has the information it needs at every point during codegen. Retrofitting this
later requires going back and adding position tracking throughout a preprocessor that was
built without it, which is significantly more expensive.

**Implementation note:** `#line` directives should be emitted at every point in the
generated output where the source line changes. This is a straightforward pass over the
token stream during codegen — no additional infrastructure required beyond what §2 already
establishes.

**Payoff beyond escape hatches:** Iris's own errors — malformed element trees, use of a
gated primitive against the wrong target — also get accurate source locations for free
from the same infrastructure.

______________________________________________________________________

## 4. `key` enforcement and missing `key` in loops (questions 5 and 6)

**Decision:** Compile-time enforcement of `key` rules is dropped entirely. `key` is a
runtime reconciler responsibility.

**On `key` as a struct field name (question 5):** The original rule was that a props
struct cannot declare a field named `key`. Under the preprocessor model, Iris no longer
parses struct bodies — they are host code outside `render { }`. The preprocessor
intercepts `key={...}` at the call site and strips it before codegen regardless of what
the props struct declares. A `key` field in a props struct is therefore permanently
unreachable via Iris, but not a crash.

The rejected alternative was enforcing this via a C++23 `requires` clause emitted into
the generated `.cpp`:

```cpp
template<typename PropsType>
concept NoKeyField = !requires(PropsType p) { p.key; };
```

This was rejected because `key` is only meaningful on elements inside dynamic lists —
requiring every component to prove it has no `key` field is busywork that adds noise
without adding real safety.

**On missing `key` in loops (question 6):** The preprocessor cannot see inside escape
hatches, so it cannot statically detect that a `{ }` block returns a
`std::vector<IrisComponent>` and therefore that its children should carry `key`. This
enforcement is architecturally impossible at compile time under the preprocessor model.

**Runtime behaviour:** The reconciler warns when two elements land at the same tree
position without distinct keys. This is consistent with how React handles the equivalent
case — missing `key` in a list is a runtime warning, not a compile error.

**Questions 5 and 6 are the same question.** The agent updating the open questions doc
should collapse them into a single entry.

______________________________________________________________________

## 5. Dynamic `class` values

**Decision:** `class={expr}` is valid. `class` accepts both string literals and escape
hatches, consistent with every other prop.

```cpp
// Both are valid
<Frame class="button">
<Frame class={isActive ? "button-active" : "button-inactive"}>
```

**Reasoning:** Conditional styling based on runtime state is one of the most common UI
patterns. There is no principled reason to restrict `class` to literals when every other
prop already accepts escape hatches. The preprocessor treats `class={expr}` identically
to any other escape hatch — the contents are emitted verbatim into the generated `.cpp`.

**Interaction with Lustre:** Dynamic class names are invisible to any static analysis
Lustre might perform. This is Lustre's problem to solve, not Iris's — Lustre is a
deferred separate project and that interaction belongs in its own design doc.

______________________________________________________________________

## 6. Comments inside the element tree

**Decision:** `//` and `/* */` comments are valid between elements inside a `render { }`
block. The preprocessor strips them silently before codegen.

```cpp
render {
    <Frame class="start-menu">
        // Renders the settings button
        <Button label="Settings" onPress={[&]() { settingsOpen.set(true); }} />

        /* Conditionally render the settings page */
        { [&]() -> IrisComponent {
            if (settingsOpen.get()) return <SettingsPage />;
            return nullptr;
        }() }
    </Frame>
}
```

**Reasoning:** The `IHostLanguageTokenizer` already understands comment syntax in order
to safely detect `render {` blocks and balance braces. Supporting comments inside render
blocks is therefore free — no additional implementation cost. Being able to comment out
a subtree during development is a basic authoring need and there is no good reason to
prohibit it.
