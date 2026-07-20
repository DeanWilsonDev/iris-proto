#include "RenderTextHeuristics.h"

#include <algorithm>
#include <cctype>

namespace IrisLsp {

std::string_view LineText(std::string_view Text, std::uint32_t Line) {
    std::uint32_t Current = 1;
    std::size_t   Start = 0;
    for (std::size_t Index = 0; Index <= Text.size(); ++Index) {
        if (Current == Line && Start == 0 && (Index == 0 || Text[Index - 1] == '\n')) {
            Start = Index;
        }
        if (Index == Text.size() || Text[Index] == '\n') {
            if (Current == Line) {
                return Text.substr(Start, Index - Start);
            }
            ++Current;
        }
    }
    return {};
}

RenderCompletionKind ClassifyRenderCompletion(std::string_view Line, std::uint32_t ColumnOneBased) {
    const std::size_t CursorOffset = ColumnOneBased > 0 ? static_cast<std::size_t>(ColumnOneBased - 1) : 0;
    bool               SawWhitespace = false;
    std::size_t        Index = std::min(CursorOffset, Line.size());
    while (Index > 0) {
        --Index;
        const char C = Line[Index];
        if (C == '>') {
            return RenderCompletionKind::None;
        }
        if (C == '<') {
            return SawWhitespace ? RenderCompletionKind::AttributeName : RenderCompletionKind::TagName;
        }
        if (std::isspace(static_cast<unsigned char>(C)) != 0) {
            SawWhitespace = true;
        }
    }
    return RenderCompletionKind::None;
}

std::optional<std::string> TagNameAtPosition(std::string_view Line, std::uint32_t ColumnOneBased) {
    const std::size_t Cursor = ColumnOneBased > 0 ? static_cast<std::size_t>(ColumnOneBased - 1) : 0;

    for (std::size_t Index = 0; Index < Line.size(); ++Index) {
        if (Line[Index] != '<') {
            continue;
        }
        std::size_t NameStart = Index + 1;
        if (NameStart < Line.size() && Line[NameStart] == '/') {
            ++NameStart; // a closing tag, `</Name>` -- Name starts one further in
        }
        if (NameStart >= Line.size() ||
            !(std::isalpha(static_cast<unsigned char>(Line[NameStart])) != 0 || Line[NameStart] == '_')) {
            continue; // not an identifier start -- e.g. `<=`, `<<`, or a stray '<' in an escape hatch
        }
        std::size_t NameEnd = NameStart;
        while (NameEnd < Line.size() &&
               (std::isalnum(static_cast<unsigned char>(Line[NameEnd])) != 0 || Line[NameEnd] == '_')) {
            ++NameEnd;
        }
        // Inclusive of the opening '<'/'</' so clicking right on it (a common goto-def
        // habit) still resolves, and inclusive of NameEnd so clicking just past the last
        // character still counts, matching typical "word under cursor" LSP UX.
        if (Cursor >= Index && Cursor <= NameEnd) {
            return std::string(Line.substr(NameStart, NameEnd - NameStart));
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> FindComponentDeclaration(const std::string& Text,
                                                                                  const std::string& Name) {
    std::size_t Pos = 0;
    while ((Pos = Text.find(Name, Pos)) != std::string::npos) {
        const bool WordStartOk = Pos == 0 || (std::isalnum(static_cast<unsigned char>(Text[Pos - 1])) == 0 &&
                                               Text[Pos - 1] != '_');
        std::size_t After = Pos + Name.size();
        const bool  WordEndOk = After >= Text.size() || (std::isalnum(static_cast<unsigned char>(Text[After])) == 0 &&
                                                          Text[After] != '_');
        if (WordStartOk && WordEndOk) {
            while (After < Text.size() && std::isspace(static_cast<unsigned char>(Text[After])) != 0) {
                ++After;
            }
            if (After < Text.size() && Text[After] == '(') {
                std::uint32_t Line = 1;
                std::uint32_t Column = 1;
                for (std::size_t I = 0; I < Pos; ++I) {
                    if (Text[I] == '\n') {
                        ++Line;
                        Column = 1;
                    } else {
                        ++Column;
                    }
                }
                return std::make_pair(Line, Column);
            }
        }
        Pos += Name.size();
    }
    return std::nullopt;
}

} // namespace IrisLsp
