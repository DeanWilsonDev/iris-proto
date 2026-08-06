#include "Iris/ReloadTarget.h"
#include "Iris/Reconciler.h"

#include <utility>

namespace iris {

ReloadTarget::ReloadTarget(std::unique_ptr<Umbra::IWidget> RootWidget, Iris::Component RootTree)
    : RootWidget_(std::move(RootWidget)), PreviousTree_(std::move(RootTree)) {}

const Iris::Component& ReloadTarget::PreviousTree() const { return PreviousTree_; }

Umbra::IWidget* ReloadTarget::RootWidget() const { return RootWidget_.get(); }

void ReloadTarget::Reconcile(Iris::Component New, const MountFn& Mount) {
    ReconcileWidget(RootWidget_, PreviousTree_, New, Mount);
    PreviousTree_ = std::move(New);
}

} // namespace iris
