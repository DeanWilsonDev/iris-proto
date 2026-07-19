#include "Iris/Reconciler.h"

#include <functional>
#include <type_traits>
#include <variant>

namespace iris {

namespace {

template <typename T>
const T* GetProp(const Iris::IrisProps& Props, const std::string& Name) {
    const auto It = Props.find(Name);
    if (It == Props.end()) {
        return nullptr;
    }
    return std::get_if<T>(&It->second);
}

// The generic case: T supports operator==, so an unchanged value is correctly left as
// std::nullopt ("don't touch it") rather than reapplied every reconcile.
template <typename T>
void DiffField(std::optional<T>& Field, const Iris::IrisProps& Old, const Iris::IrisProps& New,
               const std::string& Name) {
    const T* NewValue = GetProp<T>(New, Name);
    if (NewValue == nullptr) {
        return; // New doesn't have this prop at all — nothing to set (see Reconciler.h)
    }
    const T* OldValue = GetProp<T>(Old, Name);
    if (OldValue != nullptr && *OldValue == *NewValue) {
        return; // unchanged
    }
    Field = *NewValue;
}

// std::function<void()> has no operator== (only against nullptr) — always "changed"
// whenever New has one at all.
void DiffEventField(std::optional<std::function<void()>>& Field, const Iris::IrisProps& New,
                     const std::string& Name) {
    if (const auto* NewValue = GetProp<std::function<void()>>(New, Name)) {
        Field = *NewValue;
    }
}

// iris::TextureHandle (Umbra::TextureHandle) currently carries no data to compare —
// same treatment as the event-prop case above.
void DiffHandleField(std::optional<Umbra::TextureHandle>& Field, const Iris::IrisProps& New, const std::string& Name) {
    if (const auto* NewValue = GetProp<iris::TextureHandle>(New, Name)) {
        Field = *NewValue;
    }
}

bool KeysEqual(const std::optional<Iris::IrisPropValue>& A, const std::optional<Iris::IrisPropValue>& B) {
    if (A.has_value() != B.has_value()) {
        return false;
    }
    if (!A.has_value()) {
        return true; // neither side has a key
    }
    if (A->index() != B->index()) {
        return false;
    }
    return std::visit(
        [&](const auto& AValue) {
            using T = std::decay_t<decltype(AValue)>;
            if constexpr (std::is_same_v<T, std::function<void()>> || std::is_same_v<T, iris::TextureHandle>) {
                return false; // never meaningfully comparable — never a key match
            } else {
                return AValue == std::get<T>(*B);
            }
        },
        *A);
}

std::vector<std::unique_ptr<Umbra::IWidget>> ReconcileList(std::vector<std::unique_ptr<Umbra::IWidget>> OldWidgets,
                                                             const std::vector<Iris::IrisComponent>& OldList,
                                                             const std::vector<Iris::IrisComponent>& NewList,
                                                             const MountFn& Mount) {
    std::vector<bool> OldMatched(OldList.size(), false);
    std::vector<int>  NewToOld(NewList.size(), -1);

    // Pass 1: match by explicit key (docs/iris_stage3_decision_doc.md §3) — position-
    // independent, so an item that moved is still recognised as the same item.
    for (std::size_t NewIndex = 0; NewIndex < NewList.size(); ++NewIndex) {
        if (!NewList[NewIndex].Key.has_value()) {
            continue;
        }
        for (std::size_t OldIndex = 0; OldIndex < OldList.size(); ++OldIndex) {
            if (OldMatched[OldIndex] || !OldList[OldIndex].Key.has_value()) {
                continue;
            }
            if (OldList[OldIndex].Tag == NewList[NewIndex].Tag &&
                KeysEqual(OldList[OldIndex].Key, NewList[NewIndex].Key)) {
                NewToOld[NewIndex] = static_cast<int>(OldIndex);
                OldMatched[OldIndex] = true;
                break;
            }
        }
    }

    // Pass 2: remaining unkeyed entries matched by relative order among what's left —
    // the documented, simpler fallback (Reconciler.h's "Known limitation").
    std::size_t OldCursor = 0;
    for (std::size_t NewIndex = 0; NewIndex < NewList.size(); ++NewIndex) {
        if (NewToOld[NewIndex] != -1 || NewList[NewIndex].Key.has_value()) {
            continue;
        }
        while (OldCursor < OldList.size() && (OldMatched[OldCursor] || OldList[OldCursor].Key.has_value())) {
            ++OldCursor;
        }
        if (OldCursor < OldList.size() && OldList[OldCursor].Tag == NewList[NewIndex].Tag) {
            NewToOld[NewIndex] = static_cast<int>(OldCursor);
            OldMatched[OldCursor] = true;
            ++OldCursor;
        }
    }

    std::vector<std::unique_ptr<Umbra::IWidget>> Result;
    Result.reserve(NewList.size());
    for (std::size_t NewIndex = 0; NewIndex < NewList.size(); ++NewIndex) {
        if (NewToOld[NewIndex] != -1) {
            std::unique_ptr<Umbra::IWidget> Reused = std::move(OldWidgets[static_cast<std::size_t>(NewToOld[NewIndex])]);
            ReconcileWidget(Reused, OldList[static_cast<std::size_t>(NewToOld[NewIndex])], NewList[NewIndex], Mount);
            Result.push_back(std::move(Reused));
        } else {
            std::unique_ptr<Umbra::IWidget> Fresh;
            ReconcileWidget(Fresh, Iris::IrisComponent(nullptr), NewList[NewIndex], Mount);
            Result.push_back(std::move(Fresh));
        }
    }
    // Unmatched entries in OldWidgets were never moved-from and destruct here,
    // releasing whatever real widget they wrapped — correct unmount.
    return Result;
}

// A `Slot`-tagged child contributes zero real widgets — same convention `BuildWidgetTree`/
// `ResolveSlotsRecursive` already apply to the static tree (docs/iris_slot_stage2_wiring_
// decision.md) — so it must never reach `ReconcileList`'s own tag+key matching, which assumes
// 1:1 alignment between an `IrisComponent` list and its matched widget's real children. A
// nested `<Slot>` found this way gets its own `SlotState` entirely separately
// (docs/iris_nested_slot_discovery_decision.md's `SlotState::NestedSlots_`), never through
// this list-diffing path.
std::vector<Iris::IrisComponent> FilterOrdinary(const std::vector<Iris::IrisComponent>& Children) {
    std::vector<Iris::IrisComponent> Ordinary;
    for (const Iris::IrisComponent& Child : Children) {
        if (Child.Tag != Iris::IrisElementTag::Slot) {
            Ordinary.push_back(Child);
        }
    }
    return Ordinary;
}

} // namespace

Umbra::IrisPropDiff ComputePropDiff(const Iris::IrisProps& Old, const Iris::IrisProps& New) {
    Umbra::IrisPropDiff Diff;
    DiffField(Diff.ClassName, Old, New, "class");
    DiffField(Diff.Text, Old, New, "text");
    DiffField(Diff.Src, Old, New, "src");
    DiffField(Diff.Checked, Old, New, "checked");
    DiffHandleField(Diff.Handle, New, "handle");
    DiffEventField(Diff.OnPress, New, "onPress");
    DiffEventField(Diff.OnRelease, New, "onRelease");
    DiffEventField(Diff.OnHover, New, "onHover");
    DiffEventField(Diff.OnFocus, New, "onFocus");
    DiffEventField(Diff.OnChange, New, "onChange");
    return Diff;
}

void ReconcileWidget(std::unique_ptr<Umbra::IWidget>& Widget, const Iris::IrisComponent& Old,
                      const Iris::IrisComponent& New, const MountFn& Mount) {
    if (New.Tag == Iris::IrisElementTag::None) {
        Widget.reset();
        return;
    }

    const bool SameIdentity = Old.Tag == New.Tag && KeysEqual(Old.Key, New.Key);
    if (!SameIdentity) {
        Widget = Mount(New);
        return;
    }

    Widget->ApplyPropDiff(ComputePropDiff(Old.Props, New.Props));

    const std::vector<Iris::IrisComponent> OldOrdinary = FilterOrdinary(Old.Children);
    const std::vector<Iris::IrisComponent> NewOrdinary = FilterOrdinary(New.Children);

    std::vector<std::unique_ptr<Umbra::IWidget>> OldChildren;
    const std::size_t                             ChildCount = Widget->GetChildCount();
    OldChildren.reserve(ChildCount);
    for (std::size_t Index = 0; Index < ChildCount; ++Index) {
        OldChildren.push_back(Widget->RemoveChildAt(0));
    }
    std::vector<std::unique_ptr<Umbra::IWidget>> NewChildren =
        ReconcileList(std::move(OldChildren), OldOrdinary, NewOrdinary, Mount);
    for (std::size_t Index = 0; Index < NewChildren.size(); ++Index) {
        Widget->InsertChildAt(Index, std::move(NewChildren[Index]));
    }
}

void ReconcileChildren(std::vector<std::unique_ptr<Umbra::IWidget>>& Widgets,
                        const std::vector<Iris::IrisComponent>& OldList,
                        const std::vector<Iris::IrisComponent>& NewList, const MountFn& Mount) {
    Widgets = ReconcileList(std::move(Widgets), OldList, NewList, Mount);
}

} // namespace iris
