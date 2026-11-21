# Editor, Terminal & Agent Integrations

## 1. Install Binary & Resident Daemon

### Linux (systemd user service)
```bash
make -j$(nproc) && make install
mkdir -p ~/.config/systemd/user
cp contrib/scryd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now scryd.service

# Register your repositories (persisted in ~/.config/scry/repos.txt):
scry watch ~/src/linux
scry watch ~/src/llvm-project
```

### macOS (launchd user agent, native FSEvents)
```bash
make -j$(sysctl -n hw.ncpu) && sudo make PREFIX=/usr/local install
cp contrib/com.scry.daemon.plist ~/Library/LaunchAgents/
launchctl load -w ~/Library/LaunchAgents/com.scry.daemon.plist
scry watch ~/src/my-monorepo
```

---

## 2. Neovim (`telescope.nvim`)

Drop-in replacement for `ripgrep` inside Telescope's `live_grep` and `grep_string`:

```lua
require("telescope").setup({
  defaults = {
    vimgrep_arguments = {
      "scry",
      "--color=never",
      "--no-heading",
      "--with-filename",
      "--line-number",
      "--column",
      "--smart-case",
    },
  },
})
```

---

## 3. Vim / Neovim `:grep` (`grepprg`)

```vim
set grepprg=scry\ --vimgrep\ --smart-case
set grepformat=%f:%l:%c:%m
```

---

## 4. `tern` / Shell Hotkey for Zero-Lag Interactive Picker (`scry -I`)

Bind `Ctrl-F` in `zsh` / `bash` (or inside `tern`) to open `scry -I` and jump directly to `$EDITOR` or `zed`:

```bash
# In ~/.zshrc
scry_widget() {
  local target
  target="$(scry -I)"
  if [[ -n "$target" ]]; then
    if command -v zed >/dev/null 2>&1; then
      zed "$target"
    else
      local file="${target%%:*}"
      local rest="${target#*:}"
      local line="${rest%%:*}"
      "${EDITOR:-vi}" "+${line}" "$file"
    fi
  fi
  zle reset-prompt
}
zle -N scry_widget
bindkey '^F' scry_widget
```

---

## 5. `fzf` Live Grep Integration

```bash
scry-fzf() {
  fzf --ansi --disabled --query "${1:-}" \
    --bind "change:reload:scry --color=always -n -H -S {q} || true" \
    --delimiter : \
    --preview 'nl -ba {1} | sed -n "$(( {2} > 10 ? {2}-10 : 1 )),$(( {2}+20 ))p"'
}
```

---

## 6. AI Coding Agents (Claude Code / Codex / Devin / Scripts)

`scry` speaks standard `rg` flags (`-i`, `-S`, `-F`, `-w`, `-e`, `-g`, `-t`, `-C`, `-c`, `-l`, `-q`, `--files`, `--json`, `--vimgrep`, stdin piping, and multiple paths), so scripts and coding agents can invoke `scry` directly or alias `rg=scry` for 30×–120× faster searches on watched monorepos.
