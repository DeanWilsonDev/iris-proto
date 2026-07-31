#include "cimmerian/test.hpp"

#include "JsonRpc.h"

#include <cstdio>

DESCRIBE("JsonRpc", {
    IT("round-trips a message through Content-Length framing", {
        std::FILE* Temp = std::tmpfile();
        REQUIRE_TRUE(Temp != nullptr);

        Amanuensis::Value Message = Amanuensis::Json::MakeObject();
        Amanuensis::Json::Insert(Message, "jsonrpc", Amanuensis::Value("2.0"));
        Amanuensis::Json::Insert(Message, "id", Amanuensis::Value(static_cast<long long>(42)));
        Amanuensis::Json::Insert(Message, "method", Amanuensis::Value("test/echo"));
        IrisLsp::JsonRpc::WriteMessage(Temp, Message);

        std::rewind(Temp);
        const auto Read = IrisLsp::JsonRpc::ReadMessage(Temp);
        REQUIRE_TRUE(Read.has_value());
        ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::Get(*Read, "method")) == "test/echo");
        ASSERT_TRUE(Amanuensis::Json::AsInteger(Amanuensis::Json::Get(*Read, "id")) == 42);
        std::fclose(Temp);
    });

    IT("reads two consecutive messages off the same stream", {
        std::FILE* Temp = std::tmpfile();
        REQUIRE_TRUE(Temp != nullptr);

        Amanuensis::Value First = Amanuensis::Json::MakeObject();
        Amanuensis::Json::Insert(First, "method", Amanuensis::Value("first"));
        Amanuensis::Value Second = Amanuensis::Json::MakeObject();
        Amanuensis::Json::Insert(Second, "method", Amanuensis::Value("second"));
        IrisLsp::JsonRpc::WriteMessage(Temp, First);
        IrisLsp::JsonRpc::WriteMessage(Temp, Second);

        std::rewind(Temp);
        const auto Read1 = IrisLsp::JsonRpc::ReadMessage(Temp);
        const auto Read2 = IrisLsp::JsonRpc::ReadMessage(Temp);
        REQUIRE_TRUE(Read1.has_value());
        REQUIRE_TRUE(Read2.has_value());
        ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::Get(*Read1, "method")) == "first");
        ASSERT_TRUE(Amanuensis::Json::AsString(Amanuensis::Json::Get(*Read2, "method")) == "second");
        std::fclose(Temp);
    });

    IT("ReadMessage returns nullopt at EOF with nothing written", {
        std::FILE* Temp = std::tmpfile();
        REQUIRE_TRUE(Temp != nullptr);
        std::rewind(Temp);
        ASSERT_FALSE(IrisLsp::JsonRpc::ReadMessage(Temp).has_value());
        std::fclose(Temp);
    });
});
