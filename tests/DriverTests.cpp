#include "cimmerian/test.hpp"

#include "Iris/Driver.h"

#include <amanuensis/io/reader.hpp>
#include <amanuensis/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

bool Contains(const std::string& Haystack, std::string_view Needle) {
    return Haystack.find(Needle) != std::string::npos;
}

template <typename Container>
bool DiagnosticsContain(const Container& Diagnostics, std::string_view Needle) {
    for (const auto& D : Diagnostics) {
        if (D.Message.find(Needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Minimal on-disk fixture — CompileFile's import handling genuinely stats files via
// ResolveImports, same reasoning as ImportResolverTests.cpp's TempProject.
class TempProject {
public:
    TempProject() {
        Root_ = std::filesystem::temp_directory_path() / "iris_driver_test";
        std::filesystem::remove_all(Root_);
        std::filesystem::create_directories(Root_ / "components");
    }

    ~TempProject() { std::filesystem::remove_all(Root_); }

    void WriteComponent(const std::string& Name) {
        std::ofstream(Root_ / "components" / (Name + ".iris")) << "// stub component\n";
    }

    std::string RootPath() const { return Root_.string(); }

private:
    std::filesystem::path Root_;
};

Iris::IrisConfig PenumbraConfig() {
    Iris::IrisConfig Config;
    Config.Target = Iris::IrisBuildTarget::Penumbra;
    Config.SearchPaths = {"components"};
    return Config;
}

} // namespace

DESCRIBE("Driver", {
    IT("a simple render block compiles", {
        const Iris::DriverResult Result =
            Iris::CompileFile("Component Foo() {\n    render { <Frame class=\"a\" /> }\n}\n", "test.iris",
                               PenumbraConfig(), "/nonexistent");
        ASSERT_TRUE(Result.Diagnostics.empty()); // a plain Core-primitive render block compiles with no diagnostics
        ASSERT_TRUE(Contains(Result.Output, "return Iris::Component{Iris::IrisElementTag::Frame"));
        // the render{ } block becomes a return statement constructing the component
        ASSERT_FALSE(Contains(Result.Output, "render {")); // the literal render{ } text is gone from the output
    });

    IT("the output has #line directives", {
        const Iris::DriverResult Result = Iris::CompileFile("Component Foo() {\n    render { <Frame /> }\n}\n",
                                                              "test.iris", PenumbraConfig(), "/nonexistent");
        ASSERT_TRUE(Contains(Result.Output, "#line 1 \"test.iris\"")); // output starts with a #line directive for the file
        ASSERT_TRUE(Contains(Result.Output, "#line 2 \"test.iris\""));
        // a resync #line directive follows the replaced render block, at the line its closing '}' was on
    });

    IT("an import line becomes an #include of the generated header", {
        TempProject Project;
        Project.WriteComponent("Button");

        const Iris::DriverResult Result =
            Iris::CompileFile("import Button\nComponent Foo() {\n    render { <Button label=\"x\" /> }\n}\n",
                               "test.iris", PenumbraConfig(), Project.RootPath());
        ASSERT_TRUE(Result.Diagnostics.empty()); // a render block using an imported component compiles with no diagnostics
        ASSERT_FALSE(Contains(Result.Output, "import Button"));
        // the literal 'import Button' text is gone, not just commented out
        ASSERT_TRUE(Contains(Result.Output, "#include \"components/Button.iris.h\""));
        // it becomes an #include of the resolved import's generated header, relative to ProjectRoot
        ASSERT_TRUE(Contains(Result.Output, "#pragma once"));
        // every generated file is a self-contained, include-guarded header
        ASSERT_TRUE(Contains(Result.Output, "Button(ButtonProps{"));
        // the imported component is emitted as an ordinary function call
    });

    IT("an unresolved import is a diagnostic and blocks output", {
        const Iris::DriverResult Result =
            Iris::CompileFile("import NoSuchComponent\nComponent Foo() { render { <Frame/> } }\n", "test.iris",
                               PenumbraConfig(), "/nonexistent");
        ASSERT_FALSE(Result.Diagnostics.empty()); // an unresolvable import is a diagnostic
        ASSERT_TRUE(Result.Output.empty());       // no output is produced when there are diagnostics
        ASSERT_TRUE(DiagnosticsContain(Result.Diagnostics, "Cannot resolve `import NoSuchComponent`"));
        // the diagnostic is ImportResolver's own unresolved-import message
    });

    IT("an unimported component reference is a diagnostic", {
        const Iris::DriverResult Result = Iris::CompileFile(
            "Component Foo() { render { <HealthBar current={1} max={2} /> } }\n", "test.iris", PenumbraConfig(),
            "/nonexistent");
        ASSERT_FALSE(Result.Diagnostics.empty()); // an unimported, non-Core-primitive tag is a diagnostic
        ASSERT_TRUE(Result.Output.empty());       // no output is produced when there are diagnostics
        ASSERT_TRUE(DiagnosticsContain(Result.Diagnostics, "HealthBar` is not imported and is not a Core primitive"));
        // the diagnostic is SemanticValidator's message
    });

    IT("a parse error is a diagnostic and blocks output", {
        const Iris::DriverResult Result = Iris::CompileFile("Component Foo() { render { <A/> <B/> } }\n", "test.iris",
                                                              PenumbraConfig(), "/nonexistent");
        ASSERT_FALSE(Result.Diagnostics.empty()); // a multi-root render block is a diagnostic
        ASSERT_TRUE(Result.Output.empty());       // no output is produced when there are diagnostics
        ASSERT_TRUE(DiagnosticsContain(Result.Diagnostics, "must have exactly one root element"));
        // the diagnostic is RenderBlockParser's own message
    });

    IT("the full PartyScreen example compiles cleanly", {
        TempProject Project;
        Project.WriteComponent("Button");
        Project.WriteComponent("HealthBar");

        const Iris::DriverResult Result = Iris::CompileFile(
            R"(import HealthBar
import Button

Component PartyScreen(PartyScreenProps props) {
    iris::Signal<bool> detailsOpen = false;

    render {
        <Frame class="party-screen">
            <Button label="Details" onPress={[&]() { detailsOpen.set(true); }} />
            <Slot>
                !{[&]() -> Component {
                    if (!detailsOpen.get()) return nullptr;
                    return <Frame class="details-panel">
                        <Slot>
                            !{[&]() -> std::vector<Component> {
                                std::vector<Component> rows;
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
    }
}
)",
            "PartyScreen.iris", PenumbraConfig(), Project.RootPath());

        ASSERT_TRUE(Result.Diagnostics.empty()); // the full spec §9 PartyScreen example compiles with no diagnostics
        ASSERT_FALSE(Contains(Result.Output, "render {")); // no render{ } text survives in the output
        ASSERT_TRUE(!Contains(Result.Output, "<Frame") && !Contains(Result.Output, "<HealthBar"));
        // no raw JSX text survives anywhere, including inside the nested !{ } bodies
        ASSERT_TRUE(Contains(Result.Output, "#include \"components/HealthBar.iris.h\"") &&
                    Contains(Result.Output, "#include \"components/Button.iris.h\""));
        // both import lines become #includes of their resolved generated headers
    });

    IT("a .irisx file compiles to a Chaos IR JSON document, not C++", {
        Iris::IrisConfig Config;
        Config.Target = Iris::IrisBuildTarget::UmbraEngine;

        const Iris::DriverResult Result = Iris::CompileFile(
            "Component Foo() {\n    render { <Frame class=\"a\" /> }\n}\n", "test.irisx", Config, "/nonexistent");

        ASSERT_TRUE(Result.Diagnostics.empty()); // a plain Core-primitive render block compiles with no diagnostics
        ASSERT_FALSE(Contains(Result.Output, "return Iris::Component"));
        // Codegen.cpp is never invoked for .irisx -- no spliced C++ expression appears anywhere
        ASSERT_FALSE(Contains(Result.Output, "#pragma once"));
        // .irisx output isn't a self-contained header the way .iris output is

        const Amanuensis::ParseResult Parsed = Amanuensis::Reader::ParseString(Result.Output);
        ASSERT_TRUE(Parsed.succeeded); // Output is valid JSON
        ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::Get(Parsed.value, "hostLanguage")) == "nyx");
        ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::Get(Parsed.value, "sourceFile")) == "test.irisx");
        const Amanuensis::Value& Body = Amanuensis::Json::Get(Parsed.value, "body");
        REQUIRE_TRUE(Amanuensis::Json::Size(Body) >= 1);
        bool FoundRenderBlock = false;
        for (std::size_t Index = 0; Index < Amanuensis::Json::Size(Body); ++Index) {
            const Amanuensis::Value& Node = Amanuensis::Json::At(Body, Index);
            if (Amanuensis::Json::AsString(Amanuensis::Json::Get(Node, "kind")) == "render_block") {
                FoundRenderBlock = true;
                ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::Get(
                                Amanuensis::Json::Get(Node, "root"), "tag")) == "Frame");
            }
        }
        ASSERT_TRUE(FoundRenderBlock); // the render{ } block round-trips through the parsed JSON as a render_block node
    });

    IT("a .irisx parse/semantic error is still a diagnostic that blocks output", {
        Iris::IrisConfig Config;
        Config.Target = Iris::IrisBuildTarget::UmbraEngine;

        const Iris::DriverResult Result = Iris::CompileFile(
            "Component Foo() { render { <HealthBar current={1} max={2} /> } }\n", "test.irisx", Config,
            "/nonexistent");
        ASSERT_FALSE(Result.Diagnostics.empty()); // an unimported, non-Core-primitive tag is still a diagnostic
        ASSERT_TRUE(Result.Output.empty());       // no IR is produced when there are diagnostics, same as the .iris path
    });
});
