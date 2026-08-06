#include "cimmerian/test.hpp"

#include "Iris/IrisIr.h"
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

// Builds the Iris IR for a source string with no imports — the common case most of these
// tests exercise. Tests that care about imports call BuildIrisIr directly instead.
Amanuensis::Value Build(std::string_view Source, const std::string& FilePath = "test.irisx") {
    const RenderBlockParser::Result ParseResult = Parse(Source, FilePath);
    return BuildIrisIr(Source, FilePath, {}, {}, ParseResult);
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

DESCRIBE("IrisIr", {
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

    IT("an escape-hatch prop becomes a nyx_expression node with a single text segment", {
        const Amanuensis::Value Doc = Build(R"(render { <Frame onPress={doIt()} /> })");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        const Amanuensis::Value& Prop = Amanuensis::Json::At(Field(Root, "props"), 0);
        const Amanuensis::Value& Value = Field(Prop, "value");
        ASSERT_TRUE(Str(Field(Value, "kind")) == "nyx_expression");
        REQUIRE_TRUE(Amanuensis::Json::Size(Field(Value, "segments")) == 1);
        const Amanuensis::Value& Seg = Amanuensis::Json::At(Field(Value, "segments"), 0);
        ASSERT_TRUE(Str(Field(Seg, "kind")) == "text");
        ASSERT_TRUE(Str(Field(Seg, "value")) == "doIt()");
    });

    IT("key and ref are preserved as their own ElementNode fields, not dropped", {
        const Amanuensis::Value Doc = Build(R"(render { <Frame key="row-1" ref="trigger" /> })");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        // Neither key nor ref leaks into `props` -- chaos-ir-spec.md §3.5 gives each its own
        // dedicated field alongside `tag`/`props`/`children`/`location`.
        ASSERT_TRUE(Amanuensis::Json::Size(Field(Root, "props")) == 0);
        ASSERT_TRUE(Str(Field(Field(Root, "key"), "kind")) == "literal");
        ASSERT_TRUE(Str(Field(Field(Root, "key"), "value")) == "row-1");
        ASSERT_TRUE(Str(Field(Field(Root, "ref"), "kind")) == "literal");
        ASSERT_TRUE(Str(Field(Field(Root, "ref"), "value")) == "trigger");
    });

    IT("an element with no key/ref omits both fields entirely", {
        const Amanuensis::Value Doc = Build("render { <Frame /> }");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        ASSERT_FALSE(Amanuensis::Json::Contains(Root, "key"));
        ASSERT_FALSE(Amanuensis::Json::Contains(Root, "ref"));
    });

    IT("a !{ } JSX-transform escape hatch preserves text/element interleaving order in segments", {
        // Real Nyx expression syntax (a ternary, no C++-only lambda-capture-list `&`/`[&]`
        // tokens Nyx's own lexer has no grammar for -- chaos-ir-spec.md §4's own worked
        // example uses exactly this shape: `!{() -> isHovered ? <Frame .../> : <Frame .../>}`).
        // docs/iris_nyx_evaluator_scope_gap.md: an earlier schema flushed text/element runs
        // into two separate fields (source/children), discarding their relative order --
        // `segments` is the fix, one ordered array mirroring JsxSegments directly.
        const Amanuensis::Value Doc = Build(R"(render {
            <Slot>
                !{settingsOpen ? <SettingsPage active="true" /> : <SettingsPage active="false" />}
            </Slot>
        })");
        const Amanuensis::Value& SlotRoot = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        REQUIRE_TRUE(Amanuensis::Json::Size(Field(SlotRoot, "children")) == 1);
        const Amanuensis::Value& EscapeHatchNode = Amanuensis::Json::At(Field(SlotRoot, "children"), 0);
        ASSERT_TRUE(Str(Field(EscapeHatchNode, "kind")) == "nyx_expression");
        const Amanuensis::Value& Segments = Field(EscapeHatchNode, "segments");
        REQUIRE_EQUAL(Amanuensis::Json::Size(Segments), static_cast<std::size_t>(4));

        const Amanuensis::Value& Seg0 = Amanuensis::Json::At(Segments, 0);
        ASSERT_TRUE(Str(Field(Seg0, "kind")) == "text");
        ASSERT_TRUE(Contains(Str(Field(Seg0, "value")), "settingsOpen"));

        const Amanuensis::Value& Seg1 = Amanuensis::Json::At(Segments, 1);
        ASSERT_TRUE(Str(Field(Seg1, "kind")) == "element");
        ASSERT_TRUE(Str(Field(Seg1, "tag")) == "SettingsPage");

        const Amanuensis::Value& Seg2 = Amanuensis::Json::At(Segments, 2);
        ASSERT_TRUE(Str(Field(Seg2, "kind")) == "text");

        const Amanuensis::Value& Seg3 = Amanuensis::Json::At(Segments, 3);
        ASSERT_TRUE(Str(Field(Seg3, "kind")) == "element");
        ASSERT_TRUE(Str(Field(Seg3, "tag")) == "SettingsPage");
    });

    IT("a literal text child is preserved as its own text node, not a prop-value literal", {
        const Amanuensis::Value Doc = Build(R"(render { <Text>Hello</Text> })");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        ASSERT_TRUE(Str(Field(Root, "tag")) == "Text");
        const Amanuensis::Value& Children = Field(Root, "children");
        REQUIRE_TRUE(Amanuensis::Json::Size(Children) == 1);
        const Amanuensis::Value& TextChild = Amanuensis::Json::At(Children, 0);
        // chaos-ir-spec.md §3.5a's dedicated "text" child-node kind -- distinct from a
        // PropNode's own "literal" value node (§3.6), which is a prop's value, not a tree
        // position.
        ASSERT_TRUE(Str(Field(TextChild, "kind")) == "text");
        ASSERT_TRUE(Str(Field(TextChild, "value")) == "Hello");
    });

    IT("a literal text child's location is its own, not the parent element's", {
        // "render { <Text>Hello</Text> }" -- <Text starts at column 10 (the '<'), but
        // "Hello" itself starts at column 16, right after '>'. Before this was fixed
        // (docs/archive/iris_next_steps_resolved.md's "Codegen has no Nyx-target emission"
        // entry), a text child's location silently fell back to its *parent* element's own
        // location
        // (ElementChild had no SourceLocation of its own), so this would have come back
        // as column 10, not 16.
        const Amanuensis::Value Doc = Build(R"(render { <Text>Hello</Text> })");
        const Amanuensis::Value& Root = Field(Amanuensis::Json::At(Field(Doc, "body"), 0), "root");
        const Amanuensis::Value& RootLocation = Field(Root, "location");
        REQUIRE_EQUAL(Int(Field(RootLocation, "column")), static_cast<long long>(10));

        const Amanuensis::Value& TextChild = Amanuensis::Json::At(Field(Root, "children"), 0);
        const Amanuensis::Value& TextLocation = Field(TextChild, "location");
        ASSERT_TRUE(Int(Field(TextLocation, "line")) == 1);
        ASSERT_TRUE(Int(Field(TextLocation, "column")) == 16); // where "Hello" itself starts
        ASSERT_TRUE(Int(Field(TextLocation, "length")) == 5);  // "Hello".size(), no quote padding
    });

    IT("an import statement is reported in `imports`, not duplicated into a `body` nyx_source node", {
        const std::string    Source = "import Button\nrender { <Frame /> }\n";
        const std::string    FilePath = "test.irisx";
        const auto            Imports = ScanImports(Source, FilePath);
        REQUIRE_TRUE(Imports.size() == 1);
        const std::vector<ResolvedImport> Resolved({ResolvedImport{"Button", "components/Button.irisx"}});
        const RenderBlockParser::Result   ParseResult = Parse(Source, FilePath);

        const Amanuensis::Value Doc = BuildIrisIr(Source, FilePath, Imports, Resolved, ParseResult);

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
