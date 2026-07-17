#include "Iris/IrisConfig.h"

#include <amanuensis/io/reader.hpp>
#include <amanuensis/value.hpp>

namespace Iris {

IrisConfigParseResult ParseIrisConfig(std::string_view JsonText) {
    IrisConfigParseResult Result;

    const Amanuensis::ParseResult Parsed = Amanuensis::Reader::ParseString(JsonText);
    if (!Parsed.succeeded || !Parsed.value.IsObject()) {
        Result.Errors.push_back({"`.iris.json` is not valid JSON."});
        return Result;
    }

    const Amanuensis::Value& Root = Parsed.value;
    IrisConfig                Config;

    const Amanuensis::Value* TargetValue = Root.Find("target");
    if (TargetValue == nullptr || !TargetValue->IsString()) {
        Result.Errors.push_back({"`.iris.json` is missing required field `target`."});
    } else {
        const std::string& TargetStr = TargetValue->AsString();
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

    const Amanuensis::Value* VersionValue = Root.Find("version");
    if (VersionValue == nullptr || !VersionValue->IsString()) {
        Result.Errors.push_back({"`.iris.json` is missing required field `version`."});
    } else {
        Config.Version = VersionValue->AsString();
    }

    const Amanuensis::Value* SearchPathsValue = Root.Find("searchPaths");
    if (SearchPathsValue == nullptr || !SearchPathsValue->IsArray()) {
        Result.Errors.push_back({"`.iris.json` is missing required field `searchPaths`."});
    } else {
        bool AllStrings = true;
        for (const Amanuensis::Value& Entry : SearchPathsValue->AsArray()) {
            if (!Entry.IsString()) {
                AllStrings = false;
                break;
            }
            Config.SearchPaths.push_back(Entry.AsString());
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
