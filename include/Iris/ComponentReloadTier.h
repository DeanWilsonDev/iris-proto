#pragma once

namespace iris {

// The result of ComponentInstance::EndReloadReplay() — see
// docs/iris_hot_reload_reconciliation_decision.md §1/§3. Kept in its own header, rather
// than nested inside ComponentInstance or defined directly in Component.h, because
// Component.h needs this type by value (inside std::optional<>, which requires a
// complete type) but can't include ComponentInstance.h itself — ComponentInstance.h
// already transitively includes Component.h (via Signal.h -> SlotRuntime.h), so the
// reverse include would be circular.
enum class ComponentReloadTier {
    Unchanged,           // tier 1: render-body-only change, every @signal preserved
    SignalLayoutChanged, // tier 2: an @signal was added, removed, or changed type
};

} // namespace iris
