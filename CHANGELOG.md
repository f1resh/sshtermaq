# Changelog

All notable changes to SSHTermaq. Newest first.

## [1.1.0] - 2026-11-14

### Added
- Local terminals: shells on your computer in tabs and split panes (Windows: Command Prompt, PowerShell, Git Bash,
  Cygwin, MSYS2, WSL; macOS: your login shell and those in `/etc/shells`), from a shortcut, the + menu, quick connect
  (`local:bash`) or the command line (`-local`).
- Upload as… / Download as…: copy or move a file or folder under a new name.

### Changed
- Tabs and panes can be renamed (Ctrl+Shift+R / ⇧⌘R, double-click a pane's title); names are kept after a restart.
- Clearer error reports: an error page instead of an empty window, and connections and errors in the app log.
- "Leave out .DS_Store and ._ files" is off by default on new Windows installations.
- Windows: the Mica / Acrylic window backgrounds are switched off for now (the window buttons appeared twice); they
  will return.

### Fixed
- Tabs keep their width instead of shrinking to their title.
- macOS: serial examples show `/dev/cu.*` instead of `COM3`; the saved-password text names the Keychain instead of
  Windows DPAPI.

### Known issues
- Folders with thousands of small files download slowly from distant servers (one file at a time).

## [1.0.0] - 2026-10-20

### Added
- First release, for Windows and macOS: SSH-2, Telnet, Rlogin, Raw and serial connections with PuTTY-style settings,
  `.ppk` keys and PuTTY's command-line options.
- One-click import of PuTTY sessions and host keys (registry, `.reg` files, `~/.putty`) and of `~/.ssh/config`.
- Passwords, key files, keyboard-interactive (2FA), Pageant and ssh-agent; saved passwords encrypted by the system.
- Local, remote and dynamic port forwarding, agent and X11 forwarding, jump hosts and proxies.
- Tabs, split panes, input broadcast, command palette, quick connect, session folders, themes and colour schemes.
- SFTP panel and two-pane file manager: drag and drop, transfer queue, resume, edit on the server, folder sync,
  search, bookmarks; SCP for servers without SFTP.
- `ssh://`, `sftp://`, `scp://` and `telnet://` links.
