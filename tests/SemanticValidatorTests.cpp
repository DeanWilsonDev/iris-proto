#include "cimmerian/test.hpp"

#include "Iris/SemanticValidator.h"
#include "Iris/RenderBlockParser.h"

#include <unordered_set>

namespace {

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

} // namespace

DESCRIBE("SemanticValidator", {
    IT("a Core primitive needs no import", {
        const auto Errors = Validate(R"(render { <Frame><Text>hi</Text></Frame> })", Iris::IrisBuildTarget::Penumbra);
        ASSERT_TRUE(Errors.empty()); // Core primitives (Frame, Text) need no import and produce no errors
    });

    IT("an imported component is in scope", {
        const auto Errors = Validate(R"(render { <HealthBar current={1} max={2} label="HP" /> })",
                                      Iris::IrisBuildTarget::Penumbra, {"HealthBar"});
        ASSERT_TRUE(Errors.empty()); // a tag matching an imported name produces no errors
    });

    IT("an unimported component reference is an error", {
        const auto Errors =
            Validate(R"(render { <HealthBar current={1} max={2} /> })", Iris::IrisBuildTarget::Penumbra, {});
        ASSERT_FALSE(Errors.empty()); // a tag that's neither a Core primitive nor imported is an error
        ASSERT_TRUE(Contains(Errors, "HealthBar` is not imported and is not a Core primitive"));
        // the error message matches the spec §6 catalogue's wording
    });

    IT("Model3d on the penumbra target is an error", {
        const auto Errors = Validate(R"(render { <Model3d/> })", Iris::IrisBuildTarget::Penumbra);
        ASSERT_FALSE(Errors.empty()); // <Model3d> on a penumbra target is an error
        ASSERT_TRUE(Contains(Errors, "requires backend umbra-engine") && Contains(Errors, "project target is penumbra"));
        // the error names the required backend and the actual target
    });

    IT("Model3d on the umbra-engine target is fine", {
        const auto Errors = Validate(R"(render { <Model3d/> })", Iris::IrisBuildTarget::UmbraEngine);
        ASSERT_TRUE(Errors.empty()); // <Model3d> on an umbra-engine target produces no errors
    });

    IT("an inline style on a primitive is an error", {
        const auto Errors = Validate(R"(render { <Frame style="background: red;" /> })", Iris::IrisBuildTarget::Penumbra);
        ASSERT_FALSE(Errors.empty()); // a style prop on a Core primitive is an error
        ASSERT_TRUE(Contains(Errors, "Inline styles are not permitted")); // the error uses the spec's exact wording
    });

    IT("an inline style on a component invocation is also an error", {
        // Codegen.h never validates a component invocation's prop names at all (they pass
        // straight through to `<Name>Props`'s designated initializers), so this case is
        // only caught here.
        const auto Errors =
            Validate(R"(render { <HealthBar style="background: red;" /> })", Iris::IrisBuildTarget::Penumbra,
                     {"HealthBar"});
        ASSERT_FALSE(Errors.empty()); // a style prop on an imported component invocation is also an error
        ASSERT_TRUE(Contains(Errors, "Inline styles are not permitted")); // the check isn't scoped to Core primitives only
    });

    IT("a font prop on Text is an error", {
        const auto Errors = Validate(R"(render { <Text font="Arial">Hello</Text> })", Iris::IrisBuildTarget::Penumbra);
        ASSERT_FALSE(Errors.empty()); // a font prop on <Text> is an error
        ASSERT_TRUE(Contains(Errors, "`<Text>` has no `font` prop")); // the error uses the spec's exact wording
    });

    IT("a font prop on a non-Text element is not font-checked", {
        // <Frame font="..."> isn't validated by the font-specific check (that's <Text>-only)
        // — it's still an error, but via Codegen's own unknown-prop-name check, not this pass.
        const auto Errors = Validate(R"(render { <Frame font="Arial" /> })", Iris::IrisBuildTarget::Penumbra);
        ASSERT_FALSE(Contains(Errors, "<Text> has no `font` prop"));
        // the <Text>-specific font message is not emitted for a non-<Text> element
    });

    IT("an unimported tag inside a !{ } JSX-transform escape hatch is still caught", {
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
        ASSERT_FALSE(Errors.empty()); // an unimported tag nested inside a !{ } escape hatch is still an error
        ASSERT_TRUE(Contains(Errors, "NotImported` is not imported and is not a Core primitive"));
        // the nested element is the one flagged, not the outer <Slot>
    });

    IT("plain escape hatch contents are never inspected", {
        // A regular `{ }` escape hatch is opaque (docs/iris_core_spec.md §1.4) — a
        // JSX-looking string inside it must never be parsed or validated, matching
        // RenderBlockParser's own "an escape hatch containing angle brackets is opaque" test.
        const auto Errors = Validate(
            R"(render {
                <Slot>
                    {[&]() -> IrisComponent {
                        return <NotImported/>;
                    }}
                </Slot>
            })",
            Iris::IrisBuildTarget::Penumbra, {});
        ASSERT_TRUE(Errors.empty()); // a JSX-looking string inside an opaque { } escape hatch is never validated
    });

    IT("the PartyScreen example with correct imports validates cleanly", {
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
        ASSERT_TRUE(Errors.empty());
        // the full spec §9 PartyScreen example, with Button/HealthBar imported, validates with no errors
    });
});
