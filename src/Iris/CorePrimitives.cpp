#include "Iris/CorePrimitives.h"

namespace Iris {

const std::unordered_set<std::string>& CorePrimitiveTagNames() {
    static const std::unordered_set<std::string> Names = {"Frame", "Inline", "Grid",   "Image", "Icon",  "Text",
                                                            "Scroll", "Input", "Slot", "Native", "Split"};
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
        // <Split>'s own dedicated props (docs/archive/iris_next_steps_resolved.md, "No
        // layout-container primitive beyond Frame's three stack modes"), mirroring
        // Penumbra::Widgets::SplitPanel's real fields (Axis/SplitRatio/
        // HandleThicknessLogical/MinPaneSizeLogical). `axis` is a plain string
        // ("horizontal"|"vertical") rather than a new bool/enum IrisPropValue kind --
        // resolving it to SplitPanel's own SplitAxis enum is a backend-mapping concern,
        // same treatment `icon`'s catalog-key string already gets.
        {"axis", "std::string"},
        {"ratio", "float"},
        {"minPaneSize", "float"},
        {"handleThickness", "float"},
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
        // <Input>'s own value-carrying event prop (docs/archive/iris_next_steps_resolved.md,
        // "<Input>'s onChange can't carry the new text") -- distinct from the shared
        // zero-argument `onChange` above so every other primitive's event-prop shape stays
        // unchanged.
        {"onTextChange", "std::function<void(std::string)>"},
    };
    return Types;
}

} // namespace Iris
