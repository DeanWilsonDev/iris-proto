#include "Iris/CorePrimitives.h"

namespace Iris {

const std::unordered_set<std::string>& CorePrimitiveTagNames() {
    static const std::unordered_set<std::string> Names = {"Frame", "Inline", "Grid", "Image", "Text", "Slot"};
    return Names;
}

const std::unordered_map<std::string, IrisBuildTarget>& BackendGatedPrimitiveTagNames() {
    static const std::unordered_map<std::string, IrisBuildTarget> Names = {
        {"Model3d", IrisBuildTarget::UmbraEngine},
    };
    return Names;
}

} // namespace Iris
