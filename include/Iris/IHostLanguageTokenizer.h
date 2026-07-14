#pragma once

#include "Iris/Token.h"

namespace Iris {

// Abstraction over the host language's lexical syntax (strings, char literals,
// comments) so the Iris preprocessor core can detect `render {` blocks and
// balance `{ }` escape-hatch braces without any host-language-specific rules
// living in the preprocessor itself (docs/iris_core_spec.md §1.3,
// docs/iris_stage1_decision_doc_pt2.md §2). One concrete implementation per
// host language — CppTokenizer for `.iris` today, a future NyxTokenizer for
// `.irisx` once Nyx exists — selected by file extension at preprocessor
// startup. Swapping the implementation is the only change needed to support a
// new host language; nothing else in the preprocessor core touches
// host-language lexical rules.
class IHostLanguageTokenizer {
public:
    virtual ~IHostLanguageTokenizer() = default;

    // Returns the next token in the stream, advancing past it. Once the
    // source is exhausted, returns a Token with Kind == TokenKind::EndOfFile
    // on every subsequent call rather than throwing or asserting.
    virtual Token NextToken() = 0;
};

} // namespace Iris
