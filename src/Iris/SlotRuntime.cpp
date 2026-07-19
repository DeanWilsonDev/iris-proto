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

void SlotSiblingGroup::AddEntry(std::size_t StaticPrefixCount, SlotState* Slot) {
    Entries_.push_back(Entry{StaticPrefixCount, Slot});
}

std::size_t SlotSiblingGroup::EntryCount() const { return Entries_.size(); }

std::size_t SlotSiblingGroup::AbsoluteIndexOf(std::size_t GroupIndex) const {
    std::size_t Index = Entries_[GroupIndex].StaticPrefixCount;
    for (std::size_t I = 0; I < GroupIndex; ++I) {
        // A destroyed sibling (MarkDestroyed) already removed its own widgets from the
        // shared parent, so it contributes nothing from here on — and its SlotState is
        // gone, so CurrentRealChildCount() can no longer be called on it at all.
        if (Entries_[I].Slot != nullptr) {
            Index += Entries_[I].Slot->CurrentRealChildCount();
        }
    }
    return Index;
}

void SlotSiblingGroup::MarkDestroyed(std::size_t GroupIndex) { Entries_[GroupIndex].Slot = nullptr; }

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
    // own children, not in SingleWidget_/ListWidgets_ — remove and drop every real
    // widget it currently owns so the parent doesn't keep displaying content nothing
    // manages anymore. AttachedGroup_ may itself be gone already (a sibling slot could
    // have been destroyed first) — indices below AttachedGroupIndex_'s own live count
    // don't depend on it, since we always remove starting at our own base position and
    // this slot's own AttachedCount_ is authoritative for how many are ours.
    if (AttachedParent_ != nullptr && AttachedGroup_ != nullptr) {
        const std::size_t Base = AttachedGroup_->AbsoluteIndexOf(AttachedGroupIndex_);
        for (std::size_t I = 0; I < AttachedCount_; ++I) {
            AttachedParent_->RemoveChildAt(Base);
        }
        // From here on, a sibling still to be destroyed (in either direction — nothing
        // requires group-order teardown) must not dereference this now-about-to-be-freed
        // SlotState via AttachedGroup_->AbsoluteIndexOf.
        AttachedGroup_->MarkDestroyed(AttachedGroupIndex_);
    }
}

void SlotState::MarkDirty() {
    Dirty_ = true;
    IrisRuntime::Instance().RegisterDirtySlot(this);
}

void SlotState::AttachToGroup(Umbra::IWidget* Parent, std::shared_ptr<SlotSiblingGroup> Group,
                               std::size_t GroupIndex) {
    AttachedParent_ = Parent;
    AttachedGroup_ = std::move(Group);
    AttachedGroupIndex_ = GroupIndex;
}

std::size_t SlotState::CurrentRealChildCount() const { return AttachedCount_; }

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
            // Ask the group fresh — an earlier sibling's own contribution may have
            // changed (e.g. its own list grew) since the last time this slot
            // reconciled, shifting where this slot's own content now lives.
            const std::size_t Base = AttachedGroup_->AbsoluteIndexOf(AttachedGroupIndex_);
            std::unique_ptr<Umbra::IWidget> Current;
            if (AttachedCount_ != 0) {
                Current = AttachedParent_->RemoveChildAt(Base);
            }
            ReconcileWidget(Current, PreviousSingle_, NewOutput, Mount_);
            if (Current != nullptr) {
                AttachedParent_->InsertChildAt(Base, std::move(Current));
            }
            AttachedCount_ = (NewOutput.Tag != Iris::IrisElementTag::None) ? 1 : 0;
        } else {
            ReconcileWidget(SingleWidget_, PreviousSingle_, NewOutput, Mount_);
        }
        PreviousSingle_ = std::move(NewOutput);
    } else {
        std::vector<Iris::IrisComponent> NewOutput =
            std::get<std::function<std::vector<Iris::IrisComponent>()>>(Callable_->Callable)();
        IrisRuntime::Instance().PopActiveSlot();

        if (AttachedParent_ != nullptr) {
            const std::size_t Base = AttachedGroup_->AbsoluteIndexOf(AttachedGroupIndex_);

            std::vector<std::unique_ptr<Umbra::IWidget>> Current;
            Current.reserve(AttachedCount_);
            for (std::size_t I = 0; I < AttachedCount_; ++I) {
                Current.push_back(AttachedParent_->RemoveChildAt(Base));
            }

            ReconcileChildren(Current, PreviousList_, NewOutput, Mount_);

            for (std::size_t I = 0; I < Current.size(); ++I) {
                AttachedParent_->InsertChildAt(Base + I, std::move(Current[I]));
            }
            AttachedCount_ = Current.size();
        } else {
            ReconcileChildren(ListWidgets_, PreviousList_, NewOutput, Mount_);
        }
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
