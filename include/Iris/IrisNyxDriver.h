#pragma once

#include "Iris/ComponentInstance.h"
#include "Iris/ComponentReloadTier.h"
#include "Iris/IrisConfig.h"
#include "Iris/IrisIrDocument.h"
#include "Iris/IrisIrRuntime.h"
#include "Iris/IrisNyxEvaluator.h"
#include "Iris/NyxComponentBridge.h"

#include "host/nyx-runtime.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Iris {

// The mount/reload driver docs/next-steps.md's "Chaos runtime" entry named as the last
// missing piece: nothing before this ties `WalkIrisIrDocument`/`ConvertIrElement`
// (IrisIrRuntime.h), `MakeNyxEvaluator` (IrisNyxEvaluator.h), `.irisx` -> `.iris.ir`
// compilation (`Driver.h`'s `CompileFile`), and `iris::MountComponentInstance`
// (ComponentInstance.h) into something that actually loads a `.irisx` file from disk and
// mounts it. `IrisNyxDriver` is that glue -- still backend-agnostic (it produces
// `Iris::Component` trees, never touches `Umbra::IWidget` construction itself, same
// boundary every other file in this repo keeps).
//
// One `IrisNyxDriver` is meant to live for a whole application run: it owns the single
// `nyx::host::NyxRuntime` every mounted component (root or cross-file child) evaluates
// against, and caches both the compiled `IrisIrDocument` and the whole-file `NyxScope`
// (`IrisNyxEvaluator.h`'s `ReconstructNyxSource`/`CreateScope`) per resolved file path --
// re-parsing either on every component invocation (e.g. every item in a list) would be
// wasteful, and would also violate `NyxScope`'s own documented "one scope, evaluated
// against repeatedly" contract for module-level state (top-level `nyx_source` outside
// any one component's own body).

// What `ComponentInstance::DriverState` (`shared_ptr<void>`) always actually holds for any
// instance this driver mounted -- the scope `MakeNyxEvaluator` evaluates that instance's own
// render tree against, kept alive for as long as the instance is since a `<Slot>` callable's
// closures hold a raw reference into it (`MakeNyxEvaluator`'s own doc comment). `ClassInstance`
// is set only for a Model 2 (class-based) component -- the live `NyxObject` `Instantiate`
// produced, needed again at reload time for `ReconcileInstanceFields`; unset (`nullopt`) for an
// ordinary Model 1 free-function component, whose only per-invocation state is `RenderScope`
// itself.
struct NyxDriverState {
    nyx::host::NyxRuntime::NyxScope    RenderScope;
    std::optional<nyx::runtime::Value> ClassInstance;
};

// What `ReloadRoot` reports back: the freshly re-rendered tree, plus which tier the *entry*
// component's own reload turned out to be (`ComponentReloadTier::Unchanged`/
// `SignalLayoutChanged`) -- reported independently here rather than borrowed from
// `ComponentInstance::EndReloadReplay`'s own IRIS_SIGNAL-counting logic, which doesn't apply to
// interpreted `@signal`/`Environment`-backed state at all (`nyx-scripting-language/
// decision-log.md` §9.2's own item 4 guidance). A caller with no previous match at all (a fresh
// mount, the tier-3-equivalent case) still gets a `Tier` back -- `SignalLayoutChanged`, since
// nothing was preserved -- there being no third `ComponentReloadTier` enumerator by this
// codebase's own existing convention (`ComponentReloadTier.h`'s own doc comment: tier 3 needs
// no code here, it's just whatever the downstream reconciler's tag/key-mismatch fallback does).
struct IrisNyxReloadResult {
    Iris::Component            Root;
    iris::ComponentReloadTier  Tier;
};

class IrisNyxDriver {
public:
    // `Config`/`ProjectRoot` are exactly `Driver.h`'s `CompileFile` parameters -- this
    // class delegates every `.irisx` -> IR compilation to that existing pipeline rather
    // than re-implementing RenderBlockParser/BuildIrisIr/ImportResolver wiring itself.
    IrisNyxDriver(IrisConfig Config, std::string ProjectRoot);

    // Registers `Runtime()` `@signal` decorator support up front (`RegisterSignalDecorator`,
    // NyxSignalDecorator.h) and the `<Slot>` JSX-transform marker (`ChaosSlotMarker`,
    // IrisNyxEvaluator.h) -- both must be live before `MountRoot`/`InvokeComponent` ever
    // evaluates anything. Exposed so a caller can register additional host types/functions
    // (`RegisterType`/`RegisterFunction`) before the first `MountRoot` call -- this class
    // has no opinion on what a `.irisx` application's own Nyx-side script needs beyond
    // what Iris itself requires.
    nyx::host::NyxRuntime& Runtime();

    // Registers a named `<Native>` widget builder (`NyxEvaluator::EvaluateNative`,
    // IrisIrRuntime.h / `NativeBuilderLookup`, IrisNyxEvaluator.h) -- called by the host
    // application before the first `MountRoot`/`ReloadRoot` call that mounts a `.irisx` file
    // using `<Native build={"Name"} />`, same "register additional things up front" convention
    // `Runtime()`'s own doc comment already documents for host types/functions. `Factory` is
    // called lazily, exactly once per `Build()` call on the resulting `IrisNativeBuilder` --
    // never eagerly, and never by this driver itself.
    void RegisterNativeBuilder(std::string Name, std::function<std::unique_ptr<Umbra::IWidget>()> Factory);

    // Every error accumulated across every `MountRoot`/`ReloadRoot`/`InvokeComponent` call so
    // far -- file-load/import-resolution failures (reported with a default-constructed
    // `IrSourceLocation`, since they have no single node to point at) and ordinary
    // `ConvertIrElement`/`WalkIrisIrDocument` conversion errors alike (IrisIrRuntime.h's
    // own `IrisIrRuntimeError`). Never cleared automatically -- a caller doing several
    // `MountRoot`/`ReloadRoot` calls that wants only the latest call's own errors should
    // check `Errors().size()` before and after.
    const std::vector<IrisIrRuntimeError>& Errors() const { return Errors_; }

    // Loads (compiling via `Driver.h`'s `CompileFile` if not already cached),
    // mounts, and returns `EntryFunctionName`'s render output from
    // `EntryResolvedPath` -- the application's own root component, the one invocation
    // `Codegen.h`-generated code never produces itself (`ComponentInstance.h`'s own
    // `iris::Mount` doc comment). `InitialProps` is this root call's own argument list --
    // typically empty (`{}`) for a root component with no props, matching how a real
    // application's top-level `<App />` is invoked with none.
    //
    // The returned `Component` is exactly what `iris::ResolveSlots`/`Reconciler.h`/
    // `iris::Tick()` already consume unchanged for the compiled `.iris` path -- this
    // function's job stops at producing that tree; building real `Umbra::IWidget`s from
    // it (a `MountFn`, SlotRuntime.h) and driving `iris::Tick()` every frame is the
    // consuming application's own job, same boundary `iris::ResolveSlots` itself already
    // draws.
    Iris::Component MountRoot(const std::string& EntryResolvedPath, const std::string& EntryFunctionName,
                               std::vector<nyx::runtime::Value> InitialArgs = {});

    // The hot-reload counterpart of `MountRoot` (nyx-scripting-language/decision-log.md §9.2,
    // docs/archive/iris_nyx_slot_loop_and_reload_gap_resolved.md §2): re-reads and recompiles
    // `EntryResolvedPath` from disk (bypassing the document cache `MountRoot`/a prior
    // `ReloadRoot` call left behind, since the whole point of calling this is that the file on
    // disk changed), and re-renders `EntryFunctionName` against `PreviousRoot` -- the `Component`
    // a prior `MountRoot`/`ReloadRoot` call for this same entry point returned. For a free
    // function this reuses the live `Interpreter`/`Environment` via `Runtime_.ReInvokeComponent`,
    // preserving every `@signal` binding by name; for a class-based component (Model 2) this
    // reuses `PatchClass`/`ReconcileInstanceFields` against the same live instance -- either way,
    // `PreviousRoot.Instance` is reused in place (`Result.Instance = PreviousRoot.Instance`), not
    // replaced, so anything else already holding that `shared_ptr<ComponentInstance>` (a
    // `SlotState`'s own previous-tree bookkeeping, an app's `ReloadTarget`) keeps seeing the same
    // identity. Falls back to an ordinary fresh mount (this codebase's existing tier-3
    // convention: no special code path, just whatever `MountRoot` already does) when
    // `PreviousRoot` has no reusable `Instance`/`DriverState` at all.
    //
    // Same "driver only produces the `Component` tree" boundary `MountRoot` already has --
    // applying the result to real widgets is the caller's own job, via its own
    // `iris::ReloadTarget::Reconcile` (`ReloadTarget.h`, already built), not this driver's.
    //
    // **Scope boundary:** a *statically*-nested component invocation -- a direct, unconditional
    // child of a Core primitive somewhere in `EntryFunctionName`'s own render output (a `<Header
    // />` that's always present, not behind a `<Slot>`) -- now reloads too, matched against its
    // own prior render via `Component::InvocationTag` (Component.h) and `InvokeComponent`'s own
    // nested-invocation cursor (IrisNyxDriver.cpp's `CollectNestedInvocations`), propagating to
    // any nesting depth. What still always mounts fresh: any invocation reached only through a
    // `<Slot>`'s dynamically-produced output (a `.Map()`-rendered list, a conditional) -- a
    // `<Slot>`'s own current output is never stored in `Component::Children` at all, only inside
    // the live-widget-layer `SlotState` (SlotRuntime.h) this backend-agnostic driver deliberately
    // never touches, so such an invocation is structurally invisible to the collector above, not
    // just unmatched. Closing that would mean either breaking the backend-agnostic boundary or
    // giving `SlotState` its own way to report "here's what I rendered last time" back to a
    // driver observing from outside the widget layer -- real, undesigned scope, not attempted
    // here; see this file's own doc comment on `InvokeComponent`.
    IrisNyxReloadResult ReloadRoot(const std::string& EntryResolvedPath, const std::string& EntryFunctionName,
                                    std::vector<nyx::runtime::Value> InitialArgs, const Iris::Component& PreviousRoot);

private:
    // Returns the cached `IrisIrDocument` for `ResolvedPath`, compiling+parsing it via
    // `Driver.h`'s `CompileFile` + `ParseIrisIrDocument` on first use. `nullptr` on
    // failure (file couldn't be read, or `CompileFile`/`ParseIrisIrDocument` reported
    // diagnostics) -- every failure is also appended to `Errors_` before returning, so a
    // caller only needs to check the return value, not a separate out-parameter.
    const IrisIrDocument* LoadDocument(const std::string& ResolvedPath);

    // Drops `ResolvedPath`'s cached `IrisIrDocument` (not its `FileScope` -- see `ReloadRoot`'s
    // own doc comment on why the two caches have different reload-time lifetimes) so the next
    // `LoadDocument` call re-reads and recompiles the file from disk instead of reusing a
    // possibly-stale parse. Called by the reload path only -- `MountRoot`'s own first-mount path
    // has no reason to ever invalidate a document it just built.
    void InvalidateDocument(const std::string& ResolvedPath);

    // Returns the cached whole-file `NyxScope` for `ResolvedPath` (`ReconstructNyxSource`
    // + `Runtime_.CreateScope`), building it on first use. `Document` must be the same
    // one `LoadDocument(ResolvedPath)` already returned -- passed in rather than
    // re-looked-up since every caller already has it. Deliberately never invalidated by a
    // reload -- `Runtime_.ReInvokeComponent`/`PatchClass` patch this same live `Interpreter` in
    // place; rebuilding it would drop every other still-running file-level declaration this
    // reload has nothing to do with.
    nyx::host::NyxRuntime::NyxScope& GetFileScope(const std::string& ResolvedPath, const IrisIrDocument& Document);

    // The recursive core `MountRoot`/`ReloadRoot` and a component invocation's own
    // `ChildComponentInvoker` (built inside this function, passed to `MakeNyxEvaluator`) all
    // call. `Previous` is `nullptr` for an ordinary mount (`MountRoot`, or a nested child
    // invocation this call's own `ChildInvoker` couldn't match against `Previous->Children` --
    // see `CollectNestedInvocations`, IrisNyxDriver.cpp); non-null for `ReloadRoot`'s own
    // entry-point call, and for any nested statically-nested child invocation this call's own
    // `ChildInvoker` *did* match (`Component::InvocationTag`-based, tag+position only -- see
    // this class's own `ReloadRoot` doc comment on the `<Slot>`-mediated case this still doesn't
    // reach) -- either way, names the prior render of this exact same invocation to reload
    // against. Building `ChildInvoker`'s own matching cursor from `Previous->Children` below is
    // what makes this recursive: a match found here becomes the very next nesting level's own
    // `Previous`, so a statically-nested invocation's *own* nested invocations get the same
    // treatment automatically.
    //
    // Mount path (`Previous == nullptr`, or `Previous` has no reusable
    // `Instance`/`DriverState`): loads `ResolvedPath`'s document, invokes `FunctionName` against
    // its file scope (`Runtime_.InvokeComponent`), builds a `NyxEvaluator` bound to that
    // invocation's own scope, and converts its sole `render_block` root -- all wrapped in
    // `iris::MountComponentInstance` so `@signal` locals inside `FunctionName`'s body register
    // against a real, heap-owned `ComponentInstance` (NyxSignalDecorator.h: a `@signal` used with
    // no ambient `ComponentInstance` is a silent no-op, not an error -- exactly the mistake this
    // function exists to avoid making). The per-invocation `NyxDriverState` this produces is
    // heap-allocated and stashed on that same `ComponentInstance`
    // (`ComponentInstance::DriverState`) so it outlives this function call -- every
    // `NyxEvaluator` closure a `<Slot>` callable captured (`MakeNyxEvaluator`'s own doc comment:
    // those closures hold a raw reference into whatever `NyxScope` was passed in) still has
    // something valid to reference when `iris::Tick()` re-invokes it long after this call
    // returns.
    //
    // Reload path (`Previous` names a reusable prior invocation): invalidates and re-loads
    // `ResolvedPath`'s document, recovers the prior `NyxDriverState` from
    // `Previous->Instance->DriverState`, and re-renders via `Runtime_.ReInvokeComponent`
    // (free-function) or `PatchClass`+`ReconcileInstanceFields` (class-based, Phase C) against
    // the *same* `ComponentInstance` (`Result.Instance = Previous->Instance` -- never
    // `MountComponentInstance`/`iris::ReloadComponentInstance`, both of which are the wrong
    // mechanism here per decision-log.md §9.2's own item 4: they reconcile `IRIS_SIGNAL` storage
    // by declaration order, but interpreted `@signal` state is `Environment`-backed and already
    // reconciled by name inside `ReInvokeComponent`/`ReconcileInstanceFields` themselves). Sets
    // `*OutTier` (when non-null) to the tier that reload turned out to be, derived independently
    // by comparing the prior and new scope's own `@signal`-backed bindings -- not borrowed from
    // `ComponentInstance::EndReloadReplay`'s counting logic, which doesn't apply to this state
    // shape at all.
    //
    // Resolving `<Tag>` to a target file is purely `CallerPath`'s own already-resolved
    // `Document.Imports` list (`IrImportNode::Name`/`ResolvedPath`, populated at IR-build
    // time by `Driver.h`'s `ResolveImports` call) -- exactly the "ImportResolver's job"
    // `IrisNyxEvaluator.h`'s own `ChildComponentInvoker` doc comment named as deliberately
    // left to a driver.
    Iris::Component InvokeComponent(const std::string& ResolvedPath, const std::string& FunctionName,
                                     std::vector<nyx::runtime::Value> Args, const Iris::Component* Previous,
                                     iris::ComponentReloadTier* OutTier);

    // `InvokeComponent`'s Model 2 (class-based) counterpart -- called instead of the
    // free-function path when `FunctionName` names a class in `FileScope.interpreter->
    // Registry().classes` rather than a function (`InvokeComponent`'s own model-detection,
    // nyx-scripting-language/decision-log.md §9.2). Mount (`Previous` unusable):
    // `Interpreter::Instantiate(ClassName, Args)` runs field defaults then the constructor
    // (decision-log.md §5.16, e.g. `this.props = props`); the render scope is then built by
    // hand -- `Interpreter` exposes no "call this instance method, capture its environment"
    // primitive (only `TryCallInstanceMethod`, which returns a `Value`, no environment) -- as a
    // fresh child `Environment` of the file scope with the `Render` method's own declared first
    // parameter name (read from the real `ClassDecl`/`MethodDecl` AST, not assumed to be
    // `"props"`) bound to `Args[0]`, and `EvalContext::thisObject` set to the instantiated
    // `NyxObject` -- exactly the mechanism `host/nyx-runtime.hpp`'s own `NyxScope` doc comment
    // describes for implicit `this.field` reads. **Named simplification:** this does not
    // execute any plain Nyx statement written before `render{}` inside `Render`'s own body (no
    // public environment-capturing method-call primitive exists to run them through) -- every
    // documented Model 2 example has nothing there beyond the parameter itself, so this is a
    // reasonable scope boundary, not a silent correctness gap for anything currently specified.
    //
    // Reload (`Previous` names a reusable prior class-based invocation): `Runtime_.PatchClass` +
    // `Interpreter::ReconcileInstanceFields` against the *same* live `NyxObject` (safe to always
    // call together per decision-log.md §9.1's own note: an already-compatible field is left
    // completely untouched), tier derived by comparing the instance's own field name/`ValueKind`
    // set before and after -- not borrowed from `ComponentInstance::EndReloadReplay`'s counting
    // logic, which has nothing to do with this state shape. The render scope is then rebuilt
    // fresh against the same, now-reconciled instance with the new `Render` argument.
    Iris::Component InvokeClassComponent(const IrisIrDocument& Document, const IrRenderBlockNode& Block,
                                          nyx::host::NyxRuntime::NyxScope& FileScope, const std::string& ClassName,
                                          std::vector<nyx::runtime::Value> Args, const Iris::Component* Previous,
                                          iris::ComponentReloadTier* OutTier, const ChildComponentInvoker& ChildInvoker);

    // The `ChildComponentInvoker` body `InvokeComponent` registers with `MakeNyxEvaluator`:
    // resolves `Tag` against `CallerResolvedPath`'s own already-resolved `Document.Imports`
    // (`IrImportNode::Name` == `Tag`) to find which file it refers to, then re-enters
    // `InvokeComponent` for that file/function -- the only two things a cross-file component
    // invocation actually needs beyond what `InvokeComponent` already does for a same-file one.
    // `Previous` is whatever `InvokeComponent`'s own nested-invocation cursor matched at this
    // position (nullptr for an unmatched or non-reload call -- an ordinary fresh mount, exactly
    // as before this parameter existed) -- forwarded straight through as `InvokeComponent`'s own
    // reload parameter, so a matched statically-nested cross-file child reloads too. An
    // unimported tag is reported into `Errors_` and mounts nothing, rather than falling back to
    // guessing a same-file function of that name -- every documented `.chaos`/`.irisx` example
    // puts one component per file (chaos-ui-authoring.md §27.1), so an invocation naming a tag
    // with no matching `import` is treated as an authoring mistake, not a same-file call.
    Iris::Component InvokeChildComponent(const std::string& CallerResolvedPath, const std::string& Tag,
                                          const nyx::runtime::Value& Props, const Iris::Component* Previous);

    // Builds a `NativeBuilderLookup` (IrisNyxEvaluator.h) bound to `NativeBuilders_` -- every
    // `MakeNyxEvaluator` call site in this class passes the result of this rather than reaching
    // into `NativeBuilders_` directly, so registering a new builder kind never needs touching
    // more than this one function.
    NativeBuilderLookup MakeNativeBuilderLookup();

    IrisConfig  Config_;
    std::string ProjectRoot_;

    nyx::host::NyxRuntime Runtime_;
    ChaosSlotMarker         Marker_;

    std::unordered_map<std::string, IrisIrDocument>              Documents_;
    std::unordered_map<std::string, nyx::host::NyxRuntime::NyxScope> FileScopes_;
    std::unordered_map<std::string, std::function<std::unique_ptr<Umbra::IWidget>()>> NativeBuilders_;

    std::vector<IrisIrRuntimeError> Errors_;
};

} // namespace Iris
