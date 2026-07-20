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
    Amanuensis::Value Message = Amanuensis::Value::MakeObject();
    Message.Insert("jsonrpc", Amanuensis::Value("2.0"));
    Message.Insert("method", Amanuensis::Value(std::move(Method)));
    Message.Insert("params", std::move(Params));
    return Message;
}

Amanuensis::Value MakeRequest(int Id, std::string Method, Amanuensis::Value Params) {
    Amanuensis::Value Message = MakeMessage(std::move(Method), std::move(Params));
    Message.Insert("id", Amanuensis::Value(Id));
    return Message;
}

Amanuensis::Value MakeTextDocument(const std::string& FileUri, const std::string& Text) {
    Amanuensis::Value TextDocument = Amanuensis::Value::MakeObject();
    TextDocument.Insert("uri", Amanuensis::Value(FileUri));
    TextDocument.Insert("languageId", Amanuensis::Value("cpp"));
    TextDocument.Insert("version", Amanuensis::Value(1));
    TextDocument.Insert("text", Amanuensis::Value(Text));
    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("textDocument", std::move(TextDocument));
    return Params;
}

Amanuensis::Value MakeTextDocumentParams(const std::string& FileUri) {
    Amanuensis::Value TextDocument = Amanuensis::Value::MakeObject();
    TextDocument.Insert("uri", Amanuensis::Value(FileUri));
    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("textDocument", std::move(TextDocument));
    return Params;
}

Amanuensis::Value MakePositionParams(const std::string& FileUri, std::uint32_t Line0, std::uint32_t Char0) {
    Amanuensis::Value TextDocument = Amanuensis::Value::MakeObject();
    TextDocument.Insert("uri", Amanuensis::Value(FileUri));
    Amanuensis::Value Position = Amanuensis::Value::MakeObject();
    Position.Insert("line", Amanuensis::Value(static_cast<long long>(Line0)));
    Position.Insert("character", Amanuensis::Value(static_cast<long long>(Char0)));
    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("textDocument", std::move(TextDocument));
    Params.Insert("position", std::move(Position));
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
        if (Message.Contains("method") && Message.Get("method").AsString() == Method) {
            return &Message;
        }
    }
    return nullptr;
}

const Amanuensis::Value* FindReplyToId(const std::vector<Amanuensis::Value>& Messages, int Id) {
    for (const Amanuensis::Value& Message : Messages) {
        if (Message.Contains("id") && Message.Get("id").IsInteger() && Message.Get("id").AsInteger() == Id) {
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
            RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Value::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Diagnostics = FindByMethod(Outputs, "textDocument/publishDiagnostics");
        REQUIRE_TRUE(Diagnostics != nullptr);
        const Amanuensis::Value& List = Diagnostics->Get("params").Get("diagnostics");
        REQUIRE_TRUE(List.Size() == 1);
        ASSERT_TRUE(Contains(List.At(0).Get("message").AsString(), "not a known prop"));
    });

    IT("publishes no diagnostics for a valid file", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        const auto         Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Value::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Diagnostics = FindByMethod(Outputs, "textDocument/publishDiagnostics");
        REQUIRE_TRUE(Diagnostics != nullptr);
        ASSERT_TRUE(Diagnostics->Get("params").Get("diagnostics").Size() == 0);
    });
});

DESCRIBE("Server.completion", {
    IT("offers Core primitive tag names right after '<'", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        // Line 6 (1-based), "            <Frame />" -- position right after '<' is
        // 0-based line 5, character 12.
        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Value::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "textDocument/completion", MakePositionParams(FileUri, 5, 13)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Items = Reply->Get("result").Get("items");
        bool                       FoundFrame = false;
        for (std::size_t Index = 0; Index < Items.Size(); ++Index) {
            if (Items.At(Index).Get("label").AsString() == "Frame") {
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
            RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Value::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "textDocument/completion", MakePositionParams(FileUri, 4, 15)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Items = Reply->Get("result").Get("items");
        bool                       FoundClass = false;
        for (std::size_t Index = 0; Index < Items.Size(); ++Index) {
            if (Items.At(Index).Get("label").AsString() == "class") {
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
            RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Value::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 1, 7)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Location = Reply->Get("result");
        ASSERT_TRUE(Contains(Location.Get("uri").AsString(), "Button.iris"));
        ASSERT_TRUE(Location.Get("range").Get("start").Get("line").AsInteger() == 2); // 0-based -- "Component Button("
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
            RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Value::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, Source)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 4, 16)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Location = Reply->Get("result");
        ASSERT_TRUE(Contains(Location.Get("uri").AsString(), "Button.iris"));
        ASSERT_TRUE(Location.Get("range").Get("start").Get("line").AsInteger() == 2);
    });

    IT("returns null for a Core primitive tag usage -- there is nothing to jump to", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        // Line 6 (1-based), "            <Frame />" -- 0-based line 5, character 14
        // lands inside "Frame".
        const auto Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Value::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "textDocument/definition", MakePositionParams(FileUri, 5, 14)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        ASSERT_TRUE(Reply->Get("result").IsNull());
    });
});

DESCRIBE("Server.semanticTokens", {
    IT("emits Type/Property/String tokens for render{}'s tags/props/string values", {
        const std::string FileUri = Uri("/scratch/Foo.iris");
        const auto         Outputs =
            RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                       MakeMessage("initialized", Amanuensis::Value::MakeObject()),
                       MakeMessage("textDocument/didOpen", MakeTextDocument(FileUri, SimpleComponentSource)),
                       MakeRequest(2, "textDocument/semanticTokens/full", MakeTextDocumentParams(FileUri)),
                       MakeRequest(3, "shutdown", Amanuensis::Value()), MakeMessage("exit", Amanuensis::Value())});

        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 2);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& Data = Reply->Get("result").Get("data");
        // SimpleComponentSource has one outer <Frame class="a"> (tag, prop, string) and
        // one inner <Frame /> (tag only) -- 4 tokens, 5 wire-format integers each.
        REQUIRE_TRUE(Data.Size() == 20);
        ASSERT_TRUE(Data.At(3).AsInteger() == 0);  // outer tag -- Type
        ASSERT_TRUE(Data.At(8).AsInteger() == 1);  // "class" -- Property
        ASSERT_TRUE(Data.At(13).AsInteger() == 2); // "a" -- String
        ASSERT_TRUE(Data.At(18).AsInteger() == 0); // inner tag -- Type
    });

    IT("declares its legend during initialize", {
        const auto Outputs = RunServer({MakeRequest(1, "initialize", Amanuensis::Value::MakeObject()),
                                         MakeRequest(2, "shutdown", Amanuensis::Value()),
                                         MakeMessage("exit", Amanuensis::Value())});
        const Amanuensis::Value* Reply = FindReplyToId(Outputs, 1);
        REQUIRE_TRUE(Reply != nullptr);
        const Amanuensis::Value& TokenTypes =
            Reply->Get("result").Get("capabilities").Get("semanticTokensProvider").Get("legend").Get("tokenTypes");
        REQUIRE_TRUE(TokenTypes.Size() == 3);
        ASSERT_TRUE(TokenTypes.At(0).AsString() == "type");
        ASSERT_TRUE(TokenTypes.At(1).AsString() == "property");
        ASSERT_TRUE(TokenTypes.At(2).AsString() == "string");
    });
});
