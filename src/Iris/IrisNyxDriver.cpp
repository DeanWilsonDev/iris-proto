#include "Iris/IrisNyxDriver.h"

#include "Iris/Driver.h"
#include "Iris/NyxLifecycleAdapter.h"
#include "Iris/NyxSignalDecorator.h"

#include "runtime/class-field-schema.hpp"
#include "runtime/environment.hpp"

#include <amanuensis/io/reader.hpp>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace Iris {

namespace {

// Casts `Instance`'s `DriverState` back to what this driver always actually stores there --
// see `IrisNyxDriver.h`'s own `NyxDriverState` doc comment. `nullptr` if `Instance` is null or
// was never mounted by this driver (an ordinary compiled-`.iris` `ComponentInstance`, whose
// `DriverState` is always left untouched/null).
NyxDriverState* GetDriverState(const std::shared_ptr<iris::ComponentInstance>& Instance) {
    if (Instance == nullptr || Instance->DriverState == nullptr) {
        return nullptr;
    }
    return static_cast<NyxDriverState*>(Instance->DriverState.get());
}

// Derives a reload tier by comparing which of `Old`'s own directly-declared bindings are
// `@signal`-backed (`OwnHasOnWrite` -- nothing else installs a write observer) against `New`'s
// -- the same comparison `NyxRuntime::ReInvokeComponent` already performs internally to decide
// what to carry forward, re-derived here since it isn't returned. Identical `@signal`-backed
// name/kind sets on both sides -- every one of `Old`'s carried forward unchanged, none added or
// retyped -- is tier 1 (`Unchanged`); anything else is tier 2 (`SignalLayoutChanged`).
iris::ComponentReloadTier CompareEnvironments(const nyx::runtime::Environment& Old, const nyx::runtime::Environment& New) {
    for (const std::string& Name : Old.OwnBindingNames()) {
        if (!Old.OwnHasOnWrite(Name)) {
            continue; // a plain local -- recomputed fresh every call, not part of this comparison
        }
        if (!New.OwnHasOnWrite(Name)) {
            return iris::ComponentReloadTier::SignalLayoutChanged; // removed, or no longer @signal
        }
        const nyx::runtime::Value* OldValue = Old.FindOwn(Name);
        const nyx::runtime::Value* NewValue = New.FindOwn(Name);
        if (OldValue == nullptr || NewValue == nullptr || OldValue->Kind() != NewValue->Kind()) {
            return iris::ComponentReloadTier::SignalLayoutChanged; // retyped
        }
    }
    for (const std::string& Name : New.OwnBindingNames()) {
        if (New.OwnHasOnWrite(Name) && !Old.OwnHasOnWrite(Name)) {
            return iris::ComponentReloadTier::SignalLayoutChanged; // a newly added @signal
        }
    }
    return iris::ComponentReloadTier::Unchanged;
}

// Pre-order collection of every direct-or-indirect component-invocation `Component` reachable
// from `Node` *without* descending past one already found -- a matched invocation's own nested
// invocations belong to a separate, later `InvokeComponent` call's own collection, not this
// one's (Component.h's own `InvocationTag` doc comment). Visits `Node.Children` in the same
// left-to-right document order `ConvertChildrenList` (IrisIrRuntime.cpp) walks the *new* IR in,
// so position N here corresponds to the Nth component invocation the new render tree's own walk
// encounters, absent a structural change (handled below by simply not matching).
//
// Deliberately does not descend into a `<Slot>` node's own `Children` for anything beyond what's
// already there structurally: a `<Slot>`'s dynamically-produced output is never stored in
// `Component::Children` at all (`SlotState` in SlotRuntime.h owns it instead, at the live-widget
// layer this backend-agnostic driver never touches) -- so a component invocation reached only
// through a `<Slot>` (a `.Map()`-rendered list, a conditional) is invisible to this collector and
// always mounts fresh, same as before this feature. Only a *statically*-nested invocation (a
// direct, unconditional child of a Core primitive) is found here.
void CollectInvocationsBelow(const Iris::Component& Node, std::vector<const Iris::Component*>& Out) {
    if (Node.InvocationTag.has_value()) {
        Out.push_back(&Node);
        return;
    }
    for (const Iris::Component& Child : Node.Children) {
        CollectInvocationsBelow(Child, Out);
    }
}

void CollectNestedInvocations(const Iris::Component& Root, std::vector<const Iris::Component*>& Out) {
    for (const Iris::Component& Child : Root.Children) {
        CollectInvocationsBelow(Child, Out);
    }
}

// Renders `V` back as parseable Nyx literal source text -- only the numeric kinds, since the
// only hook payload that exists today is `OnTick`'s `float dt` (`NyxLifecycleAdapter.h`'s own
// doc comment deliberately keeps this narrow rather than handling every ValueKind). `Float`
// uses enough digits to round-trip a float exactly (9 significant digits, matching the
// standard `FLT_DECIMAL_DIG`); `Double` uses `DBL_DECIMAL_DIG`'s 17, unused today but kept
// alongside for the same reason -- so a future numeric hook payload doesn't quietly lose
// precision on every reconstructed call.
std::string NumericLiteralText(const nyx::runtime::Value& V) {
    switch (V.Kind()) {
        case nyx::runtime::ValueKind::Int:
            return std::to_string(std::get<int32_t>(V.data));
        case nyx::runtime::ValueKind::Long:
            return std::to_string(std::get<int64_t>(V.data));
        case nyx::runtime::ValueKind::Float: {
            std::ostringstream Out;
            Out << std::setprecision(9) << std::get<float>(V.data);
            return Out.str();
        }
        case nyx::runtime::ValueKind::Double: {
            std::ostringstream Out;
            Out << std::setprecision(17) << std::get<double>(V.data);
            return Out.str();
        }
        default:
            return "0"; // unreachable for today's only hook payload (OnTick's float dt)
    }
}

// Model 1 (free-function component) half of docs/next-steps.md's ".irisx OnTick" entry:
// `Scope` is this invocation's own captured call-frame Environment (`NyxDriverState::
// RenderScope`) -- `FindOwn` (already public, `NyxRuntime::ReInvokeComponent`'s own
// @signal-carry-forward logic already relies on it) checks whether the component body
// declared a top-level local named `OnMount`/`OnUnmount`/`OnTick`
// (`auto OnTick = (float dt) -> { ... };`, functions-and-control-flow.md §10.2's own named-
// lambda-local syntax). Returns nullptr (no adapter, no per-frame cost at all) when none of
// the three names are declared -- the overwhelmingly common case for a component with no
// lifecycle needs.
std::shared_ptr<Umbra::IWidgetLifecycle> BuildFreeFunctionLifecycleAdapter(nyx::host::NyxRuntime&               Runtime,
                                                                            nyx::host::NyxRuntime::NyxScope& Scope) {
    const bool HasOnMount   = Scope.context.env->FindOwn("OnMount") != nullptr;
    const bool HasOnUnmount = Scope.context.env->FindOwn("OnUnmount") != nullptr;
    const bool HasOnTick    = Scope.context.env->FindOwn("OnTick") != nullptr;
    if (!HasOnMount && !HasOnUnmount && !HasOnTick) {
        return nullptr;
    }

    // Calls the named local by reconstructing "<HookName>(<args...>)" as source text and
    // evaluating it against `Scope` -- the same "reconstruct call-site source text, parse,
    // evaluate" mechanism `IrisNyxEvaluator.cpp`'s own `InvokeAsLambda` already uses for event-
    // handler props, since nyx-proto exposes no public "call this already-bound Value"
    // primitive to call the local directly. Re-parses on every call (including every OnTick,
    // once per frame) -- the same acknowledged cost `IrisNyxEvaluator.h`'s own doc comment
    // already accepts for event handlers, not attempted to be optimized away here.
    Iris::NyxLifecycleAdapter::HookInvoker Invoker =
        [&Runtime, &Scope](const std::string& HookName,
                            std::vector<nyx::runtime::Value> Args) -> std::optional<nyx::runtime::Value> {
        std::string Call = HookName + "(";
        for (std::size_t I = 0; I < Args.size(); ++I) {
            if (I != 0) {
                Call += ", ";
            }
            Call += NumericLiteralText(Args[I]);
        }
        Call += ")";
        return Runtime.EvaluateInScope(Scope, Call);
    };
    return std::make_shared<Iris::NyxLifecycleAdapter>(std::move(Invoker), HasOnMount, HasOnUnmount, HasOnTick);
}

} // namespace

IrisNyxDriver::IrisNyxDriver(IrisConfig Config, std::string ProjectRoot)
    : Config_(std::move(Config)), ProjectRoot_(std::move(ProjectRoot)) {
    iris::RegisterSignalDecorator(Runtime_);
    Marker_.RegisterOn(Runtime_);
    // Model 2 (class-based) components extend this -- decision-log.md §9.2's own framing: one
    // bridge/registration block iris-proto writes, `.irisx` authors just write
    // `class Tooltip : Component { ... }`. See NyxComponentBridge.h for why no `.Override(...)`
    // calls are needed.
    Runtime_.RegisterInheritableType<NyxComponentBase>("Component");
}

nyx::host::NyxRuntime& IrisNyxDriver::Runtime() { return Runtime_; }

void IrisNyxDriver::RegisterNativeBuilder(std::string Name, std::function<std::unique_ptr<Umbra::IWidget>()> Factory) {
    NativeBuilders_[std::move(Name)] = [Factory = std::move(Factory)](const nyx::runtime::Value&) {
        return Factory();
    };
}

void IrisNyxDriver::RegisterNativeBuilder(
    std::string Name, std::function<std::unique_ptr<Umbra::IWidget>(const nyx::runtime::Value&)> Factory) {
    NativeBuilders_[std::move(Name)] = std::move(Factory);
}

NativeBuilderLookup IrisNyxDriver::MakeNativeBuilderLookup() {
    return [this](const std::string& Name) -> std::function<std::unique_ptr<Umbra::IWidget>(const nyx::runtime::Value&)> {
        const auto It = NativeBuilders_.find(Name);
        return It != NativeBuilders_.end() ? It->second : nullptr;
    };
}

const IrisIrDocument* IrisNyxDriver::LoadDocument(const std::string& ResolvedPath) {
    const auto Cached = Documents_.find(ResolvedPath);
    if (Cached != Documents_.end()) {
        return &Cached->second;
    }

    std::ifstream File(ResolvedPath);
    if (!File) {
        Errors_.push_back(IrisIrRuntimeError{"cannot open '" + ResolvedPath + "'", IrSourceLocation{ResolvedPath}});
        return nullptr;
    }
    std::ostringstream Contents;
    Contents << File.rdbuf();

    const DriverResult Compiled = CompileFile(Contents.str(), ResolvedPath, Config_, ProjectRoot_);
    if (!Compiled.Diagnostics.empty()) {
        for (const DriverDiagnostic& Diag : Compiled.Diagnostics) {
            Errors_.push_back(IrisIrRuntimeError{
                Diag.Message,
                IrSourceLocation{Diag.Location.FilePath, Diag.Location.Line, Diag.Location.Column, 0}});
        }
        return nullptr;
    }

    const Amanuensis::ParseResult Parsed = Amanuensis::Reader::ParseString(Compiled.Output);
    if (!Parsed.succeeded) {
        Errors_.push_back(
            IrisIrRuntimeError{"'" + ResolvedPath + "' compiled to invalid IR JSON", IrSourceLocation{ResolvedPath}});
        return nullptr;
    }

    IrisIrDocumentParseResult DocumentResult = ParseIrisIrDocument(Parsed.value);
    if (!DocumentResult.Document.has_value()) {
        for (const IrisIrDocumentError& Err : DocumentResult.Errors) {
            Errors_.push_back(IrisIrRuntimeError{Err.Message, IrSourceLocation{ResolvedPath}});
        }
        return nullptr;
    }

    const auto Inserted = Documents_.emplace(ResolvedPath, std::move(*DocumentResult.Document));
    return &Inserted.first->second;
}

void IrisNyxDriver::InvalidateDocument(const std::string& ResolvedPath) { Documents_.erase(ResolvedPath); }

nyx::host::NyxRuntime::NyxScope& IrisNyxDriver::GetFileScope(const std::string& ResolvedPath,
                                                              const IrisIrDocument& Document) {
    const auto Cached = FileScopes_.find(ResolvedPath);
    if (Cached != FileScopes_.end()) {
        // Re-push the driver's *current* globals into this already-cached scope's
        // Interpreter before handing it back. CreateScope only seeds globals once, at
        // creation time -- the cached Interpreter is not a live view onto Runtime_.Globals(),
        // it's a snapshot -- so without this, a host's RegisterFunction/RegisterType/
        // RegisterNativeBuilder call made between two MountRoot calls on the same driver
        // (pharos-proto's own "register additional things up front" convention,
        // IrisNyxDriver.h's own doc comment) would be silently invisible to any file whose
        // scope was already cached, and a stale closure captured by the *previous*
        // registration could keep being called against freed state. DefineGlobal is safe to
        // call repeatedly with the same name -- Environment::Define unconditionally
        // overwrites, no error -- so this mirrors what CreateScope already does once at
        // creation time, just repeated on every cache hit. Doesn't affect ReloadRoot's own
        // reuse semantics: ReInvokeComponent/PatchClass patch this same live Interpreter in
        // place, unrelated to this cache-hit branch.
        for (const auto& [Name, Value] : Runtime_.Globals()) {
            Cached->second.interpreter->DefineGlobal(Name, Value);
        }
        return Cached->second;
    }
    const auto Inserted =
        FileScopes_.emplace(ResolvedPath, Runtime_.CreateScope(ReconstructNyxSource(Document), ResolvedPath));
    return Inserted.first->second;
}

Iris::Component IrisNyxDriver::InvokeComponent(const std::string& ResolvedPath, const std::string& FunctionName,
                                                std::vector<nyx::runtime::Value> Args, const Iris::Component* Previous,
                                                iris::ComponentReloadTier* OutTier) {
    NyxDriverState* PriorState = Previous != nullptr ? GetDriverState(Previous->Instance) : nullptr;

    if (PriorState != nullptr) {
        // Reloading: the file on disk may have changed since it was last compiled/cached.
        InvalidateDocument(ResolvedPath);
    }

    const IrisIrDocument* Document = LoadDocument(ResolvedPath);
    if (Document == nullptr) {
        return Iris::Component{nullptr};
    }

    const IrRenderBlockNode* Block = nullptr;
    for (const IrBodyNode& Node : Document->Body) {
        if (const auto* Candidate = std::get_if<IrRenderBlockNode>(&Node)) {
            Block = Candidate;
            break;
        }
    }
    if (Block == nullptr) {
        Errors_.push_back(
            IrisIrRuntimeError{"'" + ResolvedPath + "' has no render { } block", IrSourceLocation{ResolvedPath}});
        return Iris::Component{nullptr};
    }

    nyx::host::NyxRuntime::NyxScope& FileScope = GetFileScope(ResolvedPath, *Document);

    // Model detection (nyx-scripting-language/decision-log.md §9.2): a Model 1 free function
    // and a Model 2 class can share a name in different files without ambiguity, so this only
    // needs to check whichever single `FunctionName` this one invocation actually names.
    const nyx::interpreter::DeclRegistry& Registry = FileScope.interpreter->Registry();
    const bool                             IsClass    = Registry.classes.count(FunctionName) != 0;
    const bool                             IsFunction = Registry.functions.count(FunctionName) != 0;

    // Nested-reload matching (docs/next-steps.md): only meaningful when this call is itself a
    // reload (`PriorState != nullptr`) -- `Previous->Children` is this exact invocation's own
    // previously-rendered output, the thing a statically-nested child invocation inside it
    // should be matched against. A fresh mount (PriorState == nullptr, including every nested
    // child invocation reached via an ordinary, non-reload-aware mount) leaves this empty, so
    // every lookup below misses and every child mounts fresh -- unchanged from before this
    // feature. Shared (not per-closure-copy) mutable state, same convention IrisNyxEvaluator.cpp's
    // own EvaluateSlot uses for PickScopes -- a std::function may be copied as it's threaded
    // through MakeNyxEvaluator, and every copy must see the same cursor position.
    auto PreviousInvocations = std::make_shared<std::vector<const Iris::Component*>>();
    if (PriorState != nullptr) {
        CollectNestedInvocations(*Previous, *PreviousInvocations);
    }
    auto NextPreviousIndex = std::make_shared<std::size_t>(0);

    const ChildComponentInvoker ChildInvoker = [this, ResolvedPath, PreviousInvocations, NextPreviousIndex](
                                                     const std::string& Tag, const nyx::runtime::Value& Props,
                                                     const IrElementNode&) {
        // Tag-only, position-based matching -- not tag+key like ReconcileWidget's own rule:
        // this decision has to be made *before* the new invocation's own `key` prop (if any)
        // gets evaluated, which only happens one level up in the *caller's* ConvertIrElement,
        // after EvaluateComponentInvocation (and hence this callback) already returned a
        // mounted-or-reloaded Component. Reasonable for the statically-nested case this feature
        // targets (an unconditional, always-present child rarely needs a key at all); a
        // structural change at this position simply fails to match (falls through to a fresh
        // mount) rather than mismatching, same fail-safe direction `ReconcileWidget`'s own
        // tag/key-mismatch fallback already takes.
        const Iris::Component* Matched = nullptr;
        if (*NextPreviousIndex < PreviousInvocations->size()) {
            const Iris::Component* Candidate = (*PreviousInvocations)[*NextPreviousIndex];
            if (Candidate->InvocationTag == Tag) {
                Matched = Candidate;
            }
        }
        ++*NextPreviousIndex;
        return InvokeChildComponent(ResolvedPath, Tag, Props, Matched);
    };

    if (IsClass) {
        if (!FileScope.interpreter->IsSubtypeOf(FunctionName, "Component")) {
            Errors_.push_back(IrisIrRuntimeError{
                "class '" + FunctionName + "' in '" + ResolvedPath + "' must extend Component to be mounted",
                IrSourceLocation{ResolvedPath}});
            return Iris::Component{nullptr};
        }
        return InvokeClassComponent(*Document, *Block, FileScope, FunctionName, std::move(Args), Previous, OutTier,
                                     ChildInvoker);
    }
    if (!IsFunction) {
        Errors_.push_back(IrisIrRuntimeError{
            "'" + ResolvedPath + "' declares no function or class named '" + FunctionName + "'",
            IrSourceLocation{ResolvedPath}});
        return Iris::Component{nullptr};
    }

    if (PriorState == nullptr) {
        // Fresh mount -- no prior invocation to reload against (an ordinary MountRoot call, a
        // nested child invocation, or a reload whose named entry has nothing reusable yet).
        if (OutTier != nullptr) {
            *OutTier = iris::ComponentReloadTier::SignalLayoutChanged; // nothing carried forward
        }
        return iris::MountComponentInstance([&]() -> Iris::Component {
            auto State = std::make_shared<NyxDriverState>(
                NyxDriverState{Runtime_.InvokeComponent(FileScope, FunctionName, std::move(Args)), std::nullopt});
            State->Lifecycle = BuildFreeFunctionLifecycleAdapter(Runtime_, State->RenderScope);

            const NyxEvaluator Eval = MakeNyxEvaluator(Runtime_, State->RenderScope, Marker_, ChildInvoker, &Errors_, MakeNativeBuilderLookup());
            Iris::Component    Result = ConvertIrElement(Block->Root, Eval, &Errors_);

            // Keep `State` alive for as long as this mounted instance is -- every NyxEvaluator
            // closure above (in particular a <Slot>'s own callable, IrisIrRuntime.cpp's
            // ConvertSlot) holds a raw reference into it, and may be re-invoked by iris::Tick()
            // long after this lambda returns. Same reasoning for `State->Lifecycle`'s own raw
            // reference into `State->RenderScope`, held via `Instance->Lifecycle` below.
            iris::ComponentInstance* Instance = iris::IrisRuntime::Instance().CurrentComponentInstance();
            if (Instance != nullptr) {
                Instance->Lifecycle  = State->Lifecycle.get();
                Instance->DriverState = std::static_pointer_cast<void>(std::move(State));
            }
            return Result;
        });
    }

    // Reload: re-invoke the (possibly changed) FunctionName fresh, reconciling @signal state
    // from PriorState->RenderScope into the result by name (Runtime_.ReInvokeComponent's own
    // job) -- then reuse the *same* ComponentInstance rather than minting a new one. Deliberately
    // not iris::ReloadComponentInstance/BeginReloadReplay/EndReloadReplay -- see this class's own
    // InvokeComponent doc comment (IrisNyxDriver.h) for why that machinery doesn't apply here.
    //
    // Still needs the *ambient-instance* half of that same machinery, though:
    // NyxSignalDecorator's own OnApply (`@signal`'s registration) is a silent no-op with no
    // ambient ComponentInstance (NyxSignalDecorator.cpp) -- without this, the re-invoked
    // function's own fresh `@signal` declarations would never get `Environment::SetOnWrite`
    // installed, so CompareEnvironments below would see every @signal as "removed" even when
    // nothing changed. `Previous->Instance` (not a fresh one) is exactly right here: it's the
    // same instance `<Slot>` dependency tracking (TrackSignalDependency/NotifySignalDependents)
    // already associates this component's signals with.
    nyx::host::NyxRuntime::NyxScope NewScope;
    {
        iris::Detail::ScopedComponentInstance Guard(Previous->Instance.get());
        NewScope = Runtime_.ReInvokeComponent(PriorState->RenderScope, ReconstructNyxSource(*Document), FunctionName, Args);
    }
    const iris::ComponentReloadTier Tier =
        CompareEnvironments(*PriorState->RenderScope.context.env, *NewScope.context.env);
    if (OutTier != nullptr) {
        *OutTier = Tier;
    }

    auto NewState = std::make_shared<NyxDriverState>(NyxDriverState{std::move(NewScope), std::nullopt});
    // Rebuilt fresh every reload (not carried forward from PriorState->Lifecycle): NewState's
    // own RenderScope is a genuinely new NyxScope each reload, and the old adapter's Invoker_
    // held a raw reference into the *old* one -- see BuildFreeFunctionLifecycleAdapter's own
    // doc comment. Also naturally picks up a hook added/removed by the reloaded source, the
    // same way CompareEnvironments already re-derives the @signal-layout tier fresh each time.
    NewState->Lifecycle = BuildFreeFunctionLifecycleAdapter(Runtime_, NewState->RenderScope);
    Previous->Instance->Lifecycle   = NewState->Lifecycle.get();
    Previous->Instance->DriverState = std::static_pointer_cast<void>(NewState);

    const NyxEvaluator Eval = MakeNyxEvaluator(Runtime_, NewState->RenderScope, Marker_, ChildInvoker, &Errors_, MakeNativeBuilderLookup());
    Iris::Component    Result = ConvertIrElement(Block->Root, Eval, &Errors_);
    Result.Instance = Previous->Instance;
    Result.ReloadTier = Tier;
    return Result;
}

namespace {

// The `Render` method's own declared first parameter name (`void Render(TooltipProps props)`'s
// `props`) -- read from the real AST rather than assumed, the same "extract the real
// identifier, don't guess" posture `IrisNyxEvaluator.cpp`'s own `.Map()`/`.Reduce()` parameter
// scanner already takes. Falls back to `"props"` if `Render` isn't declared at all (or takes no
// parameter) -- a `render{}` block with no matching `Render(...)` method is already a real
// authoring mistake elsewhere in the file; this only needs *some* deterministic name so a
// component that happens not to reference its own render parameter still mounts.
std::string RenderParamName(const nyx::interpreter::DeclRegistry& Registry, const std::string& ClassName) {
    const auto ClassIt = Registry.classes.find(ClassName);
    if (ClassIt != Registry.classes.end()) {
        for (const nyx::ast::MethodDecl& Method : ClassIt->second->methods) {
            if (Method.name == "Render" && !Method.params.empty()) {
                return Method.params.front().name;
            }
        }
    }
    return "props";
}

// Model 2 (class-based component) half of docs/next-steps.md's ".irisx OnTick" entry: true
// if `ClassName`'s own declared methods (not walking any base chain -- `RenderParamName`
// above takes the same "own methods only" shortcut, reasonable here since Model 2 components
// only ever extend the zero-method `NyxComponentBase` marker, `NyxComponentBridge.h`) include
// one named `MethodName`.
bool ClassDeclaresMethod(const nyx::interpreter::DeclRegistry& Registry, const std::string& ClassName,
                          const std::string& MethodName) {
    const auto ClassIt = Registry.classes.find(ClassName);
    if (ClassIt == Registry.classes.end()) {
        return false;
    }
    for (const nyx::ast::MethodDecl& Method : ClassIt->second->methods) {
        if (Method.name == MethodName) {
            return true;
        }
    }
    return false;
}

// Model 2's counterpart to `BuildFreeFunctionLifecycleAdapter` above: `Instance` is the live
// `NyxObject` `Interpreter::Instantiate`/a prior mount already produced for this invocation.
// Unlike Model 1, no source-text reconstruction is needed -- `Interpreter::
// TryCallInstanceMethod` (already public) calls a named instance method directly and reports
// "no override" as `std::nullopt` rather than an error, which is exactly `NyxLifecycleAdapter`'s
// own "hook not declared" no-op contract -- so `HasOnMount`/`HasOnUnmount`/`HasOnTick` here are
// only a cheap up-front skip for a class that declares none of the three, not a correctness
// requirement the way Model 1's own `FindOwn` check is.
std::shared_ptr<Umbra::IWidgetLifecycle> BuildClassLifecycleAdapter(
    const nyx::host::NyxRuntime::NyxScope& FileScope, const std::string& ClassName,
    std::shared_ptr<nyx::runtime::NyxObject> Instance) {
    const nyx::interpreter::DeclRegistry& Registry = FileScope.interpreter->Registry();
    const bool                             HasOnMount   = ClassDeclaresMethod(Registry, ClassName, "OnMount");
    const bool                             HasOnUnmount = ClassDeclaresMethod(Registry, ClassName, "OnUnmount");
    const bool                             HasOnTick    = ClassDeclaresMethod(Registry, ClassName, "OnTick");
    if (!HasOnMount && !HasOnUnmount && !HasOnTick) {
        return nullptr;
    }

    Iris::NyxLifecycleAdapter::HookInvoker Invoker =
        [Interp = FileScope.interpreter, Instance](
            const std::string& HookName, std::vector<nyx::runtime::Value> Args) -> std::optional<nyx::runtime::Value> {
        return Interp->TryCallInstanceMethod(nyx::runtime::Value(Instance), HookName, std::move(Args));
    };
    return std::make_shared<Iris::NyxLifecycleAdapter>(std::move(Invoker), HasOnMount, HasOnUnmount, HasOnTick);
}

// A snapshot of a `NyxObject`'s own field shape -- name plus `ValueKind`, per field -- taken
// before and after `ReconcileInstanceFields` to derive a reload tier independently
// (`ComponentInstance::EndReloadReplay`'s own IRIS_SIGNAL-counting logic has nothing to do with
// this state shape at all -- decision-log.md §9.2's own item 4 guidance, same reasoning
// `CompareEnvironments` above already applies to the free-function case).
//
// Post-nyx-proto-e28c957 (decision-log.md §8.7/§8.8): `NyxObject::fields` (a single
// unordered_map) no longer exists -- an instance is either schema mode (`schema` set,
// field names live on the shared `ClassFieldSchema`, values in the parallel `slots`
// vector) or ad hoc mode (`schema == nullptr`, name/value pairs in `adHocFields`
// directly). Every instance `ReconcileInstanceFields` ever snapshots is a real
// class/data instance, so schema mode is the expected path here; ad hoc mode (host-
// constructed objects like Iris's own component-prop bags) is handled too since
// nothing about this helper's signature restricts it to one mode.
std::unordered_map<std::string, nyx::runtime::ValueKind> FieldShape(const nyx::runtime::NyxObject& Instance) {
    std::unordered_map<std::string, nyx::runtime::ValueKind> Shape;
    if (Instance.schema) {
        for (size_t i = 0; i < Instance.schema->slots.size(); ++i) {
            Shape.emplace(Instance.schema->slots[i].name, Instance.slots[i].Kind());
        }
    } else {
        for (const auto& [Name, Val] : Instance.adHocFields) {
            Shape.emplace(Name, Val.Kind());
        }
    }
    return Shape;
}

} // namespace

Iris::Component IrisNyxDriver::InvokeClassComponent(const IrisIrDocument& Document, const IrRenderBlockNode& Block,
                                                      nyx::host::NyxRuntime::NyxScope& FileScope,
                                                      const std::string& ClassName, std::vector<nyx::runtime::Value> Args,
                                                      const Iris::Component* Previous, iris::ComponentReloadTier* OutTier,
                                                      const ChildComponentInvoker& ChildInvoker) {
    const std::string RenderParam = RenderParamName(FileScope.interpreter->Registry(), ClassName);
    const nyx::runtime::Value RenderArg = Args.empty() ? nyx::runtime::Value() : Args.front();

    NyxDriverState* PriorState = Previous != nullptr ? GetDriverState(Previous->Instance) : nullptr;
    const bool       CanReload  = PriorState != nullptr && PriorState->ClassInstance.has_value();

    if (!CanReload) {
        if (OutTier != nullptr) {
            *OutTier = iris::ComponentReloadTier::SignalLayoutChanged; // nothing carried forward
        }
        return iris::MountComponentInstance([&]() -> Iris::Component {
            // Instantiate runs field defaults then the constructor (decision-log.md §5.16) --
            // this already has an ambient ComponentInstance (the one MountComponentInstance
            // just pushed around this whole lambda), so any @signal field default fires its
            // decorator against the right instance, same as an ordinary Model 1 mount.
            nyx::runtime::Value Instance = FileScope.interpreter->Instantiate(ClassName, Args);
            auto InstanceObj = std::get<std::shared_ptr<nyx::runtime::NyxObject>>(Instance.data);

            auto RenderEnv = std::make_shared<nyx::runtime::Environment>(FileScope.context.env);
            RenderEnv->Define(RenderParam, RenderArg);

            auto State = std::make_shared<NyxDriverState>(NyxDriverState{
                nyx::host::NyxRuntime::NyxScope{FileScope.interpreter,
                                                 nyx::interpreter::EvalContext{RenderEnv, InstanceObj}},
                Instance});
            State->Lifecycle = BuildClassLifecycleAdapter(FileScope, ClassName, InstanceObj);

            const NyxEvaluator Eval = MakeNyxEvaluator(Runtime_, State->RenderScope, Marker_, ChildInvoker, &Errors_, MakeNativeBuilderLookup());
            Iris::Component    Result = ConvertIrElement(Block.Root, Eval, &Errors_);

            iris::ComponentInstance* CurrentInstance = iris::IrisRuntime::Instance().CurrentComponentInstance();
            if (CurrentInstance != nullptr) {
                CurrentInstance->Lifecycle   = State->Lifecycle.get();
                CurrentInstance->DriverState = std::static_pointer_cast<void>(std::move(State));
            }
            return Result;
        });
    }

    // Reload: PatchClass + ReconcileInstanceFields against the *same* live NyxObject --
    // deliberately not iris::ReloadComponentInstance (see this class's own InvokeClassComponent
    // doc comment, IrisNyxDriver.h). ReconcileInstanceFields may re-fire a new/reset field's own
    // decorator (its own doc comment: "including re-firing its decorator"), so this needs the
    // same ambient-instance requirement CompareEnvironments's free-function counterpart already
    // does for ReInvokeComponent.
    auto InstanceObj = std::get<std::shared_ptr<nyx::runtime::NyxObject>>(PriorState->ClassInstance->data);
    const auto ShapeBefore = FieldShape(*InstanceObj);
    {
        iris::Detail::ScopedComponentInstance Guard(Previous->Instance.get());
        Runtime_.PatchClass(FileScope, ReconstructNyxSource(Document), ClassName);
        FileScope.interpreter->ReconcileInstanceFields(InstanceObj);
    }
    const iris::ComponentReloadTier Tier =
        ShapeBefore == FieldShape(*InstanceObj) ? iris::ComponentReloadTier::Unchanged
                                                 : iris::ComponentReloadTier::SignalLayoutChanged;
    if (OutTier != nullptr) {
        *OutTier = Tier;
    }

    auto RenderEnv = std::make_shared<nyx::runtime::Environment>(FileScope.context.env);
    RenderEnv->Define(RenderParam, RenderArg);
    auto NewState = std::make_shared<NyxDriverState>(NyxDriverState{
        nyx::host::NyxRuntime::NyxScope{FileScope.interpreter, nyx::interpreter::EvalContext{RenderEnv, InstanceObj}},
        *PriorState->ClassInstance});
    // Rebuilt fresh every reload, same reasoning as InvokeComponent's own Model 1 reload
    // branch -- naturally picks up a hook added/removed by the just-`PatchClass`-ed source.
    // `InstanceObj` itself (unlike Model 1's RenderScope) is the *same* live NyxObject across
    // a reload, so this isn't fixing a dangling reference, only keeping the declared-hook set
    // current.
    NewState->Lifecycle = BuildClassLifecycleAdapter(FileScope, ClassName, InstanceObj);
    Previous->Instance->Lifecycle   = NewState->Lifecycle.get();
    Previous->Instance->DriverState = std::static_pointer_cast<void>(NewState);

    const NyxEvaluator Eval = MakeNyxEvaluator(Runtime_, NewState->RenderScope, Marker_, ChildInvoker, &Errors_, MakeNativeBuilderLookup());
    Iris::Component    Result = ConvertIrElement(Block.Root, Eval, &Errors_);
    Result.Instance = Previous->Instance;
    Result.ReloadTier = Tier;
    return Result;
}

Iris::Component IrisNyxDriver::InvokeChildComponent(const std::string& CallerResolvedPath, const std::string& Tag,
                                                      const nyx::runtime::Value& Props,
                                                      const Iris::Component* Previous) {
    const IrisIrDocument* CallerDocument = LoadDocument(CallerResolvedPath);
    if (CallerDocument == nullptr) {
        return Iris::Component{nullptr};
    }

    const IrImportNode* Import = nullptr;
    for (const IrImportNode& Candidate : CallerDocument->Imports) {
        if (Candidate.Name == Tag) {
            Import = &Candidate;
            break;
        }
    }
    if (Import == nullptr) {
        Errors_.push_back(IrisIrRuntimeError{
            "<" + Tag + "> is not imported by '" + CallerResolvedPath +
                "' -- cross-file component invocation requires an explicit `import " + Tag + "` statement",
            IrSourceLocation{CallerResolvedPath}});
        return Iris::Component{nullptr};
    }

    // `Previous` (a matched nested invocation from the caller's own prior render, or nullptr for
    // an ordinary/unmatched mount) is forwarded straight through as InvokeComponent's own reload
    // parameter -- this composes recursively for free: if it names a reusable prior instance,
    // InvokeComponent's own reload branch builds its own fresh nested-invocation cursor from
    // *this* invocation's own children, so a match here can itself contain further statically-
    // nested matches, propagating to any depth without this function needing to know that.
    return InvokeComponent(Import->ResolvedPath, Tag, {Props}, Previous, nullptr);
}

Iris::Component IrisNyxDriver::MountRoot(const std::string& EntryResolvedPath, const std::string& EntryFunctionName,
                                          std::vector<nyx::runtime::Value> InitialArgs) {
    return InvokeComponent(EntryResolvedPath, EntryFunctionName, std::move(InitialArgs), nullptr, nullptr);
}

IrisNyxReloadResult IrisNyxDriver::ReloadRoot(const std::string& EntryResolvedPath,
                                               const std::string& EntryFunctionName,
                                               std::vector<nyx::runtime::Value> InitialArgs,
                                               const Iris::Component& PreviousRoot) {
    iris::ComponentReloadTier Tier = iris::ComponentReloadTier::SignalLayoutChanged;
    Iris::Component Root = InvokeComponent(EntryResolvedPath, EntryFunctionName, std::move(InitialArgs), &PreviousRoot, &Tier);
    return IrisNyxReloadResult{std::move(Root), Tier};
}

} // namespace Iris
