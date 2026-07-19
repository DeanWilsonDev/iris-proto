#include "Iris/SemanticValidator.h"
#include "Iris/RenderBlockParser.h"

#include <cstdio>
#include <string>
#include <unordered_set>

extern int Failures; // defined in CppTokenizerTests.cpp

namespace {

void Expect(bool Condition, const std::string& Description) {
    if (Condition) {
        std::printf("[PASS] %s\n", Description.c_str());
    } else {
        std::printf("[FAIL] %s\n", Description.c_str());
        ++Failures;
    }
}

// Parses one render block and hands its root straight to the semantic validator —
// mirrors CodegenTests.cpp's Generate() helper.
std::vector<Iris::SemanticError> Validate(std::string_view Source, Iris::IrisBuildTarget Target,
                                           const std::unordered_set<std::string>& ImportedNames = {}) {
    Iris::RenderBlockParser Parser(Source, "test.iris");
    const auto              Parsed = Parser.Parse();
    if (!Parsed.Errors.empty() || Parsed.Blocks.empty()) {
        return {{"render block failed to parse", {}}};
    }
    return Iris::ValidateElementTree(Parsed.Blocks[0].Root, Target, ImportedNames);
}

bool Contains(const std::vector<Iris::SemanticError>& Errors, std::string_view Needle) {
    for (const auto& E : Errors) {
        if (E.Message.find(Needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void TestCorePrimitiveNeedsNoImport() {
    const auto Errors = Validate(R"(render { <Frame><Text>hi</Text></Frame> })", Iris::IrisBuildTarget::Penumbra);
    Expect(Errors.empty(), "Core primitives (Frame, Text) need no import and produce no errors");
}

void TestImportedComponentIsInScope() {
    const auto Errors = Validate(R"(render { <HealthBar current={1} max={2} label="HP" /> })",
                                  Iris::IrisBuildTarget::Penumbra, {"HealthBar"});
    Expect(Errors.empty(), "a tag matching an imported name produces no errors");
}

void TestUnimportedComponentReferenceIsAnError() {
    const auto Errors =
        Validate(R"(render { <HealthBar current={1} max={2} /> })", Iris::IrisBuildTarget::Penumbra, {});
    Expect(!Errors.empty(), "a tag that's neither a Core primitive nor imported is an error");
    Expect(Contains(Errors, "HealthBar` is not imported and is not a Core primitive"),
           "the error message matches the spec §6 catalogue's wording");
}

void TestModel3dOnPenumbraTargetIsAnError() {
    const auto Errors = Validate(R"(render { <Model3d/> })", Iris::IrisBuildTarget::Penumbra);
    Expect(!Errors.empty(), "<Model3d> on a penumbra target is an error");
    Expect(Contains(Errors, "requires backend umbra-engine") && Contains(Errors, "project target is penumbra"),
           "the error names the required backend and the actual target");
}

void TestModel3dOnUmbraEngineTargetIsFine() {
    const auto Errors = Validate(R"(render { <Model3d/> })", Iris::IrisBuildTarget::UmbraEngine);
    Expect(Errors.empty(), "<Model3d> on an umbra-engine target produces no errors");
}

void TestInlineStyleOnPrimitiveIsAnError() {
    const auto Errors = Validate(R"(render { <Frame style="background: red;" /> })", Iris::IrisBuildTarget::Penumbra);
    Expect(!Errors.empty(), "a style prop on a Core primitive is an error");
    Expect(Contains(Errors, "Inline styles are not permitted"), "the error uses the spec's exact wording");
}

void TestInlineStyleOnComponentInvocationIsAnError() {
    // Codegen.h never validates a component invocation's prop names at all (they pass
    // straight through to `<Name>Props`'s designated initializers), so this case is
    // only caught here.
    const auto Errors =
        Validate(R"(render { <HealthBar style="background: red;" /> })", Iris::IrisBuildTarget::Penumbra,
                 {"HealthBar"});
    Expect(!Errors.empty(), "a style prop on an imported component invocation is also an error");
    Expect(Contains(Errors, "Inline styles are not permitted"),
           "the check isn't scoped to Core primitives only");
}

void TestTextFontPropIsAnError() {
    const auto Errors = Validate(R"(render { <Text font="Arial">Hello</Text> })", Iris::IrisBuildTarget::Penumbra);
    Expect(!Errors.empty(), "a font prop on <Text> is an error");
    Expect(Contains(Errors, "`<Text>` has no `font` prop"), "the error uses the spec's exact wording");
}

void TestFontPropOnNonTextElementIsNotFontChecked() {
    // <Frame font="..."> isn't validated by the font-specific check (that's <Text>-only)
    // — it's still an error, but via Codegen's own unknown-prop-name check, not this pass.
    const auto Errors = Validate(R"(render { <Frame font="Arial" /> })", Iris::IrisBuildTarget::Penumbra);
    Expect(!Contains(Errors, "<Text> has no `font` prop"),
           "the <Text>-specific font message is not emitted for a non-<Text> element");
}

void TestUnimportedTagInsideJsxTransformEscapeHatchIsStillCaught() {
    // A `!{ }` JSX-transform escape hatch's nested elements are real parsed elements
    // (docs/iris_escape_hatch_decision.md) and must get the same validation as anything
    // written directly in the tree — this is what distinguishes this pass from one that
    // only walks Children/Props at the top level.
    const auto Errors = Validate(
        R"(render {
            <Slot>
                !{[&]() -> IrisComponent {
                    return <NotImported/>;
                }}
            </Slot>
        })",
        Iris::IrisBuildTarget::Penumbra, {});
    Expect(!Errors.empty(), "an unimported tag nested inside a !{ } escape hatch is still an error");
    Expect(Contains(Errors, "NotImported` is not imported and is not a Core primitive"),
           "the nested element is the one flagged, not the outer <Slot>");
}

void TestPlainEscapeHatchContentsAreNeverInspected() {
    // A regular `{ }` escape hatch is opaque (docs/iris_core_spec.md §1.4) — a
    // JSX-looking string inside it must never be parsed or validated, matching
    // RenderBlockParser's own TestEscapeHatchContainingAngleBracketsIsOpaque.
    const auto Errors = Validate(
        R"(render {
            <Slot>
                {[&]() -> IrisComponent {
                    return <NotImported/>;
                }}
            </Slot>
        })",
        Iris::IrisBuildTarget::Penumbra, {});
    Expect(Errors.empty(), "a JSX-looking string inside an opaque { } escape hatch is never validated");
}

void TestPartyScreenWithCorrectImportsValidatesCleanly() {
    const auto Errors = Validate(R"(render {
        <Frame class="party-screen">
            <Button label="Details" onPress={[&]() { detailsOpen.set(true); }} />
            <Slot>
                !{[&]() -> IrisComponent {
                    if (!detailsOpen.get()) return nullptr;
                    return <Frame class="details-panel">
                        <Slot>
                            !{[&]() -> std::vector<IrisComponent> {
                                std::vector<IrisComponent> rows;
                                for (auto& member : props.members) {
                                    rows.push_back(
                                        <Frame key={member.id} class="party-row">
                                            <HealthBar current={member.hp} max={member.maxHp} label={member.name} />
                                        </Frame>
                                    );
                                }
                                return rows;
                            }}
                        </Slot>
                    </Frame>;
                }}
            </Slot>
        </Frame>
    })",
                                  Iris::IrisBuildTarget::Penumbra, {"Button", "HealthBar"});
    Expect(Errors.empty(),
           "the full spec §9 PartyScreen example, with Button/HealthBar imported, validates with no errors");
}

} // namespace

void RunSemanticValidatorTests() {
    TestCorePrimitiveNeedsNoImport();
    TestImportedComponentIsInScope();
    TestUnimportedComponentReferenceIsAnError();
    TestModel3dOnPenumbraTargetIsAnError();
    TestModel3dOnUmbraEngineTargetIsFine();
    TestInlineStyleOnPrimitiveIsAnError();
    TestInlineStyleOnComponentInvocationIsAnError();
    TestTextFontPropIsAnError();
    TestFontPropOnNonTextElementIsNotFontChecked();
    TestUnimportedTagInsideJsxTransformEscapeHatchIsStillCaught();
    TestPlainEscapeHatchContentsAreNeverInspected();
    TestPartyScreenWithCorrectImportsValidatesCleanly();
}
