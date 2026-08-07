#include "Iris/IrisNyxDriver.h"

#include "Iris/Driver.h"
#include "Iris/NyxSignalDecorator.h"

#include "runtime/environment.hpp"

#include <amanuensis/io/reader.hpp>

#include <fstream>
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
    NativeBuilders_[std::move(Name)] = std::move(Factory);
}

NativeBuilderLookup IrisNyxDriver::MakeNativeBuilderLookup() {
    return [this](const std::string& Name) -> std::function<std::unique_ptr<Umbra::IWidget>()> {
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

            const NyxEvaluator Eval = MakeNyxEvaluator(Runtime_, State->RenderScope, Marker_, ChildInvoker, &Errors_, MakeNativeBuilderLookup());
            Iris::Component    Result = ConvertIrElement(Block->Root, Eval, &Errors_);

            // Keep `State` alive for as long as this mounted instance is -- every NyxEvaluator
            // closure above (in particular a <Slot>'s own callable, IrisIrRuntime.cpp's
            // ConvertSlot) holds a raw reference into it, and may be re-invoked by iris::Tick()
            // long after this lambda returns.
            iris::ComponentInstance* Instance = iris::IrisRuntime::Instance().CurrentComponentInstance();
            if (Instance != nullptr) {
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

// A snapshot of a `NyxObject`'s own field shape -- name plus `ValueKind`, per field -- taken
// before and after `ReconcileInstanceFields` to derive a reload tier independently
// (`ComponentInstance::EndReloadReplay`'s own IRIS_SIGNAL-counting logic has nothing to do with
// this state shape at all -- decision-log.md §9.2's own item 4 guidance, same reasoning
// `CompareEnvironments` above already applies to the free-function case).
std::unordered_map<std::string, nyx::runtime::ValueKind> FieldShape(const nyx::runtime::NyxObject& Instance) {
    std::unordered_map<std::string, nyx::runtime::ValueKind> Shape;
    for (const auto& [Name, Value] : Instance.fields) {
        Shape.emplace(Name, Value.Kind());
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

            const NyxEvaluator Eval = MakeNyxEvaluator(Runtime_, State->RenderScope, Marker_, ChildInvoker, &Errors_, MakeNativeBuilderLookup());
            Iris::Component    Result = ConvertIrElement(Block.Root, Eval, &Errors_);

            iris::ComponentInstance* CurrentInstance = iris::IrisRuntime::Instance().CurrentComponentInstance();
            if (CurrentInstance != nullptr) {
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
