#include "cimmerian/test.hpp"

#include "Iris/IrisIr.h"
#include "Iris/IrisIrDocument.h"
#include "Iris/ImportResolver.h"
#include "Iris/RenderBlockParser.h"

#include <amanuensis/json.hpp>
#include <amanuensis/io/reader.hpp>

#include <string>

namespace {

using namespace Iris;

// Builds the real Iris IR JSON for `Source` via the existing (already-tested) serializer,
// then round-trips it through ParseIrisIrDocument -- exercising the deserializer against
// exactly what the producer actually emits, not a hand-typed JSON fixture that could drift
// from BuildIrisIr's real field names/shapes.
IrisIrDocumentParseResult Roundtrip(std::string_view Source, const std::string& FilePath = "test.irisx") {
    RenderBlockParser::Result Parsed = RenderBlockParser(Source, FilePath).Parse();
    Amanuensis::Value          Json = BuildIrisIr(Source, FilePath, {}, {}, Parsed);
    return ParseIrisIrDocument(Json);
}

} // namespace

DESCRIBE("IrisIrDocument", {
    IT("parses the top-level document fields", {
        const IrisIrDocumentParseResult Result = Roundtrip("render { <Frame /> }");
        ASSERT_TRUE(Result.Errors.empty());
        REQUIRE_TRUE(Result.Document.has_value());
        ASSERT_TRUE(Result.Document->Version == "1.0");
        ASSERT_TRUE(Result.Document->SourceFile == "test.irisx");
        ASSERT_TRUE(Result.Document->HostLanguage == "nyx");
    });

    IT("a render_block body node round-trips its root element and locations", {
        const IrisIrDocumentParseResult Result = Roundtrip("render {\n    <Frame />\n}");
        REQUIRE_TRUE(Result.Document.has_value());
        REQUIRE_EQUAL(Result.Document->Body.size(), static_cast<std::size_t>(1));
        const IrRenderBlockNode* Block = std::get_if<IrRenderBlockNode>(&Result.Document->Body[0]);
        REQUIRE_TRUE(Block != nullptr);
        ASSERT_TRUE(Block->Root.Tag == "Frame");
        ASSERT_EQUAL(Block->Location.Line, static_cast<std::uint32_t>(1));
        ASSERT_EQUAL(Block->Location.Length, static_cast<std::size_t>(6)); // strlen("render")
        ASSERT_EQUAL(Block->EndLocation.Length, static_cast<std::size_t>(1)); // the closing '}'
    });

    IT("nyx_source body nodes round-trip their raw text", {
        const IrisIrDocumentParseResult Result = Roundtrip("PRE render { <Frame /> } POST");
        REQUIRE_TRUE(Result.Document.has_value());
        REQUIRE_EQUAL(Result.Document->Body.size(), static_cast<std::size_t>(3));
        const IrNyxSourceNode* Before = std::get_if<IrNyxSourceNode>(&Result.Document->Body[0]);
        REQUIRE_TRUE(Before != nullptr);
        ASSERT_TRUE(Before->Source == "PRE ");
        ASSERT_TRUE(std::holds_alternative<IrRenderBlockNode>(Result.Document->Body[1]));
        const IrNyxSourceNode* After = std::get_if<IrNyxSourceNode>(&Result.Document->Body[2]);
        REQUIRE_TRUE(After != nullptr);
        ASSERT_TRUE(After->Source == " POST");
    });

    IT("a string-literal prop round-trips as an IsLiteral value", {
        const IrisIrDocumentParseResult Result = Roundtrip(R"(render { <Frame class="a" /> })");
        REQUIRE_TRUE(Result.Document.has_value());
        const auto& Root = std::get<IrRenderBlockNode>(Result.Document->Body[0]).Root;
        REQUIRE_EQUAL(Root.Props.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Root.Props[0].Name == "class");
        ASSERT_TRUE(Root.Props[0].Value.IsLiteral);
        ASSERT_TRUE(Root.Props[0].Value.Literal.Value == "a");
    });

    IT("an escape-hatch prop round-trips as a nyx_expression value with its raw source", {
        const IrisIrDocumentParseResult Result = Roundtrip(R"(render { <Frame onPress={doIt()} /> })");
        REQUIRE_TRUE(Result.Document.has_value());
        const auto& Root = std::get<IrRenderBlockNode>(Result.Document->Body[0]).Root;
        REQUIRE_EQUAL(Root.Props.size(), static_cast<std::size_t>(1));
        ASSERT_FALSE(Root.Props[0].Value.IsLiteral);
        ASSERT_TRUE(Root.Props[0].Value.Expression.Source == "doIt()");
        ASSERT_TRUE(Root.Props[0].Value.Expression.Children.empty());
    });

    IT("key and ref round-trip as their own optional fields", {
        const IrisIrDocumentParseResult Result = Roundtrip(R"(render { <Frame key="row-1" ref="trigger" /> })");
        REQUIRE_TRUE(Result.Document.has_value());
        const auto& Root = std::get<IrRenderBlockNode>(Result.Document->Body[0]).Root;
        ASSERT_TRUE(Root.Props.empty());
        REQUIRE_TRUE(Root.Key.has_value());
        ASSERT_TRUE(Root.Key->Literal.Value == "row-1");
        REQUIRE_TRUE(Root.Ref.has_value());
        ASSERT_TRUE(Root.Ref->Literal.Value == "trigger");
    });

    IT("an element with no key/ref leaves both unset", {
        const IrisIrDocumentParseResult Result = Roundtrip("render { <Frame /> }");
        REQUIRE_TRUE(Result.Document.has_value());
        const auto& Root = std::get<IrRenderBlockNode>(Result.Document->Body[0]).Root;
        ASSERT_FALSE(Root.Key.has_value());
        ASSERT_FALSE(Root.Ref.has_value());
    });

    IT("a !{ } JSX-transform escape hatch's nested elements round-trip as NyxExpression children", {
        const IrisIrDocumentParseResult Result = Roundtrip(R"(render {
            <Slot>
                !{settingsOpen ? <SettingsPage active="true" /> : <SettingsPage active="false" />}
            </Slot>
        })");
        REQUIRE_TRUE(Result.Document.has_value());
        const auto& SlotRoot = std::get<IrRenderBlockNode>(Result.Document->Body[0]).Root;
        REQUIRE_EQUAL(SlotRoot.Children.size(), static_cast<std::size_t>(1));
        const IrElementChild& Child = SlotRoot.Children[0];
        REQUIRE_TRUE(Child.Kind == IrElementChildKind::NyxExpression);
        REQUIRE_EQUAL(Child.Expression->Children.size(), static_cast<std::size_t>(2));
        ASSERT_TRUE(Child.Expression->Children[0].Tag == "SettingsPage");
        ASSERT_TRUE(Child.Expression->Children[1].Tag == "SettingsPage");
    });

    IT("a literal text child round-trips as its own Text-kind child, not folded into a prop", {
        const IrisIrDocumentParseResult Result = Roundtrip(R"(render { <Text>Hello</Text> })");
        REQUIRE_TRUE(Result.Document.has_value());
        const auto& Root = std::get<IrRenderBlockNode>(Result.Document->Body[0]).Root;
        ASSERT_TRUE(Root.Tag == "Text");
        REQUIRE_EQUAL(Root.Children.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Root.Children[0].Kind == IrElementChildKind::Text);
        ASSERT_TRUE(Root.Children[0].Text.Value == "Hello");
        ASSERT_EQUAL(Root.Children[0].Text.Location.Column, static_cast<std::uint32_t>(16));
    });

    IT("imports round-trip separately from body, and never leak into a nyx_source region", {
        const std::string Source = "import Button\nrender { <Frame /> }\n";
        const std::string FilePath = "test.irisx";
        const auto          Imports = ScanImports(Source, FilePath);
        REQUIRE_TRUE(Imports.size() == 1);
        const std::vector<ResolvedImport> Resolved({ResolvedImport{"Button", "components/Button.irisx"}});
        const RenderBlockParser::Result   Parsed = RenderBlockParser(Source, FilePath).Parse();
        const Amanuensis::Value            Json = BuildIrisIr(Source, FilePath, Imports, Resolved, Parsed);

        const IrisIrDocumentParseResult Result = ParseIrisIrDocument(Json);
        REQUIRE_TRUE(Result.Document.has_value());
        REQUIRE_EQUAL(Result.Document->Imports.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Result.Document->Imports[0].Name == "Button");
        ASSERT_TRUE(Result.Document->Imports[0].ResolvedPath == "components/Button.irisx");

        for (const IrBodyNode& Node : Result.Document->Body) {
            if (const IrNyxSourceNode* Source2 = std::get_if<IrNyxSourceNode>(&Node)) {
                ASSERT_FALSE(Source2->Source.find("import") != std::string::npos);
            }
        }
    });

    IT("a malformed document (missing required field) is reported, not silently defaulted", {
        Amanuensis::Value Bad = Amanuensis::Json::MakeObject();
        Amanuensis::Json::Insert(Bad, "version", Amanuensis::Value{std::string("1.0")});
        // sourceFile/hostLanguage/imports/body all deliberately missing.
        const IrisIrDocumentParseResult Result = ParseIrisIrDocument(Bad);
        ASSERT_FALSE(Result.Errors.empty());
    });
});
