-- Treesitter syntax highlighting for .iris/.irisx via the "cpp" grammar
-- (docs/iris_lsp_decision.md §6). No .iris-specific tree-sitter grammar exists -- most of
-- a .iris file already IS valid C++23 syntax outside render{} blocks, so writing one from
-- scratch would mostly just re-derive tree-sitter-cpp for little benefit. The render{}
-- block's JSX-flavored tags (`<Tag prop="x">`) aren't valid C++ syntax, so tree-sitter's
-- own error recovery would produce ERROR nodes around them if left alone -- that gap is
-- closed by iris-lsp's own semantic tokens instead (§7 -- see iris-lsp.lua alongside this
-- file), which Neovim's built-in LSP client applies automatically once the server attaches
-- and advertises semanticTokensProvider, no extra config needed here.
--
-- Requires the `cpp` parser installed (:TSInstall cpp, or add "cpp" to nvim-treesitter's
-- own ensure_installed list if you manage parsers that way).

vim.treesitter.language.register("cpp", "iris")

vim.api.nvim_create_autocmd("FileType", {
  pattern = "iris",
  callback = function(args)
    pcall(vim.treesitter.start, args.buf, "cpp")
  end,
})
