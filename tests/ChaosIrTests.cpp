#include "cimmerian/test.hpp"

#include "Iris/ChaosIr.h"
#include "Iris/ImportResolver.h"
#include "Iris/RenderBlockParser.h"

#include <amanuensis/json.hpp>

#include <string>
#include <vector>

namespace {

using namespace Iris;

RenderBlockParser::Result Parse(std::string_view Source, const std::string& FilePath) {
    RenderBlockParser Parser(Source, FilePath);
    return Parser.Parse();
}

// Builds the Chaos IR for a source string with no imports — the common case most of these
// tests exercise. Tests that care about imports call BuildChaosIr directly instead.
Amanuensis::Value Build(std::string_view Source, const std::string& FilePath = "test.irisx") {
    const RenderBlockParser::Result ParseResult = Parse(Source, FilePath);
    return BuildChaosIr(Source, FilePath, {}, {}, ParseResult);
}

const Amanuensis::Value& Field(const Amanuensis::Value& V, const std::string& Key) {
    return Amanuensis::Json::Get(V, Key);
}

const std::string& Str(const Amanuensis::Value& V) { return Amanuensis::Json::AsString(V); }

long long Int(const Amanuensis::Value& V) { return Amanuensis::Json::AsInteger(V); }

bool Contains(const std::string& Haystack, std::string_view Needle) {
    return Haystack.find(Needle) != std::string::npos;
}

} // namespace

DESCRIBE("ChaosIr", {
    IT("the top-level document carries version/sourceFile/hostLanguage", {
        const Amanuensis::Value Doc = Build("render { <Frame /> }");
        ASSERT_TRUE(Str(Field(Doc, "version")) == "1.0");
        ASSERT_TRUE(Str(Field(Doc, "sourceFile")) == "test.irisx");
        ASSERT_TRUE(Str(Field(Doc, "hostLanguage")) == "nyx");
    });

    IT("a render block becomes a render_block body node", {
        const Amanuensis::Value Doc = Build("render {\n    <Frame />\n}");
        const Amanuensis::Value& Body = Field(Doc, "body");
        REQUIRE_TRUE(Amanuensis::Json::Size(Body) == 1); // no trailing nyx_source when the block ends the file
        const Amanuensis::Value& Node = Amanuensis::Json::At(Body, 0);
        ASSERT_TRUE(Str(Field(Node, "kind")) == "render_block");
        ASSERT_TRUE(Int(Field(Field(Node, "location"), "line")) == 1);
        ASSERT_TRUE(Int(Field(Field(Node, "location"), "length")) == 6); // strlen("render")
        ASSERT_TRUE(Int(Field(Field(Node, "endLocation"), "length")) == 1); // the closing '}'
        ASSERT_TRUE(Str(Field(Field(Node, "root"), "tag")) == "Frame");
    });

    IT("nyx source before and after a render block becomes nyx_source body nodes", {
        const Amanuensis::Value Doc = Build("PRE render { <Frame /> } POST");
        const Amanuensis::Value& Body = Field(Doc, "body");
        REQUIRE_TRUE(Amanuensis::Json::Size(Body) == 3);

        const Amanuensis::Value& Before = Amanuensis::Json::At(Body, 0);
        ASSERT_TRUE(Str(Field(Before, "kind")) == "nyx_source");
        ASSERT_TRUE(Str(Field(Before, "source")) == "PRE ");
        ASSERT_TRUE(Int(Field(Field(Before, "location"), "length")) == 4);

        ASSERT_TRUE(Str(Field(Amanuensis::Json::At(Body, 1), "kind")) == "render_block");

        const Amanuensis::Value& After = Amanuensis::Json::At(Body, 2);
        ASSERT_TRUE(Str(Field(After, "kind")) == "nyx_source");
        ASSERT_TRUE(Str(Field(After, "source")) == " POST");
    });

    IT("a string-literal prop becomes a literal value node", {
        const Amanuensis::Value Doc = Build(R"(render { <Frame class="a" /> })");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        const Amanuensis::Value& Props = Field(Root, "props");
        REQUIRE_TRUE(Amanuensis::Json::Size(Props) == 1);
        const Amanuensis::Value& ClassProp = Amanuensis::Json::At(Props, 0);
        ASSERT_TRUE(Str(Field(ClassProp, "kind")) == "prop");
        ASSERT_TRUE(Str(Field(ClassProp, "name")) == "class");
        const Amanuensis::Value& Value = Field(ClassProp, "value");
        ASSERT_TRUE(Str(Field(Value, "kind")) == "literal");
        ASSERT_TRUE(Str(Field(Value, "value")) == "a");
    });

    IT("an escape-hatch prop becomes a nyx_expression node with the raw text as source", {
        const Amanuensis::Value Doc = Build(R"(render { <Frame onPress={doIt()} /> })");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        const Amanuensis::Value& Prop = Amanuensis::Json::At(Field(Root, "props"), 0);
        const Amanuensis::Value& Value = Field(Prop, "value");
        ASSERT_TRUE(Str(Field(Value, "kind")) == "nyx_expression");
        ASSERT_TRUE(Str(Field(Value, "source")) == "doIt()");
        ASSERT_TRUE(Amanuensis::Json::Size(Field(Value, "children")) == 0);
    });

    IT("key and ref are preserved as synthetic props, not dropped", {
        const Amanuensis::Value Doc = Build(R"(render { <Frame key="row-1" ref="trigger" /> })");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        const Amanuensis::Value& Props = Field(Root, "props");
        REQUIRE_TRUE(Amanuensis::Json::Size(Props) == 2);
        ASSERT_TRUE(Str(Field(Amanuensis::Json::At(Props, 0), "name")) == "key");
        ASSERT_TRUE(Str(Field(Field(Amanuensis::Json::At(Props, 0), "value"), "value")) == "row-1");
        ASSERT_TRUE(Str(Field(Amanuensis::Json::At(Props, 1), "name")) == "ref");
        ASSERT_TRUE(Str(Field(Field(Amanuensis::Json::At(Props, 1), "value"), "value")) == "trigger");
    });

    IT("a !{ } JSX-transform escape hatch's nested element becomes a nyx_expression child", {
        // Real Nyx expression syntax (a ternary, no C++-only lambda-capture-list `&`/`[&]`
        // tokens Nyx's own lexer has no grammar for -- chaos-ir-spec.md §4's own worked
        // example uses exactly this shape: `!{() -> isHovered ? <Frame .../> : <Frame .../>}`).
        const Amanuensis::Value Doc = Build(R"(render {
            <Slot>
                !{settingsOpen ? <SettingsPage active="true" /> : <SettingsPage active="false" />}
            </Slot>
        })");
        const Amanuensis::Value& SlotRoot = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        REQUIRE_TRUE(Amanuensis::Json::Size(Field(SlotRoot, "children")) == 1);
        const Amanuensis::Value& EscapeHatchNode = Amanuensis::Json::At(Field(SlotRoot, "children"), 0);
        ASSERT_TRUE(Str(Field(EscapeHatchNode, "kind")) == "nyx_expression");
        const Amanuensis::Value& Children = Field(EscapeHatchNode, "children");
        REQUIRE_TRUE(Amanuensis::Json::Size(Children) == 2); // both ternary branches are nested elements
        ASSERT_TRUE(Str(Field(Amanuensis::Json::At(Children, 0), "tag")) == "SettingsPage");
        ASSERT_TRUE(Str(Field(Amanuensis::Json::At(Children, 1), "tag")) == "SettingsPage");
        // both nested elements are extracted into `children`, not left inline in `source`
        ASSERT_FALSE(Contains(Str(Field(EscapeHatchNode, "source")), "<SettingsPage"));
        ASSERT_TRUE(Contains(Str(Field(EscapeHatchNode, "source")), "settingsOpen"));
    });

    IT("a literal text child is preserved as a literal node", {
        const Amanuensis::Value Doc = Build(R"(render { <Text>Hello</Text> })");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        ASSERT_TRUE(Str(Field(Root, "tag")) == "Text");
        const Amanuensis::Value& Children = Field(Root, "children");
        REQUIRE_TRUE(Amanuensis::Json::Size(Children) == 1);
        const Amanuensis::Value& TextChild = Amanuensis::Json::At(Children, 0);
        ASSERT_TRUE(Str(Field(TextChild, "kind")) == "literal");
        ASSERT_TRUE(Str(Field(TextChild, "value")) == "Hello");
    });

    IT("an import statement is reported in `imports`, not duplicated into a `body` nyx_source node", {
        const std::string    Source = "import Button\nrender { <Frame /> }\n";
        const std::string    FilePath = "test.irisx";
        const auto            Imports = ScanImports(Source, FilePath);
        REQUIRE_TRUE(Imports.size() == 1);
        const std::vector<ResolvedImport> Resolved({ResolvedImport{"Button", "components/Button.irisx"}});
        const RenderBlockParser::Result   ParseResult = Parse(Source, FilePath);

        const Amanuensis::Value Doc = BuildChaosIr(Source, FilePath, Imports, Resolved, ParseResult);

        const Amanuensis::Value& ImportNodes = Field(Doc, "imports");
        REQUIRE_TRUE(Amanuensis::Json::Size(ImportNodes) == 1);
        const Amanuensis::Value& ImportNode = Amanuensis::Json::At(ImportNodes, 0);
        ASSERT_TRUE(Str(Field(ImportNode, "kind")) == "import");
        ASSERT_TRUE(Str(Field(ImportNode, "name")) == "Button");
        ASSERT_TRUE(Str(Field(ImportNode, "resolvedPath")) == "components/Button.irisx");
        ASSERT_TRUE(Int(Field(Field(ImportNode, "location"), "length")) == 13); // strlen("import Button")

        const Amanuensis::Value& Body = Field(Doc, "body");
        for (std::size_t Index = 0; Index < Amanuensis::Json::Size(Body); ++Index) {
            const Amanuensis::Value& Node = Amanuensis::Json::At(Body, Index);
            if (Str(Field(Node, "kind")) == "nyx_source") {
                ASSERT_FALSE(Contains(Str(Field(Node, "source")), "import"));
            }
        }
    });
});
