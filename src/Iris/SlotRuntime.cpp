#include "Iris/SlotRuntime.h"
#include "Iris/Reconciler.h"

#include <algorithm>
#include <unordered_map>
#include <variant>

namespace iris {

namespace {

// Which SlotState objects registered a dependency on which signal (identified by the
// signal's own `this` pointer, type-erased). Owns no lifetime of either side — a
// SlotState clears its own entries on destruction (SlotState::~SlotState), and nothing
// here ever outlives a Signal's own storage since notification only ever happens while
// the Signal itself is still alive and calling NotifySignalDependents.
class SignalRegistry {
public:
    static SignalRegistry& Instance() {
        static SignalRegistry Registry;
        return Registry;
    }

    void TrackRead(const void* SignalIdentity, SlotState* Slot) { Subscribers_[SignalIdentity].push_back(Slot); }

    void Notify(const void* SignalIdentity) {
        const auto It = Subscribers_.find(SignalIdentity);
        if (It == Subscribers_.end()) {
            return;
        }
        for (SlotState* Slot : It->second) {
            Slot->MarkDirty();
        }
    }

    // Removes every dependency Slot registered, on any signal — called both when a
    // slot is about to re-invoke its callable (dependencies can differ between
    // invocations, e.g. a conditional signal read) and when it's destroyed.
    void ClearSlot(SlotState* Slot) {
        for (auto& [Identity, Slots] : Subscribers_) {
            Slots.erase(std::remove(Slots.begin(), Slots.end(), Slot), Slots.end());
        }
    }

private:
    std::unordered_map<const void*, std::vector<SlotState*>> Subscribers_;
};

} // namespace

void TrackSignalDependency(const void* SignalIdentity) {
    if (SlotState* Active = IrisRuntime::Instance().ActiveSlot()) {
        SignalRegistry::Instance().TrackRead(SignalIdentity, Active);
    }
}

void NotifySignalDependents(const void* SignalIdentity) { SignalRegistry::Instance().Notify(SignalIdentity); }

SlotState::SlotState(std::shared_ptr<Iris::IrisSlotCallable> Callable, MountFn Mount)
    : Callable_(std::move(Callable)), Mount_(std::move(Mount)) {}

SlotState::~SlotState() {
    SignalRegistry::Instance().ClearSlot(this);
    IrisRuntime::Instance().UnregisterSlot(this);

    // Attached mode: whatever this slot last rendered lives inside AttachedParent_'s
    // own children, not in SingleWidget_ — remove and drop it so the parent doesn't
    // keep displaying content nothing manages anymore.
    if (AttachedParent_ != nullptr && PreviousSingle_.Tag != Iris::IrisElementTag::None) {
        AttachedParent_->RemoveChildAt(AttachedIndex_);
    }
}

void SlotState::MarkDirty() {
    Dirty_ = true;
    IrisRuntime::Instance().RegisterDirtySlot(this);
}

void SlotState::AttachAt(Umbra::IWidget* Parent, std::size_t Index) {
    AttachedParent_ = Parent;
    AttachedIndex_ = Index;
}

bool SlotState::HasMountedContent() const { return PreviousSingle_.Tag != Iris::IrisElementTag::None; }

void SlotState::Reconcile() {
    if (Mounted_ && !Dirty_) {
        return;
    }
    Mounted_ = true;
    Dirty_ = false;

    SignalRegistry::Instance().ClearSlot(this);
    IrisRuntime::Instance().PushActiveSlot(this);

    if (std::holds_alternative<std::function<Iris::IrisComponent()>>(Callable_->Callable)) {
        Iris::IrisComponent NewOutput = std::get<std::function<Iris::IrisComponent()>>(Callable_->Callable)();
        IrisRuntime::Instance().PopActiveSlot();

        if (AttachedParent_ != nullptr) {
            // Only pull the current widget back out if something is actually there —
            // a previous None output means AttachedIndex_ has no corresponding entry
            // in AttachedParent_'s children at all (None contributes nothing, same as
            // everywhere else in the reconciler).
            std::unique_ptr<Umbra::IWidget> Current;
            if (PreviousSingle_.Tag != Iris::IrisElementTag::None) {
                Current = AttachedParent_->RemoveChildAt(AttachedIndex_);
            }
            ReconcileWidget(Current, PreviousSingle_, NewOutput, Mount_);
            if (Current != nullptr) {
                AttachedParent_->InsertChildAt(AttachedIndex_, std::move(Current));
            }
        } else {
            ReconcileWidget(SingleWidget_, PreviousSingle_, NewOutput, Mount_);
        }
        PreviousSingle_ = std::move(NewOutput);
    } else {
        std::vector<Iris::IrisComponent> NewOutput =
            std::get<std::function<std::vector<Iris::IrisComponent>()>>(Callable_->Callable)();
        IrisRuntime::Instance().PopActiveSlot();

        ReconcileChildren(ListWidgets_, PreviousList_, NewOutput, Mount_);
        PreviousList_ = std::move(NewOutput);
    }
}

IrisRuntime& IrisRuntime::Instance() {
    static IrisRuntime Runtime;
    return Runtime;
}

void IrisRuntime::BeginBatch() { ++BatchDepth_; }

void IrisRuntime::EndBatch() {
    if (BatchDepth_ > 0) {
        --BatchDepth_;
    }
    if (BatchDepth_ == 0) {
        ReconcileDirtySlots();
    }
}

void IrisRuntime::ReconcileDirtySlots() {
    // Snapshot first: reconciling a slot can mark other slots dirty (e.g. a Signal set
    // from inside a <Slot>'s own callable, however unusual), which must not invalidate
    // the vector this loop is iterating.
    std::vector<SlotState*> Snapshot;
    Snapshot.swap(DirtySlots_);
    for (SlotState* Slot : Snapshot) {
        Slot->Reconcile();
    }
}

void IrisRuntime::RegisterDirtySlot(SlotState* Slot) {
    if (std::find(DirtySlots_.begin(), DirtySlots_.end(), Slot) == DirtySlots_.end()) {
        DirtySlots_.push_back(Slot);
    }
}

void IrisRuntime::UnregisterSlot(SlotState* Slot) {
    DirtySlots_.erase(std::remove(DirtySlots_.begin(), DirtySlots_.end(), Slot), DirtySlots_.end());
    ActiveSlotStack_.erase(std::remove(ActiveSlotStack_.begin(), ActiveSlotStack_.end(), Slot),
                            ActiveSlotStack_.end());
}

void IrisRuntime::PushActiveSlot(SlotState* Slot) { ActiveSlotStack_.push_back(Slot); }

void IrisRuntime::PopActiveSlot() { ActiveSlotStack_.pop_back(); }

SlotState* IrisRuntime::ActiveSlot() const { return ActiveSlotStack_.empty() ? nullptr : ActiveSlotStack_.back(); }

void IrisRuntime::PushComponentInstance(ComponentInstance* Instance) { ComponentInstanceStack_.push_back(Instance); }

void IrisRuntime::PopComponentInstance() { ComponentInstanceStack_.pop_back(); }

ComponentInstance* IrisRuntime::CurrentComponentInstance() const {
    return ComponentInstanceStack_.empty() ? nullptr : ComponentInstanceStack_.back();
}

void Tick() { IrisRuntime::Instance().ReconcileDirtySlots(); }

} // namespace iris
