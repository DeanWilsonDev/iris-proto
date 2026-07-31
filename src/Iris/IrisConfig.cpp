#include "Iris/IrisConfig.h"

#include <amanuensis/io/reader.hpp>
#include <amanuensis/json.hpp>
#include <amanuensis/value.hpp>

namespace Iris {

IrisConfigParseResult ParseIrisConfig(std::string_view JsonText) {
    IrisConfigParseResult Result;

    const Amanuensis::ParseResult Parsed = Amanuensis::Reader::ParseString(JsonText);
    if (!Parsed.succeeded || !Amanuensis::Json::IsObject(Parsed.value)) {
        Result.Errors.push_back({"`.iris.json` is not valid JSON."});
        return Result;
    }

    const Amanuensis::Value& Root = Parsed.value;
    IrisConfig                Config;

    const Amanuensis::Value* TargetValue = Amanuensis::Json::Find(Root, "target");
    if (TargetValue == nullptr || !Amanuensis::Json::IsString(*TargetValue)) {
        Result.Errors.push_back({"`.iris.json` is missing required field `target`."});
    } else {
        const std::string& TargetStr = Amanuensis::Json::AsString(*TargetValue);
        if (TargetStr == "penumbra") {
            Config.Target = IrisBuildTarget::Penumbra;
        } else if (TargetStr == "umbra-engine") {
            Config.Target = IrisBuildTarget::UmbraEngine;
        } else {
            Result.Errors.push_back(
                {"`.iris.json` field `target` must be \"penumbra\" or \"umbra-engine\", got \"" + TargetStr +
                 "\"."});
        }
    }

    const Amanuensis::Value* VersionValue = Amanuensis::Json::Find(Root, "version");
    if (VersionValue == nullptr || !Amanuensis::Json::IsString(*VersionValue)) {
        Result.Errors.push_back({"`.iris.json` is missing required field `version`."});
    } else {
        Config.Version = Amanuensis::Json::AsString(*VersionValue);
    }

    const Amanuensis::Value* SearchPathsValue = Amanuensis::Json::Find(Root, "searchPaths");
    if (SearchPathsValue == nullptr || !Amanuensis::Json::IsArray(*SearchPathsValue)) {
        Result.Errors.push_back({"`.iris.json` is missing required field `searchPaths`."});
    } else {
        bool AllStrings = true;
        for (const Amanuensis::Value& Entry : Amanuensis::Json::AsArray(*SearchPathsValue)) {
            if (!Amanuensis::Json::IsString(Entry)) {
                AllStrings = false;
                break;
            }
            Config.SearchPaths.push_back(Amanuensis::Json::AsString(Entry));
        }
        if (!AllStrings) {
            Result.Errors.push_back({"`.iris.json` field `searchPaths` must be a list of strings."});
        }
    }

    if (!Result.Errors.empty()) {
        return Result;
    }

    Result.Config = std::move(Config);
    return Result;
}

} // namespace Iris
