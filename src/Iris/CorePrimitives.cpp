#include "Iris/CorePrimitives.h"

namespace Iris {

const std::unordered_set<std::string>& CorePrimitiveTagNames() {
    static const std::unordered_set<std::string> Names = {"Frame",  "Inline", "Grid", "Image", "Icon",
                                                            "Text",  "Scroll", "Input", "Slot"};
    return Names;
}

const std::unordered_map<std::string, IrisBuildTarget>& BackendGatedPrimitiveTagNames() {
    static const std::unordered_map<std::string, IrisBuildTarget> Names = {
        {"Model3d", IrisBuildTarget::UmbraEngine},
    };
    return Names;
}

const std::unordered_map<std::string, std::string>& PrimitivePropTypeNames() {
    static const std::unordered_map<std::string, std::string> Types = {
        {"class", "std::string"},
        {"src", "std::string"},
        {"icon", "std::string"},
        {"size", "float"},
        {"wheelStep", "float"},
        // <Text>'s own content prop is synthesized by EmitTextPrimitive, bypassing this
        // table entirely -- this entry is only for <Input>'s initial-value prop, which
        // (unlike <Text>) is an ordinary attribute going through EmitPrimitiveProps like
        // any other prop, so it needs a real table entry to validate.
        {"text", "std::string"},
        {"preferredWidth", "float"},
        {"checked", "bool"},
        {"handle", "iris::TextureHandle"},
        {"onPress", "std::function<void()>"},
        {"onRelease", "std::function<void()>"},
        {"onHover", "std::function<void()>"},
        {"onFocus", "std::function<void()>"},
        {"onChange", "std::function<void()>"},
    };
    return Types;
}

} // namespace Iris
