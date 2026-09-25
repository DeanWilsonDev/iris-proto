#pragma once

#include "Iris/IrisProps.h"

#include "Umbra/IWidget.h"

#include <functional>

namespace Iris {

struct PortalProperties {
    bool                  HasPlacement{false};
    float                 X{0.0f};
    float                 Y{0.0f};
    float                 Width{0.0f};
    float                 Height{0.0f};
    bool                  DismissOnOutsideClick{false};
    std::function<void()> OnDismiss;
};

PortalProperties ReadPortalProperties(const IrisProps& Props);

// Interface implemented by the live IWidget adapter a backend creates for a
// <Portal>. Iris keeps the portal's child in ordinary Component::Children and reconciles it
// through IWidget's normal child API; this interface only carries portal-specific state and
// makes teardown ordering explicit.
//
// PreparePortalUnmount must be idempotent. It is called before Iris destroys or replaces the
// adapter, so an implementation can destroy any SlotState owners before destroying detached
// child content. A backend-triggered synchronous dismissal first copies the latest OnDismiss
// callback supplied through ApplyPortalProperties, then follows that same order and invokes
// the copy. The callback may synchronously reconcile and destroy the adapter that stored it.
class IPortalTarget {
public:
    virtual void ApplyPortalProperties(const PortalProperties& Properties) = 0;
    virtual void PreparePortalUnmount() = 0;
    virtual ~IPortalTarget() = default;
};

// Prepares every portal in a live widget subtree, deepest first. Reconciliation and
// SlotState teardown call this before releasing widgets. A backend performing synchronous
// dismissal must copy the latest OnDismiss callback, call this on the portal adapter, then
// invoke the copy.
void PreparePortalSubtreeForUnmount(Umbra::IWidget* Root);

} // namespace Iris
