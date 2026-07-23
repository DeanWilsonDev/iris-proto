#include "Iris/SemanticValidator.h"
#include "Iris/CorePrimitives.h"

namespace Iris {

namespace {

class Validator {
public:
    Validator(IrisBuildTarget Target, const std::unordered_set<std::string>& ImportedNames,
              std::vector<SemanticError>& Errors)
        : Target_(Target), ImportedNames_(ImportedNames), Errors_(Errors) {}

    void Validate(const ElementNode& Node) {
        ValidateTagIsInScope(Node);
        ValidateNoInlineStyle(Node);
        if (Node.Tag == "Text") {
            ValidateTextHasNoFontProp(Node);
        }

        if (Node.Key.has_value()) {
            ValidatePropValue(*Node.Key);
        }
        if (Node.Ref.has_value()) {
            ValidatePropValue(*Node.Ref);
        }
        for (const Prop& P : Node.Props) {
            ValidatePropValue(P.Value);
        }
        for (const ElementChild& Child : Node.Children) {
            if (Child.Kind == ElementChildKind::Element) {
                Validate(*Child.Element);
            } else if (Child.Kind == ElementChildKind::EscapeHatch) {
                ValidatePropValue(*Child.EscapeHatch);
            }
        }
    }

private:
    void AddError(std::string Message, SourceLocation Location) {
        Errors_.push_back(SemanticError{std::move(Message), std::move(Location)});
    }

    // A `!{ }` JSX-transform escape hatch's body can contain nested elements
    // (docs/iris_escape_hatch_decision.md) — those get the same validation as any
    // element written directly in the tree. A plain `{ }` escape hatch has no
    // segments to walk; nothing to do for it here.
    void ValidatePropValue(const PropValue& Value) {
        if (Value.Kind != PropValueKind::JsxEscapeHatch) {
            return;
        }
        for (const JsxSegment& Segment : Value.JsxSegments) {
            if (Segment.Kind == JsxSegmentKind::Element) {
                Validate(*Segment.Element);
            }
        }
    }

    void ValidateTagIsInScope(const ElementNode& Node) {
        if (CorePrimitiveTagNames().count(Node.Tag) != 0) {
            return;
        }

        const auto GatedIt = BackendGatedPrimitiveTagNames().find(Node.Tag);
        if (GatedIt != BackendGatedPrimitiveTagNames().end()) {
            if (Target_ != GatedIt->second) {
                AddError("`<" + Node.Tag + ">` requires backend " + BackendName(GatedIt->second) +
                              "; project target is " + BackendName(Target_) + ".",
                          Node.Location);
            }
            return;
        }

        if (ImportedNames_.count(Node.Tag) != 0) {
            return;
        }

        AddError("`" + Node.Tag + "` is not imported and is not a Core primitive.", Node.Location);
    }

    // Inline styles are never permitted, on any element — Core primitive or
    // component invocation alike (docs/iris_core_spec.md §4). This is the one
    // check here that isn't scoped to Core primitives: Codegen.h's per-primitive
    // prop tables happen to reject an unrecognised `style` prop on a primitive
    // too, but with a generic "unknown prop" message, and never checks component
    // invocations' props at all — this is what supplies the catalogue's specific
    // message and closes that gap.
    void ValidateNoInlineStyle(const ElementNode& Node) {
        for (const Prop& P : Node.Props) {
            if (P.Name == "style") {
                AddError("Inline styles are not permitted. Use `class` and define styling in a `.lustre` file.",
                          P.Value.Location);
            }
        }
    }

    void ValidateTextHasNoFontProp(const ElementNode& Node) {
        for (const Prop& P : Node.Props) {
            if (P.Name == "font") {
                AddError("`<Text>` has no `font` prop; specify font via a Lustre class.", P.Value.Location);
            }
        }
    }

    static std::string BackendName(IrisBuildTarget Target) {
        return Target == IrisBuildTarget::UmbraEngine ? "umbra-engine" : "penumbra";
    }

    IrisBuildTarget                         Target_;
    const std::unordered_set<std::string>&  ImportedNames_;
    std::vector<SemanticError>&             Errors_;
};

} // namespace

std::vector<SemanticError> ValidateElementTree(const ElementNode& Root, IrisBuildTarget Target,
                                                const std::unordered_set<std::string>& ImportedNames) {
    std::vector<SemanticError> Errors;
    Validator                   V(Target, ImportedNames, Errors);
    V.Validate(Root);
    return Errors;
}

} // namespace Iris
