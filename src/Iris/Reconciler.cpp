#include "Iris/Reconciler.h"

#include <algorithm>
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

// Same "no operator==, always changed whenever New has one" treatment as the
// zero-argument event props above, for <Input>'s value-carrying onTextChange.
void DiffTextEventField(std::optional<std::function<void(std::string)>>& Field, const Iris::IrisProps& New,
                         const std::string& Name) {
    if (const auto* NewValue = GetProp<std::function<void(std::string)>>(New, Name)) {
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
            if constexpr (std::is_same_v<T, std::function<void()>> ||
                          std::is_same_v<T, std::function<void(std::string)>> ||
                          std::is_same_v<T, iris::TextureHandle>) {
                return false; // never meaningfully comparable — never a key match
            } else {
                return AValue == std::get<T>(*B);
            }
        },
        *A);
}

// Matches `OldList` against `NewList`: keyed entries first (position-independent, so
// an item that moved is still recognised as the same item), then remaining unkeyed
// entries by relative order among what's left. Shared by both the plain-vector
// (`ReconcileList`) and live-widget (`ReconcileChildrenAt`) reconciliation paths so
// they can never drift out of sync on what counts as a match.
struct ListMatch {
    std::vector<bool> OldMatched;
    std::vector<int>  NewToOld; // -1 == no match, needs a fresh mount
};

ListMatch MatchLists(const std::vector<Iris::Component>& OldList,
                      const std::vector<Iris::Component>& NewList) {
    ListMatch Match;
    Match.OldMatched.assign(OldList.size(), false);
    Match.NewToOld.assign(NewList.size(), -1);

    // Pass 1: match by explicit key (docs/iris_stage3_decision_doc.md §3).
    for (std::size_t NewIndex = 0; NewIndex < NewList.size(); ++NewIndex) {
        if (!NewList[NewIndex].Key.has_value()) {
            continue;
        }
        for (std::size_t OldIndex = 0; OldIndex < OldList.size(); ++OldIndex) {
            if (Match.OldMatched[OldIndex] || !OldList[OldIndex].Key.has_value()) {
                continue;
            }
            if (OldList[OldIndex].Tag == NewList[NewIndex].Tag &&
                KeysEqual(OldList[OldIndex].Key, NewList[NewIndex].Key)) {
                Match.NewToOld[NewIndex] = static_cast<int>(OldIndex);
                Match.OldMatched[OldIndex] = true;
                break;
            }
        }
    }

    // Pass 2: remaining unkeyed entries matched by relative order among what's left.
    std::size_t OldCursor = 0;
    for (std::size_t NewIndex = 0; NewIndex < NewList.size(); ++NewIndex) {
        if (Match.NewToOld[NewIndex] != -1 || NewList[NewIndex].Key.has_value()) {
            continue;
        }
        while (OldCursor < OldList.size() && (Match.OldMatched[OldCursor] || OldList[OldCursor].Key.has_value())) {
            ++OldCursor;
        }
        if (OldCursor < OldList.size() && OldList[OldCursor].Tag == NewList[NewIndex].Tag) {
            Match.NewToOld[NewIndex] = static_cast<int>(OldCursor);
            Match.OldMatched[OldCursor] = true;
            ++OldCursor;
        }
    }

    return Match;
}

// Longest increasing subsequence, by value, of the matched entries in `NewToOld`
// (unmatched `-1` entries are ignored). Returns a mask over *new* positions: `true`
// marks a position whose matched old widget is already in the correct relative order
// and needs no structural move at all — only these positions' `RemoveChildAt`/
// `InsertChildAt` pair is skippable; everything else needs exactly one such pair
// (moved) or one `InsertChildAt` (freshly mounted). Standard patience-sorting LIS,
// O(n log n).
std::vector<bool> ComputeKeepInPlace(const std::vector<int>& NewToOld) {
    std::vector<std::size_t> MatchedPositions; // positions (indices into NewToOld) that matched something
    for (std::size_t Index = 0; Index < NewToOld.size(); ++Index) {
        if (NewToOld[Index] != -1) {
            MatchedPositions.push_back(Index);
        }
    }

    std::vector<int>         TailValues;   // OldIndex values, kept sorted increasing
    std::vector<std::size_t> TailAt;       // index into MatchedPositions for each tail
    std::vector<int>         Predecessor(MatchedPositions.size(), -1);

    for (std::size_t I = 0; I < MatchedPositions.size(); ++I) {
        const int Value = NewToOld[MatchedPositions[I]];
        const auto It   = std::lower_bound(TailValues.begin(), TailValues.end(), Value);
        const auto Pos  = static_cast<std::size_t>(It - TailValues.begin());
        Predecessor[I]  = (Pos > 0) ? static_cast<int>(TailAt[Pos - 1]) : -1;
        if (Pos == TailValues.size()) {
            TailValues.push_back(Value);
            TailAt.push_back(I);
        } else {
            TailValues[Pos] = Value;
            TailAt[Pos]     = I;
        }
    }

    std::vector<bool> Keep(NewToOld.size(), false);
    if (!TailAt.empty()) {
        int Cursor = static_cast<int>(TailAt.back());
        while (Cursor != -1) {
            Keep[MatchedPositions[static_cast<std::size_t>(Cursor)]] = true;
            Cursor = Predecessor[static_cast<std::size_t>(Cursor)];
        }
    }
    return Keep;
}

std::vector<std::unique_ptr<Umbra::IWidget>> ReconcileList(std::vector<std::unique_ptr<Umbra::IWidget>> OldWidgets,
                                                             const std::vector<Iris::Component>& OldList,
                                                             const std::vector<Iris::Component>& NewList,
                                                             const MountFn& Mount) {
    const ListMatch Match = MatchLists(OldList, NewList);

    std::vector<std::unique_ptr<Umbra::IWidget>> Result;
    Result.reserve(NewList.size());
    for (std::size_t NewIndex = 0; NewIndex < NewList.size(); ++NewIndex) {
        if (Match.NewToOld[NewIndex] != -1) {
            std::unique_ptr<Umbra::IWidget> Reused =
                std::move(OldWidgets[static_cast<std::size_t>(Match.NewToOld[NewIndex])]);
            ReconcileWidget(Reused, OldList[static_cast<std::size_t>(Match.NewToOld[NewIndex])], NewList[NewIndex],
                             Mount);
            Result.push_back(std::move(Reused));
        } else {
            std::unique_ptr<Umbra::IWidget> Fresh;
            ReconcileWidget(Fresh, Iris::Component(nullptr), NewList[NewIndex], Mount);
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
// 1:1 alignment between an `Component` list and its matched widget's real children. A
// nested `<Slot>` found this way gets its own `SlotState` entirely separately
// (docs/iris_nested_slot_discovery_decision.md's `SlotState::NestedSlots_`), never through
// this list-diffing path.
std::vector<Iris::Component> FilterOrdinary(const std::vector<Iris::Component>& Children) {
    std::vector<Iris::Component> Ordinary;
    for (const Iris::Component& Child : Children) {
        if (Child.Tag != Iris::IrisElementTag::Slot) {
            Ordinary.push_back(Child);
        }
    }
    return Ordinary;
}

// Updates `Widget` in place for a matched pair (same tag, same key — guaranteed by
// every caller, never re-checked here) — applies the prop diff and recurses into
// children via `ReconcileChildrenAt`. Never replaces or destroys `Widget` itself,
// which is exactly why callers that already know they have a match (a kept-in-place
// LIS entry, or a matched-but-moved entry about to be reinserted) can call this
// through a non-owning reference instead of `ReconcileWidget`'s `unique_ptr&`.
void ReconcileMatchedInPlace(Umbra::IWidget& Widget, const Iris::Component& Old, const Iris::Component& New,
                             const MountFn& Mount) {
    Widget.ApplyPropDiff(ComputePropDiff(Old.Props, New.Props));
    ReconcileChildrenAt(Widget, 0, FilterOrdinary(Old.Children), FilterOrdinary(New.Children), Mount);
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
    DiffTextEventField(Diff.OnTextChange, New, "onTextChange");
    return Diff;
}

void ReconcileWidget(std::unique_ptr<Umbra::IWidget>& Widget, const Iris::Component& Old,
                      const Iris::Component& New, const MountFn& Mount) {
    if (New.Tag == Iris::IrisElementTag::None) {
        Widget.reset();
        return;
    }

    const bool SameIdentity = Old.Tag == New.Tag && KeysEqual(Old.Key, New.Key);
    if (!SameIdentity) {
        Widget = Mount(New);
        return;
    }

    ReconcileMatchedInPlace(*Widget, Old, New, Mount);
}

void ReconcileChildren(std::vector<std::unique_ptr<Umbra::IWidget>>& Widgets,
                        const std::vector<Iris::Component>& OldList,
                        const std::vector<Iris::Component>& NewList, const MountFn& Mount) {
    Widgets = ReconcileList(std::move(Widgets), OldList, NewList, Mount);
}

void ReconcileChildrenAt(Umbra::IWidget& Parent, std::size_t Base,
                          const std::vector<Iris::Component>& OldList,
                          const std::vector<Iris::Component>& NewList, const MountFn& Mount) {
    const ListMatch         Match = MatchLists(OldList, NewList);
    const std::vector<bool> Keep  = ComputeKeepInPlace(Match.NewToOld);

    // Which old index (if any) each new position keeps in place — used below to
    // decide, per old index, whether it survives untouched or must be removed.
    std::vector<int> KeptOldIndex(OldList.size(), -1);
    for (std::size_t NewIndex = 0; NewIndex < NewList.size(); ++NewIndex) {
        if (Keep[NewIndex]) {
            KeptOldIndex[static_cast<std::size_t>(Match.NewToOld[NewIndex])] = static_cast<int>(NewIndex);
        }
    }

    // Remove every old child not staying in place — highest original index first, so
    // an as-yet-unremoved lower index's absolute position (Base + OldIndex) never
    // shifts out from under it (RemoveChildAt only ever moves indices *above* the one
    // removed). Matched-but-moved entries are kept alive here for reinsertion below;
    // unmatched (dropped) entries destruct immediately — correct unmount.
    std::vector<std::unique_ptr<Umbra::IWidget>> MovedOld(OldList.size());
    for (std::size_t Reverse = OldList.size(); Reverse-- > 0;) {
        if (KeptOldIndex[Reverse] != -1) {
            continue; // stays exactly where it is
        }
        std::unique_ptr<Umbra::IWidget> Removed = Parent.RemoveChildAt(Base + Reverse);
        if (Match.OldMatched[Reverse]) {
            MovedOld[Reverse] = std::move(Removed);
        }
    }

    // Walk left to right inserting/updating at each target index. By induction,
    // Parent's children [Base, Base + NewIndex) already equal NewList[0, NewIndex)
    // before this iteration: a kept entry is already sitting at Base + NewIndex (kept
    // entries preserve both their relative order — the LIS property — and, since
    // nothing is ever inserted before an already-correct prefix, their absolute
    // position too); anything else gets inserted exactly once, right here.
    for (std::size_t NewIndex = 0; NewIndex < NewList.size(); ++NewIndex) {
        if (Keep[NewIndex]) {
            Umbra::IWidget* InPlace = Parent.GetChildAt(Base + NewIndex);
            ReconcileMatchedInPlace(*InPlace, OldList[static_cast<std::size_t>(Match.NewToOld[NewIndex])],
                                     NewList[NewIndex], Mount);
            continue;
        }
        if (Match.NewToOld[NewIndex] != -1) {
            std::unique_ptr<Umbra::IWidget>& Reused = MovedOld[static_cast<std::size_t>(Match.NewToOld[NewIndex])];
            ReconcileMatchedInPlace(*Reused, OldList[static_cast<std::size_t>(Match.NewToOld[NewIndex])],
                                     NewList[NewIndex], Mount);
            Parent.InsertChildAt(Base + NewIndex, std::move(Reused));
        } else {
            Parent.InsertChildAt(Base + NewIndex, Mount(NewList[NewIndex]));
        }
    }
}

} // namespace iris
