#include "cimmerian/test.hpp"

#include "JsonRpc.h"
#include "Server.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

// Same "one temp project directory per test, cleaned up after" shape
// tests/DriverTests.cpp's own TempProject uses.
class TempProject {
public:
    explicit TempProject(const std::string& Name) {
        Root_ = std::filesystem::temp_directory_path() / ("iris_lsp_server_test_" + Name);
        std::filesystem::remove_all(Root_);
        std::filesystem::create_directories(Root_);
        std::ofstream(Root_ / ".iris.json") << R"({"target":"penumbra","version":"1.0","searchPaths":["."]})";
    }
    ~TempProject() { std::filesystem::remove_all(Root_); }

    void WriteFile(const std::string& RelativeName, const std::string& Content) {
        std::ofstream(Root_ / RelativeName) << Content;
    }

    std::string PathOf(const std::string& RelativeName) const { return (Root_ / RelativeName).string(); }

private:
    std::filesystem::path Root_;
};

std::string Uri(const std::string& Path) { return "file://" + Path; }

Amanuensis::Value MakeMessage(std::string Method, Amanuensis::Value Params) {
    Amanuensis::Value Message = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Message, "jsonrpc", Amanuensis::Value("2.0"));
    Amanuensis::Json::Insert(Message, "method", Amanuensis::Value(std::move(Method)));
    Amanuensis::Json::Insert(Message, "params", std::move(Params));
    return Message;
}

Amanuensis::Value MakeRequest(int Id, std::string Method, Amanuensis::Value Params) {
    Amanuensis::Value Message = MakeMessage(std::move(Method), std::move(Params));
    Amanuensis::Json::Insert(Message, "id", Amanuensis::Value(static_cast<long long>(Id)));
    return Message;
}

Amanuensis::Value MakeTextDocument(const std::string& FileUri, const std::string& Text) {
    Amanuensis::Value TextDocument = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(TextDocument, "uri", Amanuensis::Value(FileUri));
    Amanuensis::Json::Insert(TextDocument, "languageId", Amanuensis::Value("cpp"));
    Amanuensis::Json::Insert(TextDocument, "version", Amanuensis::Value(static_cast<long long>(1)));
    Amanuensis::Json::Insert(TextDocument, "text", Amanuensis::Value(Text));
    Amanuensis::Value Params = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Params, "textDocument", std::move(TextDocument));
    return Params;
}

Amanuensis::Value MakeTextDocumentParams(const std::string& FileUri) {
    Amanuensis::Value TextDocument = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(TextDocument, "uri", Amanuensis::Value(FileUri));
    Amanuensis::Value Params = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Params, "textDocument", std::move(TextDocument));
    return Params;
}

Amanuensis::Value MakePositionParams(const std::string& FileUri, std::uint32_t Line0, std::uint32_t Char0) {
    Amanuensis::Value TextDocument = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(TextDocument, "uri", Amanuensis::Value(FileUri));
    Amanuensis::Value Position = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Position, "line", Amanuensis::Value(static_cast<long long>(Line0)));
    Amanuensis::Json::Insert(Position, "character", Amanuensis::Value(static_cast<long long>(Char0)));
    Amanuensis::Value Params = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Params, "textDocument", std::move(TextDocument));
    Amanuensis::Json::Insert(Params, "position", std::move(Position));
    return Params;
}

// Runs Server over a scratch pair of temp files carrying the given input messages,
// disables proxy creation first (tools/iris-lsp/tests/ has no dependency on clangd
// being installed -- see docs/iris_lsp_decision.md §7), and returns every message the
// server wrote to its "client" in order.
std::vector<Amanuensis::Value> RunServer(const std::vector<Amanuensis::Value>& Inputs) {
    std::FILE* In = std::tmpfile();
    std::FILE* Out = std::tmpfile();

    for (const Amanuensis::Value& Message : Inputs) {
        IrisLsp::JsonRpc::WriteMessage(In, Message);
    }
    std::rewind(In);

    IrisLsp::Server Server;
    Server.DisableProxyForTesting();
    Server.Run(In, Out);

    std::rewind(Out);
    std::vector<Amanuensis::Value> Outputs;
    for (;;) {
        auto Message = IrisLsp::JsonRpc::ReadMessage(Out);
        if (!Message) {
            break;
        }
        Outputs.push_back(std::move(*Message));
    }
    std::fclose(In);
    std::fclose(Out);
    return Outputs;
}

const Amanuensis::Value* FindByMethod(const std::vector<Amanuensis::Value>& Messages, const std::string& Method) {
    for (const Amanuensis::Value& Message : Messages) {
        if (Amanuensis::Json::Contains(Message, "method") &&
            Amanuensis::Json::AsString(Amanuensis::Json::Get(Message, "method")) == Method) {
            return &Message;
        }
    }
    return nullptr;
}

const Amanuensis::Value* FindReplyToId(const std::vector<Amanuensis::Value>& Messages, int Id) {
    for (const Amanuensis::Value& Message : Messages) {
        if (Amanuensis::Json::Contains(Message, "id") &&
            Amanuensis::Json::IsInteger(Amanuensis::Json::Get(Message, "id")) &&
            Amanuensis::Json::AsInteger(Amanuensis::Json::Get(Message, "id")) == Id) {
            return &Message;
        }
    }
    return nullptr;
}

bool Contains(const std::string& Haystack, std::string_view Needle) { return Haystack.find(Needle) != std::string::npos; }

const std::string SimpleComponentSource = "#include \"Iris/Component.h\"\n"
                                           "using Iris::Component;\n"
                                           "Component Foo() {\n"
                                           "    render {\n"
                                           "        <Frame class=\"a\">\n"
                                           "            <Frame />\n"
                                           "        </Frame>\n"
                                           "    }\n"
                                           "}\n";

} // namespace

DESCRIBE("Server.diagnostics", {
    IT("publishes an Iris-level diagnostic for an unknown prop", {
        const std::string FileUri = Uri("/scratch/Bad.iris");
        const std::string Source = "#include \"Iris/Component.h\"\n"
                                    "using Iris::Component;\n"
                                    "Component Bad() { render { <Frame nonsense=\"x\" /> } }\n";

        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Diagnostics = FindByMethod(Outputs, "textDocument/publishDiagnostics");
        REQUIRE_TRUE(Diagnostics != nullptr);
        const Amanuensis::Value& List =
            Amanuensis::Json::Get(Amanuensis::Json::Get(*Diagnostics, "params"), "diagnostics");
        REQUIRE_TRUE(Amanuensis::Json::Size(List) == 1);
        ASSERT_TRUE(Contains(Amanuensis::Json::AsString(Amanuensis::Json::Get(Amanuensis::Json::At(List, 0), "message")),
                              "not a known prop"));
    });

    IT("publishes no diagnostics for a valid file", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        const auto         Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Diagnostics = FindByMethod(Outputs, "textDocument/publishDiagnostics");
        REQUIRE_TRUE(Diagnostics != nullptr);
        ASSERT_TRUE(Amanuensis::Json::Size(Amanuensis::Json::Get(Amanuensis::Json::Get(*Diagnostics, "params"),
                                                                   "diagnostics")) == 0);
    });
});

DESCRIBE("Server.completion", {
    IT("offers Core primitive tag names right after '<'", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        // Line 6 (1-based), "            <Frame />" -- position right after '<' is
        // 0-based line 5, character 12.
        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "textDocument/completion", MakePositionParams(FileUri, 5, 13)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Items = Amanuensis::Json::Get(Amanuensis::Json::Get(*Reply, "result"), "items");
        bool                       FoundFrame = false;
        for (std::size_t Index = 0; Index < Amanuensis::Json::Size(Items); ++Index) {
            if (Amanuensis::Json::AsString(Amanuensis::Json::Get(Amanuensis::Json::At(Items, Index), "label")) ==
                "Frame") {
                FoundFrame = true;
            }
        }
        ASSERT_TRUE(FoundFrame);
    });

    IT("offers prop names in an attribute position", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        // Line 5, "        <Frame class=\"a\">" -- position right after "<Frame " is
        // 0-based line 4, character 15.
        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "textDocument/completion", MakePositionParams(FileUri, 4, 15)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Items = Amanuensis::Json::Get(Amanuensis::Json::Get(*Reply, "result"), "items");
        bool                       FoundClass = false;
        for (std::size_t Index = 0; Index < Amanuensis::Json::Size(Items); ++Index) {
            if (Amanuensis::Json::AsString(Amanuensis::Json::Get(Amanuensis::Json::At(Items, Index), "label")) ==
                "class") {
                FoundClass = true;
            }
        }
        ASSERT_TRUE(FoundClass);
    });
});

DESCRIBE("Server.definition", {
    IT("jumps from 'import Button' to Button.iris's own declaration", {
        TempProject Project("import_goto_def");
        Project.WriteFile("Button.iris", "#include \"Iris/Component.h\"\n"
                                          "using Iris::Component;\n"
                                          "Component Button() {\n"
                                          "    render { <Frame /> }\n"
                                          "}\n");
        const std::string Source = "#include \"Iris/Component.h\"\n"
                                    "import Button\n"
                                    "using Iris::Component;\n"
                                    "Component UsesButton() {\n"
                                    "    render { <Button /> }\n"
                                    "}\n";
        const std::string FileUri = Uri(Project.PathOf("UsesButton.iris"));

        // Line 2 (1-based) is "import Button" -- 0-based line 1, character 7 lands
        // inside "Button".
        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 1, 7)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Location = Amanuensis::Json::Get(*Reply, "result");
        ASSERT_TRUE(Contains(Amanuensis::Json::AsString(Amanuensis::Json::Get(Location, "uri")), "Button.iris"));
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::Get(
                        Amanuensis::Json::Get(Amanuensis::Json::Get(Location, "range"), "start"), "line")) ==
                    2); // 0-based -- "Component Button("
    });

    IT("jumps from a <Button /> usage to the same declaration goto-def on the import finds", {
        TempProject Project("tag_usage_goto_def");
        Project.WriteFile("Button.iris", "#include \"Iris/Component.h\"\n"
                                          "using Iris::Component;\n"
                                          "Component Button() {\n"
                                          "    render { <Frame /> }\n"
                                          "}\n");
        const std::string Source = "#include \"Iris/Component.h\"\n"
                                    "import Button\n"
                                    "using Iris::Component;\n"
                                    "Component UsesButton() {\n"
                                    "    render { <Button /> }\n"
                                    "}\n";
        const std::string FileUri = Uri(Project.PathOf("UsesButton.iris"));

        // Line 5 (1-based) is "    render { <Button /> }" -- 0-based line 4, character
        // 16 lands inside "Button" in the tag usage, not the import statement.
        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 4, 16)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Location = Amanuensis::Json::Get(*Reply, "result");
        ASSERT_TRUE(Contains(Amanuensis::Json::AsString(Amanuensis::Json::Get(Location, "uri")), "Button.iris"));
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::Get(
                        Amanuensis::Json::Get(Amanuensis::Json::Get(Location, "range"), "start"), "line")) == 2);
    });

    IT("returns null for a Core primitive tag usage -- there is nothing to jump to", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        // Line 6 (1-based), "            <Frame />" -- 0-based line 5, character 14
        // lands inside "Frame".
        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 5, 14)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        ASSERT_TRUE(Amanuensis::Json::IsNull(Amanuensis::Json::Get(*Reply, "result")));
    });

    IT("jumps from a class=\"card\" value to the .card selector in the paired Name.lustre file", {
        TempProject Project("class_goto_def");
        Project.WriteFile("Foo.lustre", ".other {\n}\n\n.card {\n  background-color: #fff;\n}\n");
        const std::string Source = "#include \"Iris/Component.h\"\n"
                                    "using Iris::Component;\n"
                                    "Component Foo() {\n"
                                    "    render { <Frame class=\"card\"> </Frame> }\n"
                                    "}\n";
        const std::string FileUri = Uri(Project.PathOf("Foo.iris"));

        // Line 4 (1-based) is "    render { <Frame class=\"card\"> </Frame> }" -- 0-based
        // line 3, character 28 lands inside "card".
        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 3, 28)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Location = Amanuensis::Json::Get(*Reply, "result");
        ASSERT_TRUE(Contains(Amanuensis::Json::AsString(Amanuensis::Json::Get(Location, "uri")), "Foo.lustre"));
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::Get(
                        Amanuensis::Json::Get(Amanuensis::Json::Get(Location, "range"), "start"), "line")) ==
                    3); // 0-based -- ".card {"
    });

    IT("falls back to global.lustre when the paired Name.lustre doesn't define the class", {
        TempProject Project("class_goto_def_fallback");
        Project.WriteFile("Foo.lustre", ".other {\n}\n");
        Project.WriteFile("global.lustre", ".card {\n  background-color: #fff;\n}\n");
        const std::string Source = "#include \"Iris/Component.h\"\n"
                                    "using Iris::Component;\n"
                                    "Component Foo() {\n"
                                    "    render { <Frame class=\"card\"> </Frame> }\n"
                                    "}\n";
        const std::string FileUri = Uri(Project.PathOf("Foo.iris"));

        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 3, 28)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Location = Amanuensis::Json::Get(*Reply, "result");
        ASSERT_TRUE(Contains(Amanuensis::Json::AsString(Amanuensis::Json::Get(Location, "uri")), "global.lustre"));
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::Get(
                        Amanuensis::Json::Get(Amanuensis::Json::Get(Location, "range"), "start"), "line")) ==
                    0); // 0-based -- ".card {"
    });

    IT("returns null for a class value with no selector in either Name.lustre or global.lustre", {
        TempProject Project("class_goto_def_missing");
        Project.WriteFile("Foo.lustre", ".other {\n}\n");
        const std::string Source = "#include \"Iris/Component.h\"\n"
                                    "using Iris::Component;\n"
                                    "Component Foo() {\n"
                                    "    render { <Frame class=\"card\"> </Frame> }\n"
                                    "}\n";
        const std::string FileUri = Uri(Project.PathOf("Foo.iris"));

        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 3, 28)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        ASSERT_TRUE(Amanuensis::Json::IsNull(Amanuensis::Json::Get(*Reply, "result")));
    });
});

DESCRIBE("Server.semanticTokens", {
    IT("emits Type/Property/String tokens for render{}'s tags/props/string values", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        const auto         Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Json::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "textDocument/semanticTokens/full", MakeTextDocumentParams(FileUri)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Data = Amanuensis::Json::Get(Amanuensis::Json::Get(*Reply, "result"), "data");
        // SimpleComponentSource has one outer <Frame class="a">...</Frame> (opening tag,
        // prop, string, closing tag) wrapping one inner self-closing <Frame /> (opening
        // tag only, no closing tag) -- 5 tokens, 5 wire-format integers each.
        REQUIRE_TRUE(Amanuensis::Json::Size(Data) == 25);
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::At(Data, 3)) == 0);  // outer opening tag -- Type
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::At(Data, 8)) == 1);  // "class" -- Property
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::At(Data, 13)) == 2); // "a" -- String
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::At(Data, 18)) == 0); // inner opening tag -- Type
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::At(Data, 23)) == 0); // outer closing tag -- Type
    });

    IT("declares its legend during initialize", {
        const auto Outputs = RunServer({MakeRequest(1, "initialize", Amanuensis::Json::MakeObject()),
                                         MakeRequest(2, "shutdown", Amanuensis::Value()),
                                         MakeMessage("exit", Amanuensis::Value())});
        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 1);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& TokenTypes = Amanuensis::Json::Get(
            Amanuensis::Json::Get(
                Amanuensis::Json::Get(
                    Amanuensis::Json::Get(Amanuensis::Json::Get(*Reply, "result"), "capabilities"),
                    "semanticTokensProvider"),
                "legend"),
            "tokenTypes");
        REQUIRE_TRUE(Amanuensis::Json::Size(TokenTypes) == 3);
        ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::At(TokenTypes, 0)) == "type");
        ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::At(TokenTypes, 1)) == "property");
        ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::At(TokenTypes, 2)) == "string");
    });
});
