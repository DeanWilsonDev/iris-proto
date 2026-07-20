-- Minimal Neovim wiring for iris-lsp (docs/iris_lsp_decision.md §6). Source this from
-- init.lua, or copy it into an existing config -- there's no plugin to install, just
-- Neovim's own built-in LSP client (vim.lsp.start), same as any stdio LSP server.
--
-- Requires `iris_lsp` on PATH (built via `cmake --build build --target iris_lsp` in this
-- repo) and, for host-language (non-render{}) completion/goto-definition, `clangd` on
-- PATH too -- iris-lsp degrades gracefully (render{}/import features still work) if
-- clangd isn't found.
--
-- Syntax highlighting is separate -- see iris-treesitter.lua alongside this file. Its
-- own gap (render{}'s JSX-flavored tags, which the cpp grammar can't parse) is covered
-- by this server's own semantic tokens (docs/iris_lsp_decision.md §7) -- nothing extra
-- to configure for that; Neovim applies them automatically once attached.

vim.filetype.add({ extension = { iris = "iris", irisx = "iris" } })

vim.api.nvim_create_autocmd("FileType", {
  pattern = "iris",
  callback = function(args)
    vim.lsp.start({
      name = "iris-lsp",
      cmd = { "iris_lsp" },
      root_dir = vim.fs.root(args.buf, { ".iris.json" }),
    })
  end,
})
