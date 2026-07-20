// iris-lsp: an LSP server for `.iris`/`.irisx` files, stdio transport only (the one
// transport every LSP client, including Neovim's built-in client, supports without extra
// configuration). See docs/iris_lsp_decision.md for the architecture this implements —
// render{}-scoped diagnostics/completion/goto-definition are Iris's own, direct from
// `libiris`; everything else is delegated to a real host-language server through
// IHostLanguageServerProxy (ClangdProxy for `.iris` today).
//
// Usage: iris-lsp   (no arguments — LSP servers are always driven entirely by the
// initialize request's own params, never argv)

#include "Server.h"

int main() {
    IrisLsp::Server Server;
    Server.Run(stdin, stdout);
    return 0;
}
