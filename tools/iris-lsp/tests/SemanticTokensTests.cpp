#include "cimmerian/test.hpp"

#include "Iris/RenderBlockParser.h"
#include "SemanticTokens.h"

namespace {

std::vector<Iris::RenderBlockParser::ParsedBlock> ParseBlocks(std::string_view Source) {
    Iris::RenderBlockParser Parser(Source, "test.iris");
    return Parser.Parse().Blocks;
}

} // namespace

DESCRIBE("SemanticTokens.CollectRenderBlockSemanticTokens", {
    IT("emits a Type token for a tag name, at the name itself (not the '<')", {
        const auto Blocks = ParseBlocks("render { <Frame /> }");
        const auto Tokens = IrisLsp::CollectRenderBlockSemanticTokens(Blocks);
        REQUIRE_TRUE(Tokens.size() == 1);
        ASSERT_TRUE(Tokens[0].Type == IrisLsp::SemanticTokenType::Type);
        ASSERT_TRUE(Tokens[0].Length == 5); // "Frame"
        ASSERT_TRUE(Tokens[0].Column == 11); // one past the '<' at column 10
    });

    IT("emits Property + String tokens for a string-valued prop", {
        const auto Blocks = ParseBlocks(R"(render { <Frame class="a" /> })");
        const auto Tokens = IrisLsp::CollectRenderBlockSemanticTokens(Blocks);
        REQUIRE_TRUE(Tokens.size() == 3); // tag, prop name, string value
        ASSERT_TRUE(Tokens[1].Type == IrisLsp::SemanticTokenType::Property);
        ASSERT_TRUE(Tokens[1].Length == 5); // "class"
        ASSERT_TRUE(Tokens[2].Type == IrisLsp::SemanticTokenType::String);
        ASSERT_TRUE(Tokens[2].Length == 3); // `"a"`, quotes included
    });

    IT("emits no String token for an escape-hatch prop value", {
        const auto Blocks = ParseBlocks("render { <Frame onPress={[&]() { x.set(true); }} /> }");
        const auto Tokens = IrisLsp::CollectRenderBlockSemanticTokens(Blocks);
        REQUIRE_TRUE(Tokens.size() == 2); // tag, prop name -- no third token for the { } body
        ASSERT_TRUE(Tokens[0].Type == IrisLsp::SemanticTokenType::Type);
        ASSERT_TRUE(Tokens[1].Type == IrisLsp::SemanticTokenType::Property);
    });

    IT("recurses into nested elements in source order", {
        const auto Blocks = ParseBlocks(R"(render { <Frame class="outer"><Frame class="inner" /></Frame> })");
        const auto Tokens = IrisLsp::CollectRenderBlockSemanticTokens(Blocks);
        // (tag, prop, string) x 2 for the two opening tags, plus one closing-tag token
        // for the outer </Frame> (the inner one is self-closing -- no closing tag at all).
        REQUIRE_TRUE(Tokens.size() == 7);
        ASSERT_TRUE(Tokens[0].Column < Tokens[3].Column); // outer tag comes before inner tag
        ASSERT_TRUE(Tokens.back().Type == IrisLsp::SemanticTokenType::Type); // the closing </Frame>
        ASSERT_TRUE(Tokens.back().Length == 5);                              // "Frame"
    });

    IT("emits a Type token for a non-self-closing tag's own closing tag", {
        const auto Blocks = ParseBlocks("render { <Frame></Frame> }");
        const auto Tokens = IrisLsp::CollectRenderBlockSemanticTokens(Blocks);
        REQUIRE_TRUE(Tokens.size() == 2); // opening tag, closing tag
        ASSERT_TRUE(Tokens[0].Type == IrisLsp::SemanticTokenType::Type);
        ASSERT_TRUE(Tokens[1].Type == IrisLsp::SemanticTokenType::Type);
        ASSERT_TRUE(Tokens[1].Length == 5);          // "Frame" in </Frame>
        ASSERT_TRUE(Tokens[1].Column > Tokens[0].Column); // closing tag comes after opening tag
    });

    IT("emits no closing-tag token for a self-closing element", {
        const auto Blocks = ParseBlocks("render { <Frame /> }");
        const auto Tokens = IrisLsp::CollectRenderBlockSemanticTokens(Blocks);
        REQUIRE_TRUE(Tokens.size() == 1); // just the opening tag -- no `/>` to jump to
    });

    IT("covers every render{} block when a file has more than one", {
        const std::string Source = "Component A() { render { <Frame /> } }\n"
                                    "Component B() { render { <Text /> } }\n";
        const auto Blocks = ParseBlocks(Source);
        REQUIRE_TRUE(Blocks.size() == 2);
        const auto Tokens = IrisLsp::CollectRenderBlockSemanticTokens(Blocks);
        REQUIRE_TRUE(Tokens.size() == 2);
        ASSERT_TRUE(Tokens[0].Line == 1);
        ASSERT_TRUE(Tokens[1].Line == 2);
    });
});
