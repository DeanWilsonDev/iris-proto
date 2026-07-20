#include "cimmerian/test.hpp"

#include "RenderTextHeuristics.h"

using IrisLsp::ClassifyRenderCompletion;
using IrisLsp::FindComponentDeclaration;
using IrisLsp::LineText;
using IrisLsp::RenderCompletionKind;
using IrisLsp::TagNameAtPosition;

DESCRIBE("RenderTextHeuristics.LineText", {
    IT("extracts the requested 1-based line", {
        const std::string Text = "one\ntwo\nthree";
        ASSERT_TRUE(LineText(Text, 1) == "one");
        ASSERT_TRUE(LineText(Text, 2) == "two");
        ASSERT_TRUE(LineText(Text, 3) == "three");
    });

    IT("returns empty for a line past the end", { ASSERT_TRUE(LineText("one\ntwo", 5).empty()); });
});

DESCRIBE("RenderTextHeuristics.ClassifyRenderCompletion", {
    IT("cursor right after '<' is a tag-name position", {
        ASSERT_TRUE(ClassifyRenderCompletion("<", 2) == RenderCompletionKind::TagName);
    });

    IT("cursor mid-identifier after '<' is still a tag-name position", {
        ASSERT_TRUE(ClassifyRenderCompletion("<Fra", 5) == RenderCompletionKind::TagName);
    });

    IT("cursor after whitespace inside an open tag is an attribute-name position", {
        ASSERT_TRUE(ClassifyRenderCompletion("<Frame ", 8) == RenderCompletionKind::AttributeName);
        ASSERT_TRUE(ClassifyRenderCompletion("<Frame cla", 11) == RenderCompletionKind::AttributeName);
    });

    IT("cursor after a closed '>' is neither", {
        // Column 10 is right after the whole "<Frame />" -- past its closing '>'.
        ASSERT_TRUE(ClassifyRenderCompletion("<Frame />", 10) == RenderCompletionKind::None);
        ASSERT_TRUE(ClassifyRenderCompletion("plain text", 5) == RenderCompletionKind::None);
    });
});

DESCRIBE("RenderTextHeuristics.TagNameAtPosition", {
    IT("finds the tag name when the cursor sits inside it", {
        const auto Result = TagNameAtPosition("<Frame class=\"a\">", 3);
        REQUIRE_TRUE(Result.has_value());
        ASSERT_TRUE(*Result == "Frame");
    });

    IT("finds the tag name when the cursor sits right on the opening '<'", {
        const auto Result = TagNameAtPosition("<Button />", 1);
        REQUIRE_TRUE(Result.has_value());
        ASSERT_TRUE(*Result == "Button");
    });

    IT("finds the tag name in a closing tag, skipping the '/'", {
        const auto Result = TagNameAtPosition("</Button>", 4);
        REQUIRE_TRUE(Result.has_value());
        ASSERT_TRUE(*Result == "Button");
    });

    IT("finds the correct tag among several on one line", {
        const auto Result = TagNameAtPosition("<Frame><Button /></Frame>", 11);
        REQUIRE_TRUE(Result.has_value());
        ASSERT_TRUE(*Result == "Button");
    });

    IT("returns nullopt for a cursor outside any tag name", {
        ASSERT_FALSE(TagNameAtPosition("plain text, no tags", 5).has_value());
    });
});

DESCRIBE("RenderTextHeuristics.FindComponentDeclaration", {
    IT("finds a component function declaration by name", {
        const std::string Text = "#include \"Iris/Component.h\"\n\nComponent Button(ButtonProps props) {\n}\n";
        const auto         Result = FindComponentDeclaration(Text, "Button");
        REQUIRE_TRUE(Result.has_value());
        ASSERT_TRUE(Result->first == 3); // 1-based line of "Component Button("
    });

    IT("does not match a substring of a longer identifier", {
        const std::string Text = "Component ButtonGroup() {}\n";
        ASSERT_FALSE(FindComponentDeclaration(Text, "Button").has_value());
    });

    IT("does not match the name unless followed by '('", {
        const std::string Text = "// Button is mentioned here but not declared\n";
        ASSERT_FALSE(FindComponentDeclaration(Text, "Button").has_value());
    });
});
