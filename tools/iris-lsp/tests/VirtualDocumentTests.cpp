#include "cimmerian/test.hpp"

#include "VirtualDocument.h"

namespace {

Iris::IrisConfig TestConfig() {
    Iris::IrisConfig Config;
    Config.Target = Iris::IrisBuildTarget::Penumbra;
    Config.Version = "1.0";
    return Config;
}

} // namespace

DESCRIBE("VirtualDocument.IsInsideRenderBlock", {
    IT("is true inside a render{} block and false outside it", {
        const std::string Source = "#include \"Iris/Component.h\"\n"
                                    "using Iris::Component;\n"
                                    "Component Foo() {\n"
                                    "    render {\n"
                                    "        <Frame class=\"a\" />\n"
                                    "    }\n"
                                    "}\n";
        IrisLsp::VirtualDocument Doc(Source, "test.iris", TestConfig(), ".");
        ASSERT_FALSE(Doc.IsInsideRenderBlock(1, 1));   // #include line
        ASSERT_FALSE(Doc.IsInsideRenderBlock(3, 1));   // component signature line
        ASSERT_TRUE(Doc.IsInsideRenderBlock(5, 10));    // inside <Frame ... />
    });
});

DESCRIBE("VirtualDocument.ImportNameAtLine", {
    IT("finds the imported name on an import statement's own line only", {
        const std::string Source = "#include \"Iris/Component.h\"\n"
                                    "\n"
                                    "import Button\n"
                                    "\n"
                                    "using Iris::Component;\n"
                                    "Component Foo() { render { <Frame /> } }\n";
        IrisLsp::VirtualDocument Doc(Source, "test.iris", TestConfig(), ".");
        const auto                Result = Doc.ImportNameAtLine(3);
        REQUIRE_TRUE(Result.has_value());
        ASSERT_TRUE(*Result == "Button");
        ASSERT_FALSE(Doc.ImportNameAtLine(1).has_value());
        ASSERT_FALSE(Doc.ImportNameAtLine(6).has_value());
    });
});

DESCRIBE("VirtualDocument.line mapping", {
    IT("maps a host-language line after a render block to its shifted generated line and back", {
        // Two render{} blocks, each collapsing to one `return ...;` line in the
        // generated output -- CompileFile emits a `#line` directive resyncing line
        // numbers after each splice, which is what ToGenerated/ToSource read back.
        const std::string Source = "#include \"Iris/Component.h\"\n"     // 1
                                    "using Iris::Component;\n"            // 2
                                    "Component A() {\n"                   // 3
                                    "    render {\n"                      // 4
                                    "        <Frame />\n"                 // 5
                                    "    }\n"                              // 6
                                    "}\n"                                  // 7
                                    "Component B() {\n"                   // 8
                                    "    render {\n"                      // 9
                                    "        <Frame />\n"                 // 10
                                    "    }\n"                              // 11
                                    "}\n";                                 // 12
        IrisLsp::VirtualDocument Doc(Source, "test.iris", TestConfig(), ".");
        REQUIRE_TRUE(Doc.CompileResult().Diagnostics.empty());
        REQUIRE_TRUE(!Doc.CompileResult().Output.empty());

        // Line 8 ("Component B() {") is ordinary host-language text, copied verbatim --
        // it must round-trip exactly through ToGenerated then ToSource.
        const auto Generated = Doc.ToGenerated(8, 1);
        REQUIRE_TRUE(Generated.has_value());
        const auto RoundTripped = Doc.ToSource(Generated->first, Generated->second);
        REQUIRE_TRUE(RoundTripped.has_value());
        ASSERT_TRUE(RoundTripped->first == 8);
        ASSERT_TRUE(RoundTripped->second == 1);

        // The generated line for line 8 must be strictly after the generated line for
        // line 3 (both host-language lines, one before either splice, one after the
        // first) -- confirms the block collapsing actually shifted something rather
        // than ToGenerated silently returning identity.
        const auto GeneratedForLine3 = Doc.ToGenerated(3, 1);
        REQUIRE_TRUE(GeneratedForLine3.has_value());
        ASSERT_TRUE(Generated->first > GeneratedForLine3->first);
    });
});
