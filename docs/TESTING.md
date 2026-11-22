# Testing SSHTermaq

How SSHTermaq is tested: the tools, how the tests are organised, the full list of tests, and how to run each part.
Setting up the build environment is covered in [BUILDING.md](BUILDING.md).

## 1. Overview

The tests are split into three independent parts. They use different languages, frameworks and folders, and none of
them depends on another.

| Part | What it tests | Framework | Where | Automated | Size | Run time |
|---|---|---|---|---|---|---|
| **Native tests** | the C++ backend: SSH, Telnet, Rlogin, Raw, local terminals, S3 (signing, sign-in, the HTTP client, the S3 file system), proxies, keys, PuTTY import, storage, sessions, file transfer, the host window policy, the platform layer (files, pseudo-terminals, drag & drop, local files) | Catch2 3, run by CTest | `native/tests/<module>/`, `native/tests/platform/` | yes | 13 suites, 330 test cases | about 2 minutes |
| **UI unit tests** | pure TypeScript logic of the React UI: keyboard mapping, quick connect parser, file manager paths and shortcuts, tab titles and persistence, pane closing, SFTP panel, focus and sidebar actions, built-in themes and colour schemes, version and build id in About, the macOS key bindings and menu bar, UI errors reported to the app log; the bridge contract between the UI, the native host and the mock backend; local terminal labels, sessions, restore, quick connect and reported folders; tab and pane names; the .DS_Store default per system; finished transfers' times; the font picker's lists; Credentials (which sessions and logins use a key or a login, how a login signs in, names; unlocking and retrying when they are locked; what an import did); the tabs a reloaded window gets back; host names (the cases shared with the native check); the question before a link connects; imported sessions that run a command on this computer; the WebGL renderer's font check (DOM for fonts a canvas can't draw) | Vitest 4 | `ui/src/**/*.test.ts`, next to the code | yes | 26 files, 247 tests | under 2 seconds |
| **UI browser tests** | the React UI in a real browser (Edge on Windows) against the mock backend, in the Windows and the macOS layout: welcome screen, title bar, session editor, sidebar, command palette, connecting, terminal input and output, quick connect, split panes, restarting after a lost connection, opening local terminals (also from quick connect), download / upload / move under a new name, renaming tabs and panes, names after a restart, the SFTP panel, the two-pane file manager, import and export, the key generator, settings, Credentials (keys, logins, remembered passphrases, the master password, export and import), links asking before they connect, S3 sessions, a changed host key | Playwright Test | `ui/e2e/*.spec.ts` | yes | 22 files, 102 tests, each run twice (204, of which 2 macOS-only skip in the Windows layout) | about 6 minutes |
| **End-to-end and visual checks** | the whole UI, either inside the real app or in a browser against the mock backend | Playwright scripts | `ui/scripts/app.mjs`, `ui/scripts/shot.mjs`, `ui/scripts/checks/` | no, run by hand | `checks/local-terminal.mjs`, `checks/terminal-fonts.mjs`, `checks/reload-and-second-start.mjs` | seconds |

The browser tests cover the main screens and dialogs, not yet the host key and password prompts or the color scheme
editor. There is no CI: tests run when a developer runs them, or through the build scripts:
`scripts\windows\build.ps1 -Tests` / `scripts/macos/build.sh --tests` run the core tests (native and UI unit tests),
`-E2E` / `--e2e` the end-to-end tests (UI typecheck and the browser tests). See
[section 8](#8-what-is-not-covered).

## 2. Tools and libraries

| Tool | Version | Used for |
|---|---|---|
| [Catch2](https://github.com/catchorg/Catch2) | 3.16 (vcpkg) | C++ test framework: `TEST_CASE`, `REQUIRE` / `CHECK`, tags, `SKIP`, its own `main()` (`Catch2::Catch2WithMain`) |
| CTest | ships with CMake | runs all native suites in one command |
| libssh server API | 0.12 (vcpkg) | the in-process SSH test server (`native/tests/support`) |
| [Vitest](https://vitest.dev/) | 4.1 | UI unit tests, run in Node (no browser, no DOM) |
| [Playwright Test](https://playwright.dev/docs/intro) (`@playwright/test`) | 1.62 | UI browser tests: the test runner, assertions, traces of failing tests; uses the installed Edge (Windows) or Chrome |
| TypeScript compiler | 5.9 | `npm run typecheck`: static check of the whole UI |
| [Playwright](https://playwright.dev/) (`playwright-core`) | 1.62 | end-to-end and screenshot scripts; drives Microsoft Edge or the app's WebView2 over the DevTools protocol |

Optional reference tools. The native tests use them to check our output against independent implementations, and
skip those test cases (they don't fail) when a tool is missing:

| Tool | Expected at | Checks |
|---|---|---|
| PuTTY 0.83: `plink`, `psftp`, `pageant` | `C:\Program Files\PuTTY\`; macOS: `PATH`, `/opt/homebrew/bin` (`brew install putty`) | PPK files we write log in with `plink`; the test server works with PuTTY's clients; Pageant agent login (Windows) |
| A shell with `ls` and `scp` | Git for Windows: `C:\Program Files\Git\usr\bin\`; macOS: `/bin/sh`, BSD `ls`, `/usr/bin/scp` (built in) | SCP mode against real `ls` (GNU or BSD) and OpenSSH `scp` |
| OpenSSH `ssh-keygen`, `ssh-agent`, `ssh-add` | `C:\Windows\System32\OpenSSH\`; macOS: `/usr/bin` (built in) | keys generated by `ssh-keygen` load correctly; agent login through `SSH_AUTH_SOCK` (macOS) |

## 3. How the tests are organised

### Native tests

- **One folder per module, one executable per folder.** `native/tests/<module>/*.cpp` builds into
  `st_test_<module>.exe`. Each executable links only its module's library (and that library's dependencies), the shared
  test helpers in `native/tests/support` and Catch2's `main()`. This is set up by `st_test(<module> <libraries>)`
  in [CMakeLists.txt](../CMakeLists.txt). New `.cpp` files in an existing folder are picked up automatically.
- **CTest knows each suite by its module name** (`app`, `core`, `ssh`, ...), so `ctest -R ssh` runs `st_test_ssh.exe`.
- **One more CTest entry, `platform_boundaries`,** is not a Catch2 suite: it runs
  [cmake/CheckPlatformBoundaries.cmake](../cmake/CheckPlatformBoundaries.cmake), which fails when a file under
  `native/src`, `native/tests` or `native/tools` outside the `platform` folders uses OS-specific code (OS headers,
  `#ifdef _WIN32` / `__APPLE__`, Win32 names such as `HANDLE` or `wchar_t`, `L"..."`, Objective-C). Such code belongs
  in `native/src/platform` ([its README](../native/src/platform/README.md)) or `native/tests/platform/<os>`.
  [cmake/platform_allowlist.txt](../cmake/platform_allowlist.txt) lists exceptions; it is empty now and may only
  shrink. Run it alone with `ctest -R platform_boundaries`, or `cmake -DSRC=. -P cmake/CheckPlatformBoundaries.cmake`
  from the repository root (`-DUPDATE=ON` also removes allowlist entries whose files are clean).
- **Tags** in square brackets group test cases inside a suite (`[ssh][forward]`, `[store][credentials]`) and are
  used for filtering. A tag starting with a dot (`[.registry]`) hides the test case unless it is asked for by name.
- **Shared test infrastructure** in [native/tests/support](../native/tests/support), linked into every test executable:

| File | Provides |
|---|---|
| `TestServers.h`, `TestSshServer.cpp` | `TestSshServer`: the in-process SSH server |
| `TestServers.h`, `TestServers.cpp` | `TestEchoServer`, `TestScriptServer`, `TempDir` (self-deleting temp folder), `posixShell()` (a shell with `ls` and `scp`: Git for Windows' `sh.exe`, `/bin/sh` elsewhere), `findTool()` (PuTTY's and OpenSSH's tools) |
| `TestSftpServer.*` | the small SFTP v3 server used by `TestSshServer`'s `sftp` subsystem, confined to one folder |
| `TestExecProcess.*` | runs real commands (through `posixShell()`) for the server's exec channels; the OS parts are in `native/tests/platform/<os>/support` |
| `TestTransport.h` | a recording `TransportListener` and a scriptable `Prompter` |
| `TestBridge.h` | a `Bridge` with an inline dispatcher: call a handler the way the UI would and collect the events it sends |
| `TestHttpServer.*` | an HTTP / HTTPS server (a fresh self-signed certificate) with several connections, keep-alive and a fault per answer: closed without an answer, cut after N bytes, stalled after N body bytes, delayed |
| `TestNet.*` | socket helpers shared by the test servers and the socket tests, on the platform layer; the raw ones (abortive close, urgent bytes, waiting on many sockets) are in `native/tests/platform/<os>/support` |
| `support_tests.cpp` | self-tests of all of the above |

The file manager tests add [native/tests/sftp/SftpFixture.h](../native/tests/sftp/SftpFixture.h): a connected SSH
session plus `SftpService` against the test server, driven through the bridge. The `app` tests also embed
the few small files of [native/tests/app/ui_test](../native/tests/app/ui_test) the way the real build embeds the web
UI (Windows: resources from [ui_test.rc](../native/tests/app/ui_test.rc); macOS: sections of the executable, packed
by `cmake/EmbedUi.cmake`), so loading the embedded UI is tested against the real mechanism.

### UI unit tests

- **No separate folder.** Each test file sits next to the code it tests, named `<file>.test.ts`.
- **Configuration** is in [ui/vite.config.ts](../ui/vite.config.ts), under `test`: environment `node`, files matching
  `src/**/*.test.ts` or `tests/**/*.test.ts` (the `ui/tests` folder does not exist yet).
- **Only pure logic is tested**, because there is no DOM: functions that turn input into output without React or the
  native host.

### UI coverage

`npm run coverage` (in `ui`, about 6 minutes) runs the unit tests and the browser tests with coverage and prints the
UI's line coverage by area: unit tests, browser tests and both (`ui/scripts/coverage.mjs`; the browser part is recorded
by `e2e/fixtures.ts` when `PW_COV` is set). `npm run coverage -- --report` prints the last table again. Output in
`ui/coverage/` (`summary.md`, `summary.json`; not in git). 2026-11-14: 69 % of 6,546 lines.

### UI browser tests

- **Folder:** [ui/e2e](../ui/e2e), files named `<area>.spec.ts`; configuration in
  [ui/playwright.config.ts](../ui/playwright.config.ts).
- **What runs:** Playwright starts Vite on port 5174 (not 5173, so a running `npm run dev` is not in the way) and
  opens the UI in the installed Edge on Windows, Chrome elsewhere (`PW_CHANNEL` overrides it). The UI runs against the
  mock backend, as in `npm run dev`.
- **Two layouts:** every test runs in the projects `windows` and `macos`. [e2e/fixtures.ts](../ui/e2e/fixtures.ts)
  makes the page report that system to `src/shared/platform.ts`, so the macOS layout (no caption buttons, native menu
  bar, ⌘ shortcuts, Finder / Keychain words) is tested on a Windows PC as well.
- **Fresh state:** each test starts from the mock's seed data: the fixture clears the site's stored data, where the
  mock keeps its state.
- **Browser profiles:** each test worker reuses one profile folder in the system temp folder
  (`sshtermaq-e2e-profile-<n>`). Playwright's own temporary profiles can't be used on Windows: Edge and Chrome give some
  of their folders permissions that only the browser's sandbox can use, and Playwright then hangs for minutes trying to
  delete them.
- **Finding elements:** by role and accessible name, as a user sees them (`getByRole('dialog', { name: … })`), not by
  CSS classes. Shared actions are in [e2e/helpers.ts](../ui/e2e/helpers.ts).
- **Playing the host:** for what only the native host does, the mock offers hooks:
  - `__mockEmit(channel, ...args)` sends an event as the host would (the macOS menu bar's `window:onMenuCommand`);
  - `__mockMenuBar` holds the menu bar the UI last sent;
  - `__mockDialog(kind, answer)` sets the answer of the next system file dialog (`openFile`, `openFiles`,
    `openDirectory`, `saveFile`, `saveTextFile`; otherwise they answer "cancelled"); helper `mockDialog`;
  - `__mockSaved` lists what `dialog:saveTextFile` wrote (path, content, owner-only); helper `savedFiles`;
  - `__mockClipboard` holds the last text the UI copied (`app:writeClipboard`), so no clipboard permission is needed.
- **Terminal text:** xterm.js draws on a canvas by default; tests that read the terminal switch to its DOM renderer
  first (`useDomRenderer`), whose rows are plain text.
- **Failures:** the list in the console, an HTML report in `ui/playwright-report`, and a trace of each failing test in
  `ui/test-results/<test>/trace.zip` (`npx playwright show-trace <file>`: every step with a snapshot of the page).

### End-to-end and visual scripts

Two Node scripts in [ui/scripts](../ui/scripts), described in [section 7](#7-end-to-end-and-visual-checks). They
are tools for checking things by hand: they have no assertions of their own and no saved scenarios.

### Between backend and frontend

The two halves talk over the JSON bridge (`ui/src/shared/ipc.ts`). Each side tests its half separately:

- **Native side:** `TestBridge` sends the same JSON requests the UI sends and checks the responses and events
  (`session manager: bridge handlers`, `localfs: bridge handlers`, the file manager tests).
- **UI side:** in a normal browser the UI talks to a mock backend (`ui/src/bridge/mock`) instead of the host; the
  browser tests run against it.
- **The contract between them:** [ui/src/bridge/contract.test.ts](../ui/src/bridge/contract.test.ts) checks that the
  native host (its handler registrations and `emit` calls, read from `native/src`) and the mock backend both answer
  exactly the channels of `API_SCHEMA`, with the right kind. A channel added on one side only fails this test.

Only the end-to-end script `app.mjs` exercises both halves together.

## 4. How the native tests work

There are four kinds of native test:

1. **Unit tests** of logic without I/O: charsets, command-line parsing, PuTTY settings mapping, `ls -l` parsing,
   conflict rules, storage JSON.
2. **Integration tests against servers running inside the test process**, on `127.0.0.1` with a random port.
   `TestSshServer` is a real SSH server built on libssh's server API. Each test configures it:
   - which logins it accepts: password, public keys, keyboard-interactive with several prompts, a pre-login banner;
   - host key type (Ed25519, RSA, ECDSA), SFTP on or off, port forwarding on or off, real command execution;
   - its shell answers simple scripted commands:

     | Command | Reply |
     |---|---|
     | any line | `echo: <line>` |
     | `exit <n>` | exit status *n*, then closes the channel |
     | `env <NAME>` | the value of an environment variable the client sent |
     | `size` / `term` | the terminal size / `TERM` from the PTY request |
     | `user` | the logged-in user name |
     | `big <n>` | *n* bytes of output, then `END` |
     | `signal` | waits for a signal and reports its name |

   - it records events the test can wait for or check: `auth:password:ok`, `pty:xterm-256color:80x24`,
     `window-change:100x30`, `signal:INT`, `direct-tcpip:127.0.0.1:1234`, `subsystem:sftp`, ...

   `TestEchoServer` (echoes every byte) and `TestScriptServer` (records input, sends scripted output) stand in for
   Telnet, Rlogin and Raw hosts and for port-forwarding targets.
3. **Test doubles** for the layers above the network: fake transports, a scripted prompter and the inline bridge from
   `TestBridge.h`. `SessionManager::setTransportFactory` lets the session manager tests plug in fake connections.
4. **Cross-checks with independent tools** (see [section 2](#2-tools-and-libraries)): our PPK files must work with
   `plink`, our PuTTY host-key conversion must give the same fingerprints as `ssh-keygen`, SCP mode runs against real
   `ls` and `scp`.

A typical integration test, from [ssh_tests.cpp](../native/tests/ssh/ssh_tests.cpp):

```cpp
TEST_CASE("ssh: password authentication, shell echo and exit code", "[ssh]") {
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer server(o);                 // real SSH server on 127.0.0.1, random port
    Harness h;
    h.start(Harness::session(server));       // our SshTransport connects to it
    REQUIRE(h.listener.waitConnected());
    REQUIRE(h.listener.waitData("$ "));
    h.transport->write("hello world\r");
    REQUIRE(h.listener.waitData("echo: hello world"));
    h.transport->write("exit 7\r");
    REQUIRE(h.listener.waitClosed());
    REQUIRE(h.listener.info.exitCode == 7);
    REQUIRE(server.waitForEvent("auth:password:ok", 1000));
    REQUIRE(server.waitForEvent("pty:xterm-256color:80x24", 1000));
}
```

**Rules every native test follows:**

- only `127.0.0.1` / `localhost`, never another host;
- files only in `TempDir` folders (`%TEMP%\st-test-<uuid>`), deleted when the test ends;
- no fixture files: test data is generated during the test, or embedded in the source as known-answer values;
- the real PuTTY registry may be read but never written; the one test that reads it is hidden and prints only counts;
- the Pageant test is skipped when a Pageant is already running, so your own agent is never touched.

## 5. What is tested

### Native tests (330 test cases)

326 run by default; `putty: real registry smoke test`, `codepage (Windows): regenerate the tables` and the two `proxy exit child` helpers are hidden. The **Needs** column shows the tool without which a
test case is skipped. On macOS every suite runs; its `platform_ui` has the macOS local files tests (listed after
Windows' below). The other counts are Windows'.

#### app — `st_test_app.exe` (38)

Command line (PuTTY-compatible options, `ssh://` / `sftp://` URLs, `-load`, `--version`), data-folder resolution (portable / installed / override), the version and build id (`BuildInfo`), the embedded web UI (index parsing, request paths, content types, loading the embedded test files), and the host window policy (`HostController` against a fake window: allowed pages and origins, window events (menu bar commands and title bar insets too), confirming close, saved placement), and what a reloaded page gets (`PageHandover`). *Kind: unit.* Source: [native/tests/app](../native/tests/app).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| cli: no arguments => no request | `[app][cli]` |  | `cli_tests.cpp` |
| cli: [user@]host[:port] uses the default template | `[app][cli]` |  | `cli_tests.cpp` |
| cli: protocol flags set the default port unless -P is given | `[app][cli]` |  | `cli_tests.cpp` |
| cli: sftp:// and scp:// open the file manager | `[app][cli]` |  | `cli_tests.cpp` |
| host names: the shared cases (the UI's isValidHost passes the same file) | `[app][cli][host]` | `native/tests/fixtures/host-names.json` | `cli_tests.cpp` |
| cli: a host with shell characters is refused, in a link and as an argument | `[app][cli][host]` |  | `cli_tests.cpp` |
| cli: user names in links are decoded; a password in a link is dropped | `[app][cli]` |  | `cli_tests.cpp` |
| cli: a link is marked, and does not take forwarding or tunnels from Default Settings | `[app][cli]` |  | `cli_tests.cpp` |
| cli: -l, -pw, -i, flags | `[app][cli]` |  | `cli_tests.cpp` |
| cli: -load merges saved session, explicit options win regardless of order | `[app][cli]` |  | `cli_tests.cpp` |
| cli: port forwarding specs | `[app][cli]` |  | `cli_tests.cpp` |
| cli: serial line and -sercfg | `[app][cli]` |  | `cli_tests.cpp` |
| cli: -local opens a local terminal (default shell, a shell name or a command line) | `[app][cli]` |  | `cli_tests.cpp` |
| cli: -m reads the remote command, -pwfile the password | `[app][cli]` |  | `cli_tests.cpp` |
| cli: logging, misc options, unknown options | `[app][cli]` |  | `cli_tests.cpp` |
| cli: host options are split off | `[app][cli]` |  | `cli_tests.cpp` |
| cli: --version and -V ask for the version; -v stays PuTTY's verbose | `[app][cli]` |  | `cli_tests.cpp` |
| cli: parse with a real Store (-load + default template) | `[app][cli]` |  | `cli_tests.cpp` |
| buildinfo: version, build number and commit come from the build | `[app][buildinfo]` |  | `buildinfo_tests.cpp` |
| buildinfo: the build time is the link time of this executable | `[app][buildinfo]` |  | `buildinfo_tests.cpp` |
| buildinfo: UTC times and the one-line description | `[app][buildinfo]` |  | `buildinfo_tests.cpp` |
| paths: portable vs installed vs override | `[app][paths]` |  | `cli_tests.cpp` |
| host: only the app's own pages load; http(s) and mailto links go to the browser | `[app][host]` |  | `host_tests.cpp` |
| host: messages from other origins are ignored; a new page first gets the window state | `[app][host]` |  | `host_tests.cpp` |
| host: closing asks the page while sessions are active | `[app][host]` |  | `host_tests.cpp` |
| host: the window placement is kept in state.json | `[app][host]` |  | `host_tests.cpp` |
| host: resizing does not hold up the shared worker pool (500 size changes, then a pool job starts within 100 ms; the newest one saved once quiet; a final one at once) | `[app][host]` |  | `host_tests.cpp` |
| host: the bundled UI is served under its origin | `[app][host]` |  | `host_tests.cpp` |
| host: menu bar commands and the title bar insets reach the page | `[app][host]` |  | `host_tests.cpp` |
| host: the controller releases the window's hooks | `[app][host]` |  | `host_tests.cpp` |
| page handover: the first page closes nothing and is not a reload | `[app][reload]` |  | `page_handover_tests.cpp` |
| page handover: a reload closes the old page's connections and hands over its workspace once | `[app][reload]` | closed once per reload; the newest workspace; asked again: no longer a reload | `page_handover_tests.cpp` |
| page handover: a reload before the page sent a workspace hands over none | `[app][reload]` |  | `page_handover_tests.cpp` |
| ui: index lines map paths to resource ids | `[app][ui]` |  | `ui_tests.cpp` |
| ui: request URIs become paths relative to the UI root | `[app][ui]` |  | `ui_tests.cpp` |
| ui: content types by extension | `[app][ui]` |  | `ui_tests.cpp` |
| ui: files embedded as resources are found by path | `[app][ui]` |  | `ui_tests.cpp` |

#### core — `st_test_core.exe` (9)

Strings (UTF-8 paths, UTF-8 validation, UTF-16LE), crypto helpers, session defaults and lenient JSON parsing, bridge message routing, app log rotation. *Kind: unit.* Source: [native/tests/core](../native/tests/core).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| strings: UTF-8 path roundtrip and helpers | — |  | `core_tests.cpp` |
| strings: UTF-8 validation and UTF-16LE conversion | — |  | `core_tests.cpp` |
| crypto: base64 + fingerprint | — |  | `core_tests.cpp` |
| types: session defaults and lenient parsing | — |  | `core_tests.cpp` |
| bridge: invoke, send and events | — |  | `core_tests.cpp` |
| bridge: a reloaded page gets no answers or events meant for the page before it | — | the new page's ids start at 1 again: the old page's late answer (also a dropped one) and its queued events never reach it; within one page as before | `core_tests.cpp` |
| log: a full log is kept as `<name>.1.log` and a new one starts | — |  | `core_tests.cpp` |
| threads: with every worker blocked a new job still runs; the extra workers end when idle | `[core][threads]` |  | `threading_tests.cpp` |
| threads: the debouncer runs the newest call once things are quiet | `[core][threads]` |  | `threading_tests.cpp` |

#### keys — `st_test_keys.exe` (9)

Key generation, PuTTY PPK v2/v3 read/write and encryption, OpenSSH <-> PPK conversion, cross-checks with `ssh-keygen` and `plink`. *Kind: unit + cross-check.* Source: [native/tests/keys](../native/tests/keys).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| keygen: ed25519 / rsa / ecdsa produce loadable OpenSSH and PPK files | `[keys]` |  | `keys_tests.cpp` |
| ppk: encrypted v3 asks for the passphrase and rejects wrong ones | `[keys][ppk]` |  | `keys_tests.cpp` |
| ppk: a v3 file may ask for at most 1 GiB and about a minute of work to unlock (refused while parsing, before the passphrase; a 64 MB key opens) | `[keys][ppk]` |  | `keys_tests.cpp` |
| ppk: v2 writer round-trips and tampering is detected | `[keys][ppk]` |  | `keys_tests.cpp` |
| convert: OpenSSH <-> PPK | `[keys]` |  | `keys_tests.cpp` |
| OpenSSH keys made by ssh-keygen load (encrypted with bcrypt KDF) | `[keys][openssh]` | OpenSSH `ssh-keygen` | `keys_tests.cpp` |
| plink authenticates with PPK files written by us | `[keys][plink]` | PuTTY `plink` | `keys_tests.cpp` |
| detectKeyFormat | `[keys]` |  | `keys_tests.cpp` |
| key text (pasted or stored in Credentials): inspect and load without a file | `[keys][vault]` | OpenSSH and PPK text; a public key and junk refused | `keys_tests.cpp` |

#### net — `st_test_net.exe` (35)

Local echo / line editing, proxy rules, sockets, SOCKS5 handshake, Raw / Telnet / Rlogin against scripted servers, the local terminal transport with this computer's default shell (Windows: PowerShell; macOS: the login shell), and the socket semantics every OS must reproduce (`[edge]`: partial writes, idle CPU, FIN and RST, urgent data, many sockets, proxy commands). *Kind: unit + integration.* Source: [native/tests/net](../native/tests/net).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| ldisc: forced off passes input straight through | `[net][ldisc]` |  | `net_tests.cpp` |
| ldisc: auto follows the transport modes | `[net][ldisc]` |  | `net_tests.cpp` |
| ldisc: line editing buffers until Enter and handles erase keys | `[net][ldisc]` |  | `net_tests.cpp` |
| ldisc: backspace removes a whole UTF-8 character | `[net][ldisc]` |  | `net_tests.cpp` |
| ldisc: ^U kills the line, ^C is sent immediately | `[net][ldisc]` |  | `net_tests.cpp` |
| ldisc: echo without editing | `[net][ldisc]` |  | `net_tests.cpp` |
| proxy: exclusions and localhost | `[net][proxy]` |  | `net_tests.cpp` |
| proxy: telnet/command template expansion | `[net][proxy]` |  | `net_tests.cpp` |
| proxy: a local proxy command takes only plain names; the password goes through the environment | `[net][proxy]` |  | `net_tests.cpp` |
| sockets: connect refused, pair and listen | `[net]` |  | `net_tests.cpp` |
| proxy: SOCKS5 handshake is byte-exact | `[net][proxy]` |  | `net_tests.cpp` |
| raw: bytes pass through both ways | `[net][raw]` |  | `net_tests.cpp` |
| telnet: active negotiation, TTYPE, NAWS, ECHO and IAC escaping | `[net][telnet]` |  | `net_tests.cpp` |
| telnet: unsupported options are refused | `[net][telnet]` |  | `net_tests.cpp` |
| rlogin: handshake and window size on request | `[net][rlogin]` |  | `net_tests.cpp` |
| edge: a large write to a slow reader arrives complete and in order | `[net][edge]` |  | `semantics_tests.cpp` |
| edge: an idle connection does not use the CPU | `[net][edge]` |  | `semantics_tests.cpp` |
| edge: data followed by FIN is delivered completely, then a clean close | `[net][edge]` |  | `semantics_tests.cpp` |
| edge: a connection reset is an error, after the data that came before it | `[net][edge]` |  | `semantics_tests.cpp` |
| edge: writing while the peer resets the connection fails cleanly | `[net][edge]` |  | `semantics_tests.cpp` |
| edge: Raw EOF half-closes the sending side and keeps receiving | `[net][edge]` |  | `semantics_tests.cpp` |
| edge: Telnet Synch sends IAC normally and the DM byte as urgent data | `[net][edge][telnet]` |  | `semantics_tests.cpp` |
| edge: Telnet reads urgent data inline, once, without spinning | `[net][edge][telnet]` |  | `semantics_tests.cpp` |
| edge: rlogin's urgent 0x80 asks for the window size, exactly once | `[net][edge][rlogin]` |  | `semantics_tests.cpp` |
| edge: writes from several threads all arrive, each thread's in order | `[net][edge]` |  | `semantics_tests.cpp` |
| edge: connections work while many other sockets are open | `[net][edge]` |  | `semantics_tests.cpp` |
| edge: a proxy command's output reaches the session, its errors the event log | `[net][edge][proxy]` |  | `semantics_tests.cpp` |
| edge: closing the session ends a running proxy command promptly | `[net][edge][proxy]` |  | `semantics_tests.cpp` |
| edge: a proxy command gets any password unchanged, and a bad host name runs nothing | `[net][edge][proxy]` |  | `semantics_tests.cpp` |
| edge: exiting with a proxy command running ends the command and does not crash (runs a copy of the test program: the hidden `[.][proxy-exit-child]` cases exit with the command running, or right after closing it) | `[net][edge][proxy]` |  | `semantics_tests.cpp` |
| local: program names for labels | `[local]` |  | `local_transport_tests.cpp` |
| local: there is a default shell | `[local]` |  | `local_transport_tests.cpp` |
| local: the default shell runs, takes input and reports its exit code | `[local]` |  | `local_transport_tests.cpp` |
| local: closing ends the shell, cleanly and once | `[local]` |  | `local_transport_tests.cpp` |
| local: a missing start folder fails with a readable reason | `[local]` |  | `local_transport_tests.cpp` |

#### platform — `st_test_platform.exe` (27)

The platform layer's tier 0 (`native/src/platform/sys`): what every OS's implementation must do the same way (files and atomic writes, UTF-8 paths, file names, whether letter case counts in a folder, environment, time, secrets, code pages, registry), the pseudo-terminals of local terminals (`[pty]`: output and exit code, input, resize, environment, a non-ASCII start folder, ending the program with what it started, errors, the shell list; Windows: ConPTY with `cmd.exe`; POSIX: `/bin/sh`, also that the program gets default signal handling), plus each OS's own rules: on Windows NTFS names, the Mark-of-the-Web and the check that the code page tables other OSes use are Windows' own (`[.generate]` rewrites them from Windows); on macOS (33 cases there) APFS names, the Local Network hint of failed connects, private atomic writes, executable types and the quarantine attribute (`[.keychain]` tries the real Keychain), the serial port listing, serial ports driven through a pseudo-terminal (data both ways, exclusive use, a vanished device, settings macOS lacks) and the `SSH_AUTH_SOCK` agent client against OpenSSH's `ssh-agent` (skipped without it). Uses no test servers, so it builds on every OS first. *Kind: unit (file system, pseudo-terminals, ssh-agent).* Source: [native/tests/platform](../native/tests/platform) (`common/` plus the OS's folder).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| fs: UTF-8 path text round trip | `[platform][fs]` |  | `fs_tests.cpp` |
| fs: atomic write creates folders, replaces content, leaves no temp files | `[platform][fs]` |  | `fs_tests.cpp` |
| fs: whether a folder's names differ by letter case | `[platform][fs]` |  | `fs_tests.cpp` |
| fs: fileInfo reports kind, size and times | `[platform][fs]` |  | `fs_tests.cpp` |
| fs: File writes, resumes, reads and sets times | `[platform][fs]` |  | `fs_tests.cpp` |
| fs: File reports errors | `[platform][fs]` |  | `fs_tests.cpp` |
| fs: renameReplace and removeFile | `[platform][fs]` |  | `fs_tests.cpp` |
| fs: readable now and exclusive writers | `[platform][fs]` |  | `fs_tests.cpp` |
| fs: sanitized file names are usable and stable | `[platform][fs]` |  | `fs_tests.cpp` |
| platform: names | `[platform]` |  | `sys_tests.cpp` |
| env: executable, folders, variables | `[platform][env]` |  | `sys_tests.cpp` |
| time: calendar conversions | `[platform][time]` |  | `sys_tests.cpp` |
| secret: round trip; foreign or damaged blobs do not decrypt | `[platform][secret]` |  | `sys_tests.cpp` |
| codepage: conversions every OS must agree on | `[platform][codepage]` |  | `sys_tests.cpp` |
| registry: missing keys read as missing | `[platform][registry]` |  | `sys_tests.cpp` |
| fs (Windows): NTFS file name rules | `[platform][fs]` |  | `fs_win_tests.cpp` |
| fs (Windows): executable types and the Mark-of-the-Web | `[platform][fs]` |  | `fs_win_tests.cpp` |
| codepage (Windows): the single-byte tables are Windows' own | `[platform][codepage]` |  | `codepage_win_tests.cpp` |
| codepage (Windows): the lead bytes are Windows' own | `[platform][codepage]` |  | `codepage_win_tests.cpp` |
| codepage (Windows): regenerate the tables | `[.generate]` | hidden: runs only when its tag is given (rewrites the tables) | `codepage_win_tests.cpp` |
| pty (Windows): ConPTY is there on the Windows versions we build for | `[platform][pty]` |  | `pty_win_tests.cpp` |
| pty (Windows): runs a program, passes its output and exit code | `[platform][pty]` |  | `pty_win_tests.cpp` |
| pty (Windows): input, resize, environment and a non-ASCII start folder | `[platform][pty]` |  | `pty_win_tests.cpp` |
| pty (Windows): kill ends the program and what it started | `[platform][pty]` |  | `pty_win_tests.cpp` |
| pty (Windows): a missing program or start folder is a readable error | `[platform][pty]` |  | `pty_win_tests.cpp` |
| pty (Windows): the shell list has Command Prompt and one default | `[platform][pty]` |  | `pty_win_tests.cpp` |
| pty (Windows): WSL distributions are named without quotes | `[platform][pty]` | wsl.exe keeps quotes as part of the name | `pty_win_tests.cpp` |

On macOS, `pty_posix_tests.cpp` has the POSIX counterparts (8): output and exit code, command-line splitting, input / resize / environment / a non-ASCII folder, default signal handling for the program, kill hangs it up, readable errors, the shell list starting with the login shell, and a terminal whose descriptors are numbered above 1024.

#### platform_ui — `st_test_platform_ui.exe` (11)

Second instances (command-line splitting; the window of the same data folder, and the key in the forwarded message), local files (`platform/ui/LocalFs`: paths, listings, file operations, long paths, "This PC", folder watcher, the local:* bridge handlers), and drag & drop of local files (`platform/ui/DragDrop`): the shell data object. *Kind: integration (Windows shell, file system).* Source: [native/tests/platform/win/ui](../native/tests/platform/win/ui).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| cli: splitCommandLine follows Windows quoting rules | `[app][cli]` |  | `cli_win_tests.cpp` |
| instance: a second start finds the window of its own data folder only | `[app][instance]` | two marked windows and an unmarked one (1.1.0): each key finds its own; the unmarked one only as the fallback; unmarked windows are not found | `instance_win_tests.cpp` |
| instance: the forwarded command line carries its data folder's key | `[app][instance]` | another key, the old format without a key, an empty or partial key are refused | `instance_win_tests.cpp` |
| dnd: shell data object for local files | `[dnd]` |  | `dnd_win_tests.cpp` |
| localfs: path normalisation | `[localfs]` |  | `localfs_win_tests.cpp` |
| localfs: list, stat and file operations | `[localfs]` |  | `localfs_win_tests.cpp` |
| localfs: long paths | `[localfs]` |  | `localfs_win_tests.cpp` |
| localfs: This PC lists known folders and drives | `[localfs]` |  | `localfs_win_tests.cpp` |
| localfs: folder watcher reports changes, debounced | `[localfs]` |  | `localfs_win_tests.cpp` |
| localfs: bridge handlers | `[localfs]` |  | `localfs_win_tests.cpp` |
| fonts: the installed families, with the monospace ones marked | `[platform][fonts]` | sorted, each once; Consolas monospace, Arial not (macOS: Menlo / Helvetica) | `common/ui/fonts_tests.cpp` |

#### platform_ui on macOS — `st_test_platform_ui` (10)

Local files (`platform/ui/LocalFs`, `LocalFsMac.mm`): paths with `~`, listings (dot files and Finder's hidden flag,
links described by their targets, locked files), rename never onto an existing item but with a change of letter case,
copy / move with existing items replaced as a whole, Finder's "copy" names and no folder into itself, Computer (places
and volumes), the FSEvents folder watcher (debounced, not recursive, its folder renamed away) and the local:* bridge
handlers. Nothing goes to the Trash. Drag & drop (`DragDropMacSupport.h`): the paths of files dropped from Finder
found by name; dragging itself needs a mouse and is checked by hand. *Kind: integration (file system, FSEvents).* Source: [native/tests/platform/mac/ui](../native/tests/platform/mac/ui).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| localfs: path normalisation | `[localfs]` |  | `localfs_mac_tests.cpp` |
| localfs: list and stat | `[localfs]` |  | `localfs_mac_tests.cpp` |
| localfs: make folders and rename | `[localfs]` |  | `localfs_mac_tests.cpp` |
| localfs: copy, move and delete | `[localfs]` |  | `localfs_mac_tests.cpp` |
| localfs: Computer lists places and volumes | `[localfs]` |  | `localfs_mac_tests.cpp` |
| localfs: folder watcher reports changes, debounced | `[localfs]` |  | `localfs_mac_tests.cpp` |
| localfs: the watched folder itself goes away | `[localfs]` |  | `localfs_mac_tests.cpp` |
| localfs: bridge handlers | `[localfs]` |  | `localfs_mac_tests.cpp` |
| dnd (macOS): dropped files are found by the page's names | `[dnd]` |  | `dnd_mac_tests.cpp` |
| fonts: the installed families, with the monospace ones marked | `[platform][fonts]` |  | `common/ui/fonts_tests.cpp` |

#### putty — `st_test_putty.exe` (12)

PuTTY import/export (registry names, settings mapping, `.reg` files, host keys, the settings files of PuTTY for Unix / macOS) and the `~/.ssh/config` (with macOS's `UseKeychain` and `IgnoreUnknown`) and `known_hosts` importers. *Kind: unit.* Source: [native/tests/putty](../native/tests/putty).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| putty: session names escape like mungestr | `[putty]` |  | `putty_tests.cpp` |
| putty: host keys convert to the same fingerprints as ssh-keygen | `[putty][hostkeys]` |  | `putty_tests.cpp` |
| putty: missing values take PuTTY's defaults | `[putty][mapping]` |  | `putty_tests.cpp` |
| putty: every mapped setting | `[putty][mapping]` |  | `putty_tests.cpp` |
| putty: serial settings | `[putty][mapping]` |  | `putty_tests.cpp` |
| putty: .reg export parses back to equivalent sessions | `[putty][regfile]` |  | `putty_tests.cpp` |
| putty: ANSI REGEDIT4 files with continuations and host keys | `[putty][regfile]` |  | `putty_tests.cpp` |
| putty: the settings files of PuTTY for Unix / macOS | `[putty][unix]` |  | `putty_tests.cpp` |
| ssh config: hosts, wildcards, jump hosts and forwards | `[importers][sshconfig]` |  | `putty_tests.cpp` |
| ssh config: macOS's Keychain options and IgnoreUnknown give no warnings | `[importers][sshconfig]` |  | `putty_tests.cpp` |
| known_hosts: plain, bracketed and hashed entries | `[importers][knownhosts]` |  | `putty_tests.cpp` |
| putty: real registry smoke test (read-only, counts only) | `[.registry]` | hidden: runs only when its tag is given | `putty_tests.cpp` |

#### s3 — `st_test_s3.exe` (52)

S3 storage, against [native/tests/s3/TestS3Server.h](../native/tests/s3/TestS3Server.h), a fake S3 service on
`TestHttpServer`. The fake checks every request's signature against what the client really sent, can inject a fault on
any request, and counts open multipart uploads and the most requests served at once. *Kind: unit + integration (fake
S3 server).* Source: [native/tests/s3](../native/tests/s3).

It covers:
- AWS Signature Version 4 against the worked examples of the S3 documentation and the cases of AWS's SigV4 test suite
  that apply to S3;
- the XML reader and the `~/.aws` files;
- every sign-in method;
- the HTTP client (WinHTTP / NSURLSession): streaming, cancel, timeouts, and certificate pinning with nothing sent to a
  refused server;
- the S3 client and `S3Fs`;
- S3 sessions end to end, through `SessionManager` and `SftpService`.

| Test case | File |
|---|---|
| crypto: HMAC-SHA-256 and HMAC-SHA-1 (RFC 4231 / RFC 2202 test case 2) | `sigv4_tests.cpp` |
| sigv4: dates and encoding; GET object, PUT object with a signed payload, GET bucket lifecycle and list objects, presigned URL (the documentation's examples); session tokens, anonymous requests, other services | `sigv4_tests.cpp` |
| sigv4: AWS's SigV4 test suite (from awslabs/aws-c-auth), 30 cases signed both with an Authorization header and as a presigned query: canonical request, string to sign and signature of each. Header keys, values, order, duplicates, trimming and folded lines; unreserved and UTF-8 paths and queries; query order; unnormalised paths (`//`, `/./`, `..`); POST with form bodies; session tokens. Left out: the path normalisation cases (S3 signs the path as sent) and a token added after signing | `sigv4_suite_tests.cpp` |
| xml: an S3 ListObjectsV2 answer; namespaces, empty elements, CDATA, BOM; an error document; malformed documents are refused (depth, size, entities); escaping | `xml_tests.cpp` |
| aws config: sections, comments, nested settings, warnings; the credentials file wins over config, key by key | `awsconfig_tests.cpp` |
| http: URLs; requests, headers, bodies, keep-alive; redirects are not followed; errors; a dropped connection, a short body and a stalled answer fail, cancel is quick; parallel requests; close aborts running requests; certificates (untrusted refused, probed, pinned; a wrong pin refuses before the request is written) | `http_tests.cpp` |
| s3 client: buckets, objects, metadata, listings (pages; keys with spaces, `+`, `&`, `<`, non-ASCII); If-Match and ranges past the end; readable errors; multipart (list parts, abort); copies keep metadata and storage class; part copies; batch delete in 1000s with per-key errors; a bucket in another region is followed; retries on throttling, server errors and dropped connections (none on client errors, at most 5); expired credentials refreshed once; presigned links; anonymous access; https with a pinned certificate | `s3client_tests.cpp` |
| credentials: the environment; static profiles, and errors for what is missing or unsupported; assume role through `source_profile` chains (loops caught), external id, MFA, `credential_source = Environment`; refreshed 5 minutes before they expire; `credential_process` (a real shell, a timeout, nothing printed); IAM Identity Center with the token cache (expired, revoked, a missing `sso-session`) | `credentials_tests.cpp` |
| s3fs: buckets at the root, folders, stat, capabilities; a session bucket is the root and the start folder home; rename and move of files and folders, inside and between buckets; removing a tree in batches | `s3fs_tests.cpp` |
| s3fs: downloads in parallel 8 MB ranges, in order, resumed from an offset; a dropped range is continued; a file changed meanwhile fails (If-Match) | `s3fs_tests.cpp` |
| s3fs: small uploads are one PUT with the file's time, big ones multipart in parallel; a failed multipart upload continues on the next try (only the missing parts); a cancelled one is aborted and the old object kept; memory stays within the parts in flight (96 MB down and up, 8 MB parts, 4 at once); presigned links | `s3fs_tests.cpp` |
| s3 session: the secret is asked, saved, and the file manager works through the transfer engine; a wrong secret is asked again; a session bucket is checked; an untrusted certificate is asked about first (only an unsigned probe went out), then remembered in Known hosts, or rejected; sign-in errors are readable | `s3service_tests.cpp` |

#### session — `st_test_session.exe` (30)

Charset decoding/encoding, session logging, the session manager (ordering, batching, prompts, event log, bridge handlers). *Kind: unit (fake transports).* Source: [native/tests/session](../native/tests/session).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| charset: names and aliases map to code pages | `[charset]` |  | `charset_tests.cpp` |
| charset: UTF-8 multi-byte characters split at every byte boundary | `[charset]` |  | `charset_tests.cpp` |
| charset: invalid UTF-8 becomes U+FFFD (maximal subparts) | `[charset]` |  | `charset_tests.cpp` |
| charset: DBCS code pages hold a trailing lead byte | `[charset]` |  | `charset_tests.cpp` |
| charset: GB18030 two- and four-byte sequences | `[charset]` |  | `charset_tests.cpp` |
| charset: single-byte code pages roundtrip | `[charset]` |  | `charset_tests.cpp` |
| charset: unmappable characters encode as '?' | `[charset]` |  | `charset_tests.cpp` |
| charset: binary strings | `[charset]` |  | `charset_tests.cpp` |
| sessionlog: expandPath placeholders | `[sessionlog]` |  | `sessionlog_tests.cpp` |
| sessionlog: stripEscapes | `[sessionlog]` |  | `sessionlog_tests.cpp` |
| sessionlog: printable mode strips sequences across chunk boundaries | `[sessionlog]` |  | `sessionlog_tests.cpp` |
| sessionlog: all and ssh-debug modes | `[sessionlog]` |  | `sessionlog_tests.cpp` |
| sessionlog: off mode, header, directories | `[sessionlog]` |  | `sessionlog_tests.cpp` |
| sessionlog: append, overwrite and ask | `[sessionlog]` |  | `sessionlog_tests.cpp` |
| sessionlog: a file that is in use reports an error | `[sessionlog]` |  | `sessionlog_tests.cpp` |
| session manager: open, status, data and remote close in order | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: output is batched and ordered | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: input is charset-encoded, binary bypasses it, pre-start input is queued | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: input and resize made before start are replayed after it | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: local echo through the line discipline | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: prompt round trip answered from another thread | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: pending prompt is cancelled when the connection closes | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: close is idempotent | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: event log is kept after close, debug entries only in ssh-debug mode | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: event log ring buffer is bounded | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: validation and duplicate connection ids | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: start failure and saved session touch | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: closeAll tears everything down | `[manager]` |  | `sessionmanager_tests.cpp` |
| session manager: a page reset closes every connection and sends nothing more about them | `[manager][reload]` | a waiting prompt is withdrawn without `prompts:onCancel`; late output, log lines and the close reach no page; the new page's connections as before | `sessionmanager_tests.cpp` |
| session manager: bridge handlers | `[manager]` |  | `sessionmanager_tests.cpp` |

#### sftp — `st_test_sftp.exe` (31)

File manager backend over SFTP and SCP: browsing, transfers, conflicts, resume, move, find, synchronize, edit. *Kind: integration (test SSH server).* Source: [native/tests/sftp](../native/tests/sftp).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| edit: saved changes are uploaded | `[edit]` |  | `edit_tests.cpp` |
| edit: missing files fail cleanly | `[edit]` |  | `edit_tests.cpp` |
| edit: the temp folder for opened files falls back when the system's is missing | `[edit]` |  | `edit_tests.cpp` |
| edit: a page reset ends the file sessions without a word to the new page | `[edit][reload]` | a watched edit is no longer uploaded, a download waiting at a conflict goes nowhere, no "cancelled" or "closed" event; the old file session is gone | `edit_tests.cpp` |
| edit: closing a file session the normal way still reports its edits and transfers | `[edit][reload]` | the edit's "closed", the transfer's "cancelled" | `edit_tests.cpp` |
| find: glob matching | `[find]` |  | `findsync_tests.cpp` |
| find: recursive search over SFTP | `[find]` |  | `findsync_tests.cpp` |
| sync: filters and comparison rules | `[sync]` |  | `findsync_tests.cpp` |
| sync: compare, apply, compare again finds nothing | `[sync]` |  | `findsync_tests.cpp` |
| scp: control records | `[scp]` |  | `scp_protocol_tests.cpp` |
| scp: shell quoting | `[scp]` |  | `scp_protocol_tests.cpp` |
| scp: permissions, offsets, dates | `[scp]` |  | `scp_protocol_tests.cpp` |
| scp: ls -l parsing | `[scp]` |  | `scp_protocol_tests.cpp` |
| scp: browse and modify through shell commands | `[scp]` | a shell with `ls` and `scp` (Windows: Git for Windows) | `scp_tests.cpp` |
| scp: transfers use the scp protocol | `[scp]` | a shell with `ls` and `scp` (Windows: Git for Windows) | `scp_tests.cpp` |
| scp: protocol selection | `[scp]` | a shell with `ls` and `scp` (Windows: Git for Windows) | `scp_tests.cpp` |
| sftp: browse, transfer and modify files over an SSH connection | `[sftp]` |  | `sftp_tests.cpp` |
| transfer: names that differ only in letter case | `[sftp][transfer]` |  | `transfer_tests.cpp` |
| transfer: conflict decisions | `[sftp][transfer]` |  | `transfer_tests.cpp` |
| transfer: download conflicts | `[sftp][transfer]` |  | `transfer_tests.cpp` |
| transfer: resume (a partial file continues only when its record matches the remote file; changed, another file, a broken or missing record: starts over; no record left after finishing or cancelling) | `[sftp][transfer]` |  | `transfer_tests.cpp` |
| tree limits: endless nesting stops every walk with a message; a deep real tree still works | `[sftp][tree][limits]` |  | `tree_walk_tests.cpp` |
| tree walks: links to folders are followed unless they lead back; links to files copy the file | `[sftp][tree][links]` |  | `tree_walk_tests.cpp` |
| tree walks: the executable bit (the bits arrive on macOS, setuid never; uploads from Windows 0644) | `[sftp][tree][mode]` |  | `tree_walk_tests.cpp` |
| tree walks: local links to folders are followed on upload unless they lead back (Windows: junctions) | `[sftp][tree][links]` |  | `tree_walk_tests.cpp` |
| transfer: move | `[sftp][transfer]` |  | `transfer_tests.cpp` |
| transfer: uploads merge into existing folders | `[sftp][transfer]` |  | `transfer_tests.cpp` |
| transfer: folder transfers report the listing before copying | `[sftp][transfer]` |  | `transfer_tests.cpp` |
| transfer: folder uploads can leave out macOS Finder's files | `[sftp][transfer]` |  | `transfer_tests.cpp` |
| transfer: upload as / download as give the one item a new name | `[sftp][transfer]` | also moved under a new name (the source goes), and a move skipped at a conflict keeps the source | `transfer_tests.cpp` |
| transfer: start and end times of a transfer | `[sftp][transfer]` | a finished one has both, in order; one cancelled while it ran too | `transfer_tests.cpp` |

#### ssh — `st_test_ssh.exe` (36)

SSH transport end to end: every login method, host-key prompts (a changed key holds back the saved password, forwarding and tunnels), PTY, signals, port forwarding (including backpressure: bounded buffers, no busy loop, nothing lost when the far end closes), X11 forwarding (display names, Xauthority files, the cookie gate, and a forwarded X client reaching a local X server with the real cookie), jump hosts, Pageant and `ssh-agent`. *Kind: integration (test SSH server).* Source: [native/tests/ssh](../native/tests/ssh).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| ssh: password authentication, shell echo and exit code | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: unknown host key prompts once, then is trusted | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: changed host key is reported as a mismatch and can be rejected | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: keyboard-interactive with several prompts | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: public key authentication from an OpenSSH key file | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: a key from Credentials: stored, linked file, gone | `[ssh][vault]` | login with a stored key and a linked file; a deleted key is refused with a message, no fallback to the session's own path | `ssh_tests.cpp` |
| ssh: a login from Credentials replaces the session's user and sign-in | `[ssh][vault]` | the login's user and stored password are used, nothing is saved to the session; "ask" prompts without offering to save; a deleted login falls back to the session's own settings with a message | `ssh_tests.cpp` |
| ssh: a login with a key from Credentials and no password | `[ssh][vault]` | the login's key signs in; no password is tried | `ssh_tests.cpp` |
| ssh: encrypted key asks for the passphrase | `[ssh]` | a wrong passphrase asks again; a key file of the session's own is not offered "Remember for this key" | `ssh_tests.cpp` |
| ssh: locked Credentials ask for the master password | `[ssh][vault][master]` | a login's sealed password: "Unlock Credentials" asked, a wrong one asks again with a message, the right one unlocks and signs in without a password prompt; already unlocked: no prompt; cancelled: the connection stops and Credentials stay locked | `ssh_tests.cpp` |
| ssh: a remembered passphrase of a key from Credentials | `[ssh][vault]` | "Remember for this key" saves it once the key opens (not when unticked); a remembered one is used without asking; one that no longer opens the key is forgotten and asked again with a message | `ssh_tests.cpp` |
| ssh: saved password is used without prompting, wrong password retries | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: resize, environment and terminal type | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: remote command and signals | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: large output is delivered completely | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: local port forwarding | `[ssh][forward]` |  | `ssh_tests.cpp` |
| ssh: a forward to a peer that does not read holds the sender back without spinning | `[ssh][forward]` |  | `ssh_tests.cpp` |
| ssh: a forward to a slow local reader holds the far sender back | `[ssh][forward]` |  | `ssh_tests.cpp` |
| ssh: everything sent before the far end closes a forward arrives | `[ssh][forward]` |  | `ssh_tests.cpp` |
| ssh: dynamic SOCKS5 forwarding | `[ssh][forward]` |  | `ssh_tests.cpp` |
| ssh: remote port forwarding | `[ssh][forward]` |  | `ssh_tests.cpp` |
| ssh: close() during connect and before start | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: post() runs tasks with the session, and with nullptr after close | `[ssh]` |  | `ssh_tests.cpp` |
| ssh: jump host tunnel | `[ssh][jump]` |  | `ssh_tests.cpp` |
| ssh: authentication through Pageant (agent) | `[ssh][agent]` | a Pageant instance is already running, not touching it, PuTTY `pageant.exe` | `ssh_tests.cpp` |
| ssh: authentication through ssh-agent (SSH_AUTH_SOCK) | `[ssh][agent]` | not Windows; OpenSSH `ssh-agent`, `ssh-add` | `ssh_tests.cpp` |
| ssh: X11 forwarding checks the cookie and hands the X server the real one | `[ssh][x11]` |  | `ssh_tests.cpp` |
| ssh: after a changed host key: no saved password, no forwarding, tunnels when asked for | `[ssh][hostkey]` | accepted once and saved: the password is asked (saying why), no agent / X11 request, no tunnel until "tunnels:open", then local and remote tunnels work; the next connection: asked again (once) or as before (saved) | `ssh_tests.cpp` |
| ssh: after a changed host key the one-time password is not sent either (keyboard-interactive) | `[ssh][hostkey]` |  | `ssh_tests.cpp` |
| ssh: a known or new host key keeps the saved password, forwarding and tunnels | `[ssh][hostkey]` |  | `ssh_tests.cpp` |
| ssh: a SOCKS request longer than the limit is closed | `[ssh][forward][socks]` | an endless SOCKS4 user id: a short one waits, past 1 KiB closed, nothing forwarded | `ssh_tests.cpp` |
| ssh: a SOCKS client that does not finish its request in time is closed | `[ssh][forward][socks]` | silent, and greeted only (a short test deadline) | `ssh_tests.cpp` |
| ssh: SOCKS4, SOCKS4a and SOCKS5 requests still work, also slow ones and data right behind them | `[ssh][forward][socks]` | byte by byte; 3 KiB of data right behind the request is forwarded | `ssh_tests.cpp` |
| x11: display names | `[ssh][x11]` |  | `x11_tests.cpp` |
| x11: the real cookie from an Xauthority file | `[ssh][x11]` |  | `x11_tests.cpp` |
| x11: the cookie gate | `[ssh][x11]` |  | `x11_tests.cpp` |

#### store — `st_test_store.exe` (30)

Persistent data: atomic files, DPAPI credentials, known hosts, sessions, folders, settings, UI state, corrupt-file recovery. *Kind: unit (temp folders).* Source: [native/tests/store](../native/tests/store).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| store io: atomic write replaces content and leaves no temp files | `[store][io]` |  | `store_tests.cpp` |
| store io: loadJson tolerates BOM and comments, backs up corrupt files | `[store][io]` |  | `store_tests.cpp` |
| credentials: DPAPI protect/unprotect roundtrip | `[store][credentials]` |  | `store_tests.cpp` |
| credentials: set/get/has/remove persist encrypted | `[store][credentials]` |  | `store_tests.cpp` |
| credentials: undecryptable entries and corrupt files | `[store][credentials]` |  | `store_tests.cpp` |
| credentials: change callback runs outside the lock | `[store][credentials]` |  | `store_tests.cpp` |
| known hosts: add/check/find semantics | `[store][knownhosts]` |  | `store_tests.cpp` |
| known hosts: addMany skips identical entries and invalid ones | `[store][knownhosts]` |  | `store_tests.cpp` |
| known hosts: corrupt file and invalid entries | `[store][knownhosts]` |  | `store_tests.cpp` |
| known hosts: callback runs outside the lock | `[store][knownhosts]` |  | `store_tests.cpp` |
| store: empty data dir loads defaults | `[store][sessions]` |  | `store_tests.cpp` |
| store: save session normalizes, preserves unknown fields and persists | `[store][sessions]` |  | `store_tests.cpp` |
| store: typed save overlays onto the stored document | `[store][sessions]` |  | `store_tests.cpp` |
| store: saveSessions bulk and deleteSessions (removes saved passwords) | `[store][sessions]` |  | `store_tests.cpp` |
| store: duplicate ids on disk are repaired on load | `[store][sessions]` |  | `store_tests.cpp` |
| store: proxy password is DPAPI-encrypted at rest | `[store][sessions]` |  | `store_tests.cpp` |
| store: touchSession updates lastUsedAt without notification | `[store][sessions]` |  | `store_tests.cpp` |
| store: findSessionByName and sessionNameExists | `[store][sessions]` |  | `store_tests.cpp` |
| store: folders explicit + implied, rename re-parents, delete keeps or removes | `[store][folders]` |  | `store_tests.cpp` |
| store: deleting a top-level folder keeping sessions moves them to the root | `[store][folders]` |  | `store_tests.cpp` |
| store: settings deep merge, arrays replace, callback + persistence | `[store][settings]` |  | `store_tests.cpp` |
| store: state keys get/set/remove and persist | `[store][state]` |  | `store_tests.cpp` |
| store: corrupt files are backed up and replaced by defaults | `[store][corrupt]` |  | `store_tests.cpp` |
| store: bare array sessions file (legacy shape) is accepted | `[store][corrupt]` |  | `store_tests.cpp` |
| store: concurrent writers smoke test | `[store][concurrency]` |  | `store_tests.cpp` |
| vault: keys of Credentials (linked files and stored text) | `[store][vault]` | add, list sorted, unique names, rename, persistence, the secret only encrypted in credentials.json, remove deletes the secret, corrupt file | `store_tests.cpp` |
| vault: export and import (one sealed file, ids kept, merged) | `[store][vault][export]` | a short file password refused; secrets counted (a stored key's text always, passwords and passphrases when chosen); nothing readable in the file, not even names; on another store with a master password: a wrong password and a foreign file refused, locked refused with nothing half done, then imported with ids kept, a taken name renamed "(2)", sealed with that store's master password; a second import skips everything; without secrets a stored-password login asks | `store_tests.cpp` |
| vault: master password (set, lock, unlock, wrong, change, remove, reset) | `[store][vault][master]` | at least 8 characters; every secret sealed ("mp1:", no plain text) and a session's own password untouched; locked: secrets refused (VaultLocked), lists and linked keys still work; wrong password; locked again after a restart; change (the old one stops working), remove (plain again, no record), reset (stored keys and every secret gone, linked keys and logins stay, a stored password becomes "ask") | `store_tests.cpp` |
| vault: remembered key passphrases (per key, forgotten with the key) | `[store][vault]` | remember / forget (change events only when something changed), the flag in list / get / rename, only encrypted in credentials.json (vault.json does not record it), deleting the key forgets it | `store_tests.cpp` |
| vault: logins (user name, password stored / asked / none, key, agent) | `[store][vault]` | add (a stored password is required, names unique, name and user required), the password only encrypted, edit keeps / removes / requires the stored password, persistence, remove deletes the secret | `store_tests.cpp` |

#### support — `st_test_support.exe` (8)

Self-tests of the fake servers, validated with independent clients (`plink`, `psftp`). *Kind: integration.* Source: [native/tests/support](../native/tests/support).

| Test case | Tags | Needs (skipped without) | File |
|---|---|---|---|
| TestEchoServer echoes data | `[support]` |  | `support_tests.cpp` |
| TestScriptServer records and sends | `[support]` |  | `support_tests.cpp` |
| TestSshServer basics | `[support]` |  | `support_tests.cpp` |
| TestSshServer works with plink (exec, exit codes, password) | `[support][plink]` | PuTTY `plink` | `support_tests.cpp` |
| TestSshServer password-only authentication with plink | `[support][plink]` | PuTTY `plink` | `support_tests.cpp` |
| TestSshServer works with plink for RSA and ECDSA host keys | `[support][plink]` | PuTTY `plink` | `support_tests.cpp` |
| TestSshServer serves SFTP to psftp | `[support][plink]` | PuTTY `psftp` | `support_tests.cpp` |
| TestSshServer honours local forwarding from plink | `[support][plink]` | PuTTY `plink` | `support_tests.cpp` |

### UI unit tests (247 tests)

#### `actions.test.ts` (7)

UI actions: the SFTP side panel per tab (open, switch, close, forget closed panes), focus after a tab click, and the sidebar button. Source: [ui/src/commands/actions.test.ts](../ui/src/commands/actions.test.ts).

| Group | Test |
|---|---|
| SFTP panel per tab | opens on an SSH terminal and closes again |
| SFTP panel per tab | switches to another pane of the same tab |
| SFTP panel per tab | keeps one panel per tab |
| SFTP panel per tab | is not offered for Telnet sessions or file manager tabs |
| SFTP panel per tab | forgets panels whose tab or pane is gone |
| focus after clicking a tab | asks the tab's active pane for focus, with a new request every time |
| sidebar button | shows and hides the session sidebar |

#### `macos.test.ts` (8)

The macOS side of the UI: knowing the host, shortcut labels in Mac symbols, the default key bindings of both OSes, the menu bar model. Source: [ui/src/commands/macos.test.ts](../ui/src/commands/macos.test.ts).

| Group | Test |
|---|---|
| platform | knows the host by its message channel, else by the browser |
| shortcut labels | macOS: the Mac symbols in its order |
| shortcut labels | Windows: unchanged |
| default key bindings | Windows: every command, written canonically, no chord twice |
| default key bindings | macOS: every command, written canonically, no chord twice |
| default key bindings | macOS: Command for the app, Control left to the terminal |
| menu bar | About and Settings open the application menu, then File, View and Terminal |
| menu bar | follows the state: sidebar label, commands that need a pane |

#### `paths.test.ts` (19)

File manager path helpers: remote (POSIX) and local (Windows, macOS) paths, breadcrumbs, formatting, the list model (sorting, selection, filtering), dialog helpers, back/forward history. Source: [ui/src/components/files/paths.test.ts](../ui/src/components/files/paths.test.ts).

| Group | Test |
|---|---|
| remote paths | joins, finds parents and names |
| remote paths | builds breadcrumbs |
| remote paths | normalizes typed paths |
| local paths (Windows) | walks up to This PC |
| local paths (Windows) | joins and names |
| local paths (Windows) | builds breadcrumbs |
| local paths (Windows) | normalizes typed paths |
| local paths (macOS) | walks up to Computer |
| local paths (macOS) | joins and names |
| local paths (macOS) | builds breadcrumbs |
| local paths (macOS) | normalizes typed paths |
| formatting | formats sizes and durations |
| formatting | validates names per side |
| list model | sorts folders first, then by key |
| list model | selects single, toggle and range |
| list model | filters by substring or glob |
| dialog helpers | suggests a free-looking name |
| dialog helpers | totals synchronize selections |
| back / forward history | goes back and forward without losing either side |

#### `listKeys.test.ts` (4)

The file lists' shortcuts with modifiers per OS (Windows Ctrl; macOS Command and Finder's keys), their labels, and the
chords a focused list keeps from the global shortcuts. Source: [ui/src/components/files/listKeys.test.ts](../ui/src/components/files/listKeys.test.ts).

| Group | Test |
|---|---|
| file list keys | macOS: Command and Finder keys |
| file list keys | Windows: Ctrl as before |
| file list keys | labels the shortcuts per OS |
| file list keys | names the chords a list keeps from the global shortcuts |

#### `parse.test.ts` (16)

Quick connect: parsing `user@host:port`, URLs, IPv6, serial targets, PuTTY/OpenSSH-style flags and `sftp://` / `scp://`; applying the result to a session; formatting it back. Source: [ui/src/components/quickconnect/parse.test.ts](../ui/src/components/quickconnect/parse.test.ts).

| Group | Test |
|---|---|
| parseQuickConnect | parses a bare host with the default protocol |
| parseQuickConnect | parses user@host:port |
| parseQuickConnect | uses the last @ to split user and host |
| parseQuickConnect | parses URL forms and ignores passwords and paths |
| parseQuickConnect | parses IPv6 addresses |
| parseQuickConnect | parses serial targets |
| parseQuickConnect | parses "protocol host port" style |
| parseQuickConnect | understands PuTTY/OpenSSH style flags |
| parseQuickConnect | rejects invalid input with a message |
| applyQuickConnect | fills host/port/user and keeps template defaults |
| applyQuickConnect | configures serial lines |
| applyQuickConnect | does not mutate the base session |
| formatQuickConnect | round-trips |
| file manager URLs | parses sftp:// and scp:// with a folder |
| file manager URLs | sets the file protocol on the session |
| file manager URLs | round-trips |

#### `workspace.test.ts` (9)

Workspace store: automatic tab titles for each Tab titles setting (terminal title, session name, host address) and closing panes of a split tab. Source: [ui/src/store/workspace.test.ts](../ui/src/store/workspace.test.ts).

| Group | Test |
|---|---|
| tab titles | terminal mode: the title the server set, else the session name |
| tab titles | session mode: the session name; unsaved sessions show user@host[:port] |
| tab titles | host mode: the host name or address; the line for serial sessions |
| tab titles | a session's fixed window title and a renamed tab always win |
| tab titles | file manager tabs: the session name, or the host in host mode |
| tab titles | defaults to terminal mode and uses the active pane |
| closing panes of a split tab | keeps the tab and makes the remaining pane active |
| closing panes of a split tab | keeps the active pane when another one closes |
| closing panes of a split tab | closes the tab with its last pane |

#### `workspacePersist.test.ts` (5)

Saving and restoring open tabs, including file manager tabs, snapshots from older versions and the names of tabs and panes. Source: [ui/src/store/workspacePersist.test.ts](../ui/src/store/workspacePersist.test.ts).

| Group | Test |
|---|---|
| workspace persistence with file manager tabs | keeps the kind and folders, drops the terminal link |
| workspace persistence with file manager tabs | treats old snapshots as terminals |
| workspace persistence with local terminals | brings local terminals back waiting to be started, other sessions as before |
| workspace persistence of names | "Keep tab and pane names" off: the tabs come back with automatic titles, the snapshot keeps the names |
| workspace persistence of names | tab and pane names come back after a restart |

#### `restoreAtStart.test.ts` (9)

The tabs at start: after a reload the old page's tabs come back from the host's memory (also with "Restore tabs on startup" off, not the saved ones) and connect again, with a notice; a normal start as before; the workspace goes to the host's memory on every structural change, to disk only when restoring is on. Source: [ui/src/store/restoreAtStart.test.ts](../ui/src/store/restoreAtStart.test.ts).

#### `skipMacFiles.test.ts` (3), `reportedFolder.test.ts` (2), `quickconnect/local.test.ts` (3)

The ".DS_Store and ._ files" default (on for macOS, off for Windows, saved values kept); the folder a local shell reports (OSC 7 / OSC 9;9) turned into a path of this system; quick connect's `local:` targets (a shell by id or name, else a command line). The rename tests are in `workspace.test.ts` (tab and pane names, duplicate keeps them), `workspacePersist.test.ts` (names after a restart) and `macos.test.ts` (the shortcut and the File menu entry).

#### `local.test.ts` (2)

Local terminal helpers: program names for labels and new local sessions from a shell. Source: [ui/src/shared/local.test.ts](../ui/src/shared/local.test.ts).

| Group | Test |
|---|---|
| local terminals | names the program for labels |
| local terminals | builds an unsaved session from a shell, keeping the template |

#### `keymap.test.ts` (37)

PuTTY keyboard modes: which escape sequence each key sends for every function-key, Home/End, cursor, keypad, Backspace and AltGr setting. Source: [ui/src/terminal/keymap.test.ts](../ui/src/terminal/keymap.test.ts).

| Group | Test |
|---|---|
| function keys | ESC[n~ mode: F1..F20 and Shift+F1..F10 = F11..F20 |
| function keys | Linux mode: F1..F5 = ESC[[A..E, rest as ESC[n~ |
| function keys | Xterm R6 mode: F1..F4 = ESC OP..OS, rest as ESC[n~ |
| function keys | VT400 mode: function keys as ESC[n~ |
| function keys | VT100+ mode: ESC OP .. ESC O[ |
| function keys | SCO mode: plain / Shift / Ctrl / Ctrl+Shift ranges |
| function keys | xterm (216+) mode: unmodified like R6, modified with bitmap parameter |
| function keys | Alt prefixes ESC in legacy modes |
| function keys | formatFunctionKey rejects out-of-range keys |
| Home / End and editing keypad | standard: ESC[1~ / ESC[4~ |
| Home / End and editing keypad | rxvt: ESC[H / ESC Ow |
| Home / End and editing keypad | xterm: native in xterm mode, ESC[H/ESC[F (SS3 in app mode) otherwise |
| Home / End and editing keypad | xterm mode with standard Home/End adds modifier parameter |
| Home / End and editing keypad | xterm mode leaves Insert/Delete/PageUp/PageDown to xterm.js |
| Home / End and editing keypad | VT400: edit keys in physical VT220 order |
| Home / End and editing keypad | SCO: H L DEL F I G |
| Home / End and editing keypad | Alt prefixes ESC in legacy modes |
| cursor keys | xterm mode: native unless application cursor keys are disabled |
| cursor keys | legacy modes: normal / application / Ctrl toggles |
| cursor keys | keypad centre (Clear) sends ESC[G |
| numeric keypad | application keypad: digits ESC Op..Oy, "." ESC On, Enter ESC OM |
| numeric keypad | normal keypad types characters |
| numeric keypad | disableAppKeypad ignores application keypad mode |
| numeric keypad | xterm-style operators |
| numeric keypad | ESC[n~ / Linux: PF keys in application mode, + = l (Shift m) |
| numeric keypad | VT400: keypad top row is always PF1..PF4 |
| numeric keypad | SCO / VT100+: no operator mapping, digits mapped in application mode |
| Backspace | ^? (default): Backspace DEL, Shift/Ctrl+Backspace ^H |
| Backspace | ^H: Backspace ^H, Shift/Ctrl+Backspace DEL |
| Ctrl+Alt / AltGr | Ctrl+Alt = AltGr (default): characters typed natively |
| Ctrl+Alt / AltGr | Ctrl+Alt different from AltGr: ESC + control character |
| Ctrl+Alt / AltGr | macOS: Option types its character unless it is Meta |
| Ctrl+Alt / AltGr | controlChar mapping |
| PuTTY local actions | Shift+Insert pastes, Ctrl+Insert copies |
| PuTTY local actions | Shift+PageUp/PageDown scroll pages, Ctrl+PageUp/PageDown scroll lines |
| PuTTY local actions | Ctrl+Break sends the break special, Pause sends ^Z |
| PuTTY local actions | ordinary keys are left to xterm.js |

#### `canvasFont.test.ts` (5)

Whether the WebGL renderer can draw the terminal font: a canvas and a page element measure the same text in the font list, and different widths (a font the user installed, which WKWebView on macOS doesn't give canvases) send the pane to the DOM renderer. Source: [ui/src/terminal/canvasFont.test.ts](../ui/src/terminal/canvasFont.test.ts).

| Test |
|---|
| widths within half a pixel match |
| a font the canvas and the page both draw is drawable |
| a font only the page finds is not (a user-installed font in WKWebView) |
| measures the font list at a fixed size and remembers the answer per list |
| without a canvas or a page it leaves the renderer alone and asks again later |

#### `themes.test.ts` (5)

Built-in app themes and terminal schemes: every colour present and valid, unique ids, SSHTermaq Neon as the default (with its terminal scheme and readable contrast), "system" and unknown-theme fallback. Source: [ui/src/theme/themes.test.ts](../ui/src/theme/themes.test.ts).

| Group | Test |
|---|---|
| built-in themes and schemes | every app theme has all colours and pairs with an existing terminal scheme |
| built-in themes and schemes | every terminal scheme has a unique id and hex colours |
| SSHTermaq Neon | is the default theme, with its terminal scheme for "auto" |
| SSHTermaq Neon | is dark, has the pink second accent, and button text stays readable |
| theme resolution | "system" follows the OS, unknown ids fall back to Dark / Light by id |

#### `buildInfo.test.ts` (3)

About: the build number with the commit (marking uncommitted changes) and the build time in local time. Source: [ui/src/components/dialogs/about/buildInfo.test.ts](../ui/src/components/dialogs/about/buildInfo.test.ts).

| Group | Test |
|---|---|
| About: version and build id | shows the build number with the commit, marking uncommitted changes |
| About: version and build id | shows nothing when the host sent no build id |
| About: version and build id | shows the build time in local time, and nothing for a missing or invalid time |

#### `contract.test.ts` (7)

The bridge contract: `API_SCHEMA` against the native host's sources and the mock backend. Source: [ui/src/bridge/contract.test.ts](../ui/src/bridge/contract.test.ts).

| Group | Test |
|---|---|
| native host | handles every invoke and send channel, and nothing else |
| native host | registers each channel with its kind (on() for send, handle*() for invoke) |
| native host | emits only events of the schema, and every one of them |
| mock backend | answers every invoke channel |
| mock backend | answers every send channel |
| mock backend | registers only channels of the schema, with their kind |
| mock backend | emits only events of the schema |

#### `reportError.test.ts` (6)

The UI's own errors in the app log (`app:log`): how errors are described, repeats dropped, the cap, the window's error events. Source: [ui/src/bridge/reportError.test.ts](../ui/src/bridge/reportError.test.ts).

| Group | Test |
|---|---|
| describeError | keeps a stack that already starts with the message (Chromium) |
| describeError | puts the message in front of a stack without it (WebKit) |
| describeError | describes things that are not errors |
| reportError | writes to the app log once per distinct error |
| reportError | stops after 50 reports |
| reportError | reports uncaught errors and unhandled rejections from the window |

#### `transferTimes.test.ts` (8), `fonts.test.ts` (7), `vaultModel.test.ts` (10), `store/vault.test.ts` (4)

What a finished transfer shows ("Done · 3,000 files · 23 MB in 41 s", "Failed after 12 s", the tooltip with the average
speed and the clock times, the queue's "All done · … in …"; without times as before), and the font picker's lists
(installed monospace fonts, missing suggestions greyed with a download page, other installed fonts; the note for a
chosen font that is not installed); Credentials: the sessions and logins that use a key, the sessions that use a
login, how a login signs in ("password stored · key work-laptop · agent", "key missing"), unique names; when the host
answers "Credentials are locked", one unlock dialog (shared by requests at the same time) and the call once more,
cancelled: the error stands, other errors never ask; what an import says ("Added 2 keys and 1 login (3 secrets).",
"Already here, left as they are: …", "Renamed, the name was taken: …", "Nothing new: …"). Sources: [ui/src/components/files/transferTimes.test.ts](../ui/src/components/files/transferTimes.test.ts),
[ui/src/components/dialogs/common/fonts.test.ts](../ui/src/components/dialogs/common/fonts.test.ts),
[ui/src/components/dialogs/credentials/vaultModel.test.ts](../ui/src/components/dialogs/credentials/vaultModel.test.ts),
[ui/src/store/vault.test.ts](../ui/src/store/vault.test.ts).

#### `session/sessionModel.test.ts` (47), `linkQuestion.test.ts` (3), `import/importModel.test.ts` (3)

Host names: every entry of [native/tests/fixtures/host-names.json](../native/tests/fixtures/host-names.json) through
`isValidHost`, the same file the native `isValidHostName` is tested with (one test per entry). The question before a
clicked link connects ("Open an SSH connection to deploy@host:22?", SFTP / SCP with the folder, the local proxy
command it would run first). Imported sessions that run a command on this computer: a local proxy command or a local
terminal's command line (not a remote command, not another proxy type), rows from a file start unticked, "1 of the 3
selected sessions runs a command on this computer". Finished transfers also count links to folders that were not
followed (in `transferTimes.test.ts`). Sources: [ui/src/components/dialogs/session/sessionModel.test.ts](../ui/src/components/dialogs/session/sessionModel.test.ts),
[ui/src/components/dialogs/linkQuestion.test.ts](../ui/src/components/dialogs/linkQuestion.test.ts),
[ui/src/components/dialogs/import/importModel.test.ts](../ui/src/components/dialogs/import/importModel.test.ts).

#### `session/s3Model.test.ts` (6), and the S3 cases of `parse.test.ts` and `linkQuestion.test.ts`

What they check:
- **The session editor:** an S3 session shows only the Session and S3 sections. The endpoint, bucket, region, part
  size and proxy are checked, and plain HTTP gets a warning. Also bucket names, the names of unnamed sessions, and
  service presets that fill in the endpoint and region only while those are untouched.
- **"Copy link…":** the expiry must be between 1 minute and 7 days.
- **Quick connect:** `s3://bucket/folder`, and `bucket/folder` with the S3 chip, both become an AWS session that signs
  in with the default profile.
- **Links:** the question before an `s3://` link opens.

Source: [ui/src/components/dialogs/session/s3Model.test.ts](../ui/src/components/dialogs/session/s3Model.test.ts).

### UI browser tests (102 tests, each run as `windows` and `macos`; the two marked macOS only skip in the Windows layout)

Source: [ui/e2e](../ui/e2e).

| File | Test |
|---|---|
| `welcome.spec.ts` | lists the recent sessions and quick connect |
| `welcome.spec.ts` | quick connect shows the serial example of the system (`COM3` / `/dev/cu.*`) |
| `welcome.spec.ts` | title bar: caption buttons and app menu on Windows, none on macOS |
| `welcome.spec.ts` | the sidebar can be hidden and shown again |
| `session-editor.spec.ts` | shows the session and its categories |
| `session-editor.spec.ts` | saved password: says what encrypts it on this system (Windows DPAPI / login Keychain) |
| `session-editor.spec.ts` | proxy password: says what encrypts it on this system |
| `session-editor.spec.ts` | renaming and saving updates the sidebar |
| `session-editor.spec.ts` | cancel asks before discarding changes, then keeps the session as it was |
| `sidebar-palette.spec.ts` | search filters the saved sessions |
| `sidebar-palette.spec.ts` | the context menu offers the session actions |
| `sidebar-palette.spec.ts` | duplicate adds a copy of the session |
| `sidebar-palette.spec.ts` | the command palette opens (shortcut, or the macOS menu bar) and closes with Escape |
| `sidebar-palette.spec.ts` | the welcome screen shows the system's shortcuts |
| `terminal.spec.ts` | double-clicking a session opens a tab with the shell |
| `terminal.spec.ts` | typed commands reach the shell and its output is shown |
| `terminal.spec.ts` | closing the tab returns to the welcome screen (after "Close tab?") |
| `terminal.spec.ts` | quick connect opens a session from user@host |
| `terminal.spec.ts` | split right from the command palette makes two panes |
| `reconnect.spec.ts` | after a lost connection and Restart, the new session starts below the old output (which stays whole), after an empty line |
| `reconnect.spec.ts` | after a lost connection and Restart, the SFTP panel stays in the folder it showed |
| `local-terminal.spec.ts` | the shortcut (macOS: the File menu) opens the default shell |
| `local-terminal.spec.ts` | the new tab menu lists the installed shells and opens one |
| `local-terminal.spec.ts` | the welcome screen has a Local terminal button |
| `transfer-as.spec.ts` | "Download as…" asks for the name and the transfer uses it |
| `transfer-as.spec.ts` | "Download as and delete on server (move)…" arrives under the new name and leaves the server |
| `transfer-as.spec.ts` | "Upload as…" is offered for an item of this computer (and its move entry) |
| `local-terminal.spec.ts` | "local:" plus a shell name opens that shell |
| `rename.spec.ts` | a split pane is renamed by double-clicking its header, and names the tab |
| `rename.spec.ts` | the tab menu renames the tab; an empty name brings the automatic title back |
| `rename-restart.spec.ts` | with "Keep tab and pane names" on (the default), the tab and the pane keep their names (a reload plays the restart: the mock keeps its data in localStorage) |
| `rename-restart.spec.ts` | with it off, the tabs come back with automatic titles |
| `sftp-panel.spec.ts` | navigates: open a folder, up, Home, breadcrumbs, a typed path, Backspace; a missing folder shows the error |
| `sftp-panel.spec.ts` | hidden files are shown on request; columns sort both ways |
| `sftp-panel.spec.ts` | new folder, rename with F2, delete with and without the question ("Confirm before deleting" off) |
| `sftp-panel.spec.ts` | the permissions dialog changes a mode, by checkbox and in octal; Cancel changes nothing |
| `sftp-panel.spec.ts` | download: the transfer runs, finishes, and "Clear finished" removes it |
| `sftp-panel.spec.ts` | upload: the toolbar button, and "Upload file as…" with a new name; a cancelled picker does nothing |
| `sftp-panel.spec.ts` | an upload onto an existing name asks: Skip leaves it, Rename… keeps both |
| `sftp-panel.spec.ts` | "Download as…" takes the name from the Save dialog; not offered for folders |
| `sftp-panel.spec.ts` | context menus: Copy path, Copy current path, Refresh |
| `sftp-panel.spec.ts` | "Open in the file manager" opens the two-pane file manager for the same server |
| `sftp-panel.spec.ts` | the panel closes with its button |
| `sftp-panel.spec.ts` | a Telnet session has no SFTP panel button |
| `file-manager.spec.ts` | navigates the server: open, back, forward, up, Home, a typed path (Ctrl+L / ⌘L) |
| `file-manager.spec.ts` | copies with F5 both ways and moves with F6 (the "move" tag, "All done") |
| `file-manager.spec.ts` | a conflict asks (Overwrite); the toolbar choice "skip" answers without asking |
| `file-manager.spec.ts` | a running transfer can be cancelled and retried; "Clear finished" empties the list |
| `file-manager.spec.ts` | bookmarks: add a folder, go back to it, remove it |
| `file-manager.spec.ts` | new folder (F7), rename in place (F2), delete with the question |
| `file-manager.spec.ts` | find files on the server (wildcards, the count, no matches), and go to a result |
| `file-manager.spec.ts` | synchronize: compare shows the differences, then runs them as transfers (the "sync" tag) |
| `file-manager.spec.ts` | hidden files on request; a terminal to the same server from the toolbar |
| `file-manager.spec.ts` | the local side lists the computer ("This PC" with the drives / the home folder), and filters |
| `file-manager.spec.ts` | macOS only: the Finder keys in a file list (⌘↓ open, ⌘↑ up, ⌘[ / ⌘] back and forward, ⇧⌘N new folder, ⌘⌫ delete, ⇧⌘G a typed path) |
| `file-manager.spec.ts` | macOS only: above the home folder are "/" and Computer with the volumes; a volume goes up to Computer; `~` in a typed path |
| `file-manager.spec.ts` | local files use the words of the system: Trash / Recycle Bin, Show in Finder / Show in Explorer |
| `import.spec.ts` | offers the sources of this system and counts the PuTTY sessions |
| `import.spec.ts` | PuTTY: notes, filter, select, import into the PuTTY folder (sessions and host keys) |
| `import.spec.ts` | existing sessions at the top level: marked, and replaced when chosen |
| `import.spec.ts` | OpenSSH config into a folder of its own, with the notes of a host; "Import more" |
| `import.spec.ts` | a .reg file and a JSON backup show their previews |
| `import.spec.ts` | sessions that run a command on this computer: marked, the command shown, from a file unticked (from this computer's PuTTY ticked), counted above Import |
| `import.spec.ts` | a session exports to a PuTTY .reg file and to JSON |
| `links.spec.ts` | a link asks before it connects and shows the proxy command; cancelling opens nothing |
| `links.spec.ts` | a plain command line connects without asking |
| `links.spec.ts` | "Don't ask again" turns the question off, and the setting turns it back on |
| `keygen.spec.ts` | generates an Ed25519 key: public key, fingerprint, and a warning without a passphrase |
| `keygen.spec.ts` | RSA offers key sizes, ECDSA curves |
| `keygen.spec.ts` | the passphrase must be confirmed and match |
| `keygen.spec.ts` | saves the private key (OpenSSH, .ppk) owner-only and the public key; a cancelled Save saves nothing; copies the public key |
| `keygen.spec.ts` | loads an existing key: a missing file, the passphrase of an encrypted one, then converts it to .ppk |
| `settings.spec.ts` | every section opens |
| `settings.spec.ts` | a theme is applied to the window at once and kept after a restart |
| `settings.spec.ts` | switches are saved (copy on select, restore tabs enabling "Keep tab and pane names", confirm before deleting) |
| `settings.spec.ts` | rows only one system has (Option as Meta on macOS; the Mica / Acrylic backdrop is hidden on both while it is off) |
| `settings.spec.ts` | a new key binding works at once; "Reset to default" brings the old one back |
| `settings.spec.ts` | the font picker: installed monospace fonts first, missing suggestions greyed (not pickable, "Get it"), a note for a missing font |
| `credentials.spec.ts` | the page lists stored and linked keys; copy the public key, rename (unique names), delete |
| `credentials.spec.ts` | pasting: junk and public keys refused; a private key shown, named from its comment, added |
| `credentials.spec.ts` | linking a key file keeps only its path |
| `credentials.spec.ts` | a session uses a key from Credentials; the page counts it and the delete question names it (and the login that uses it) |
| `credentials.spec.ts` | "Paste key…" in the session editor stores the key under its name and selects it |
| `credentials.spec.ts` | the key generator saves a new key to Credentials |
| `credentials-logins.spec.ts` | the page lists logins and how they sign in; new (taken name, no way to sign in, a stored password needed), edit keeps the password, duplicate, delete |
| `credentials-logins.spec.ts` | a session signs in with a saved login: the key, agent and password settings step aside; the username hint; back to the session's settings; the key's delete question names the login |
| `credentials-logins.spec.ts` | "Use login" in the sidebar for one session and for every SSH session of a folder; "This session's settings" clears it |
| `credentials-passphrase.spec.ts` | "Forget passphrase" on the page; connecting with the key asks for it with "Remember for this key" (a wrong one asks again), ticked: remembered again; the key's delete question says the passphrase goes too |
| `credentials-passphrase.spec.ts` | without "Remember for this key" every connection asks again |
| `credentials-master.spec.ts` | set a master password (too short, the two differ, the warning), Lock now (badge, the page's notice); pasting a key while locked asks "Unlock Credentials" (a wrong one says so) and then adds it |
| `credentials-master.spec.ts` | a connection whose login has a sealed password asks "Unlock Credentials" in the pane (wrong, then right) and connects; "Lock Credentials" in the command palette locks again |
| `credentials-master.spec.ts` | change (a wrong current one is refused), remove, and reset when it is forgotten (counts what goes; the stored key goes, the linked key and the login stay, asking for its password) |
| `credentials-export.spec.ts` | export (a short password, the two differ), the file written owner-only; a deleted login comes back from the file (a wrong password is refused), what was still here is left as it is; a second import adds nothing |
| `credentials-export.spec.ts` | a cancelled save exports nothing; exported without secrets, a login that stored its password asks for it after import |
| `credentials-export.spec.ts` | locked Credentials are unlocked first, then exported |
| `s3.spec.ts` | a saved S3 session opens the buckets in a file manager tab (the S3 badge; no Permissions column or terminal button; the bucket list cannot be changed; folders inside a bucket work) |
| `s3.spec.ts` | Copy link… makes a time-limited link (1 hour preselected, "Other…" up to 7 days, copied; folders have none) |
| `s3.spec.ts` | the session editor has the S3 type with service presets (MinIO: plain HTTP warned), bucket checks and sign-in methods |
| `s3.spec.ts` | Quick connect opens s3://bucket/folder |
| `hostkey.spec.ts` | a changed host key, connect once: the question says what stays off; the terminal says so; Special Commands > Open this session's tunnels, then it is gone from the list |
| `hostkey.spec.ts` | a changed host key, cancel ends the connection |
| `hostkey.spec.ts` | a known host connects as before: no question, no held tunnels |

## 6. How to run the tests

### Native tests

Build first (Debug shown; see [BUILDING.md](BUILDING.md)). Then, from the repository root:

```powershell
ctest --test-dir build/vs -C Debug --output-on-failure    # all 12 suites and the platform boundary check
ctest --test-dir build/vs -C Debug -R ssh                 # one suite, by module name
build\vs\Debug\st_test_ssh.exe                            # one suite, directly (shows Catch2's own output)
```

Catch2 options, for any `st_test_*.exe`:

| Command | Runs |
|---|---|
| `st_test_ssh.exe "[forward]"` | test cases with a tag |
| `st_test_ssh.exe "[jump],[agent]"` | either tag (comma = or) |
| `st_test_ssh.exe "~[forward]"` | everything except a tag |
| `st_test_ssh.exe "ssh: jump host tunnel"` | one test case by its name |
| `st_test_ssh.exe "*jump*"` | names matching a pattern (`*` only at the start and/or end) |
| `st_test_putty.exe "[.registry]"` | the hidden test that reads your real PuTTY registry (read-only, counts only) |
| `st_test_ssh.exe --list-tests` / `--list-tags` | lists test cases / tags without running them |
| `st_test_core.exe -d yes` | prints each test case's duration |
| `st_test_core.exe --reporter junit --out results.xml` | writes a JUnit XML report (for CI tools) |

**In Visual Studio:** open `build\vs\SSHTermaq.sln`. The test projects are in the solution's `tests` folder. Right-click
one, choose *Set as Startup Project*, and press F5 to debug it. To debug a single test case, put its name or tag into
*Properties* → *Debugging* → *Command Arguments*.

**Release build:** `.\scripts\windows\build.ps1 -Tests` runs the UI unit tests, then builds everything in Release and
runs `ctest` (every native suite and the platform boundary check) before packaging; `-E2E` also runs the UI typecheck
(with `ui/e2e`) and the browser tests (`npm run test:e2e`, about 5 minutes). It stops when anything fails. macOS:
`scripts/macos/build.sh --tests --e2e`.

### UI unit tests

```powershell
cd ui
npm test                          # all tests, once (vitest run)
npx vitest                        # watch mode: reruns affected tests on every save
npx vitest run keymap             # only files whose path contains "keymap"
npx vitest run -t "Backspace"     # only tests whose name contains "Backspace"
npm run typecheck                 # TypeScript check of the whole UI and of e2e/ (no tests, catches type errors)
```

### UI browser tests

```powershell
cd ui
npm run test:e2e                              # all browser tests, both layouts (playwright test)
npx playwright test --project=macos           # one layout
npx playwright test session-editor            # files whose name contains "session-editor"
npx playwright test -g "saved password"       # tests whose title contains "saved password"
npx playwright test --headed                  # with the browser window visible
npx playwright test --ui                      # Playwright's interactive runner: step through, see the page
npx playwright show-report                    # the HTML report of the last run
```

They need the installed Microsoft Edge (Windows) or Google Chrome; nothing is downloaded. Node and npm: see
[BUILDING.md](BUILDING.md); `scripts\windows\build.ps1` finds the Node.js that comes with Visual Studio when `npm` is
not on `PATH`, and a terminal can do the same by putting
`C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VisualStudio\NodeJs` first on `PATH`.

### Test SSH server for manual testing

The server the native tests use also runs on its own, for trying the app or another client against it:

```powershell
build\vs\Debug\st_testserver.exe --port 2222                              # user "test", password "secret"
build\vs\Debug\st_testserver.exe --port 2222 --sftp off --exec-shell sh   # SCP-only server (needs Git for Windows)
```

| Option | Meaning |
|---|---|
| `--port N` | listen port on `127.0.0.1` (default 2222) |
| `--user U`, `--password P` | login (default `test` / `secret`) |
| `--sftp-root DIR` | folder served over SFTP and SCP |
| `--authorized-keys FILE` | OpenSSH public keys accepted for key login |
| `--banner TEXT` | pre-login banner |
| `--host-key-type ed25519\|rsa\|ecdsa` | host key type |
| `--sftp on\|off` | `off` refuses SFTP, so clients fall back to SCP |
| `--exec-shell sh\|PATH` | run commands through a real shell; `sh` is Git for Windows' `sh.exe` (`git` works too) or `/bin/sh` on macOS |

### Fake S3 server for manual testing

The fake S3 service of the `s3` tests also runs on its own, for trying S3 sessions without an account:

```powershell
build\vs\Debug\st_tests3server.exe --port 9555 --demo           # http://127.0.0.1:9555, key AKIDTEST / test-secret
build\vs\Debug\st_tests3server.exe --port 9556 --tls --bucket a --bucket b   # https, a fresh self-signed certificate
```

It prints `LISTENING <endpoint> <certificate SHA-256 or ->` and runs until its input closes. Typing `keys` lists the
objects; `stats` shows the requests with a bad signature and the open multipart uploads.

| Option | Meaning |
|---|---|
| `--port N` | listen port |
| `--tls` | HTTPS, with a fresh self-signed certificate |
| `--key ID`, `--secret S` | the access key it accepts (default `AKIDTEST` / `test-secret`) |
| `--anonymous` | also accept unsigned requests |
| `--region R` | its region |
| `--bucket NAME` | a bucket; repeatable, default `demo` |
| `--demo` | fills the first bucket: `readme.txt`, `docs/guide.md`, `photos/a.jpg` and a 40 MB `big.bin` |

In an S3 session use the service "MinIO", the endpoint above, access key `AKIDTEST` and secret `test-secret`.

## 7. End-to-end and visual checks

Both scripts need the UI's `node_modules` (`npm ci`) and are run from the `ui` folder.

### `app.mjs`: the real app

Starts the app with a new temporary data folder (your own sessions are never touched), runs an optional steps file,
takes a screenshot and closes the app.

- **Windows:** `SSHTermaq.exe`; Playwright attaches to its WebView2 over the DevTools protocol, and the steps get
  Playwright's full `page`.
- **macOS:** a **Debug** build of `SSHTermaq.app`, started with `--automation <socket>`. WKWebView has no DevTools
  protocol, so the app offers a Unix socket instead (Debug builds only, owner-only;
  `native/src/platform/mac/ui/AutomationMac.h`), and [scripts/macApp.mjs](../ui/scripts/macApp.mjs) gives the steps a
  `page` with the calls steps use:
  - `evaluate(fn, arg)`, `waitForSelector(css)`, `waitForTimeout(ms)`, `screenshot({ path })`,
    `on('console' | 'pageerror')`
  - `click(css)` and `keyboard.type(text)` / `keyboard.press('Meta+T')`, which are real AppKit input: the page sees
    trusted events, and ⌘-shortcuts go through the menu bar as they do for a user. Clicks also land while the app is
    in the background (no first click spent on activating the window).
  - `sheet({ check, press })`: the native sheet on the window (an alert such as the file manager's "already exists"
    question, or a file panel) as `{ texts, buttons }`, or null; `check` ticks a checkbox and `press` presses a
    button, both by title.

  Selectors are plain CSS (no `getByRole`). The window does not need to be on screen.

```powershell
cd ui
node scripts/app.mjs --steps my-steps.mjs --out ..\artifacts\check.png
```

| Option | Default | Meaning |
|---|---|---|
| `--exe PATH` | `build\vs\Debug\SSHTermaq.exe`; macOS: `build/mac-<arch>/Debug/SSHTermaq.app/Contents/MacOS/SSHTermaq` | app to start |
| `--data-dir DIR` | a new temporary folder, deleted afterwards | data folder for the app |
| `--keep-data` | off | keep the temporary data folder |
| `--steps FILE` | none | ES module to run before the screenshot |
| `--out FILE` | `artifacts\app.png` | screenshot path |
| `--args "..."` | none | extra command-line arguments for the app (for example a host to connect to) |
| `--port N` | 9333 | DevTools port (Windows) |

A steps file exports a default async function that receives the `page` and helpers (`platform` is
`process.platform`), and can check anything with plain code:

```js
// my-steps.mjs
export default async function (page, { bridge, dataDir, out, exe }) {
  const info = await bridge('app:info');               // raw bridge call, same wire format as the UI
  if (info.name !== 'SSHTermaq') throw new Error('unexpected app name ' + info.name);
  await page.getByRole('main').getByRole('button', { name: 'New session' }).click();   // opens the session editor
  await page.screenshot({ path: out.replace('.png', '-editor.png') });
}
```

Throwing, or a failed Playwright action, makes the script print `E2E failed: <reason>` and exit with code 1. Console
errors from the page are printed at the end. Playwright refuses to click a locator that matches more than one element:
"New session" exists in the sidebar, its toolbar and the welcome screen, hence `getByRole('main')` above. That line is
Playwright's and so Windows-only; on macOS find the element with CSS (`page.click(...)`, or `page.evaluate` to pick it
by its text).

### `checks/local-terminal.mjs`: local terminals in the real app (Windows and macOS)

```
node scripts/app.mjs --steps scripts/checks/local-terminal.mjs
```

The same steps on both systems (Debug build): the shell list (at least one, exactly one default), the shortcut opens
the default shell, `TERM_PROGRAM` / `COLORTERM`, non-ASCII text both ways, the window size and (macOS) `LANG` for
bash-like shells, `exit` closes the tab, a second shell from the + menu, and on Windows a WSL distribution if there is one. Prints a `PASS` / `FAIL` line per check and
exits non-zero on the first failure.

### `checks/terminal-fonts.mjs`: terminal fonts with both renderers (Windows and macOS)

```
node scripts/app.mjs --steps scripts/checks/terminal-fonts.mjs --out ../artifacts/check.png
```

The same steps on both systems (Debug build, a PowerShell or POSIX default shell). For the system font (Menlo /
Consolas) and the first Nerd Font installed (skipped if there is none; install one, such as CaskaydiaMono Nerd Font,
to check it), with the Renderer setting on GPU (WebGL) and on DOM, it prints a line of text, powerline arrows, Nerd
Font icons, box drawing and blocks in a local terminal. It checks automatically:
- With the GPU setting, the pane uses WebGL exactly when a canvas draws the font as page text does. The script
  measures this itself, apart from the app's own `canvasFont.ts`.
- With the DOM setting, the pane uses DOM.
- On Windows, a Nerd Font stays on WebGL.
- When a font falls back to DOM, Settings › Terminal › Renderer says so.

Prints `PASS` / `FAIL` lines and saves a screenshot per case, `check-fonts-<font>-<webgl|dom>.png`. **Look at them**:
- Every case shows the font's own letters, joined box lines and the powerline arrows (xterm.js draws those itself).
- The Nerd Font shows its icons as pictures with both settings, never boxes or blanks.
- The system font shows boxes for the icons it doesn't have; that is expected.

Expected results:

| | System font, GPU | System font, DOM | Nerd Font, GPU | Nerd Font, DOM |
|---|---|---|---|---|
| macOS | WebGL | DOM | DOM, with the note in Settings | DOM |
| Windows | WebGL | DOM | WebGL | DOM |

### `checks/reload-and-second-start.mjs`: a reloaded window and two data folders (Windows)

```
node scripts/app.mjs --steps scripts/checks/reload-and-second-start.mjs --out ../artifacts/check.png
```

Needs the Debug build's `st_testserver.exe` (`build-debug.ps1 -All`). It starts the test server and quick-connects to
it. Then it reloads the page and checks, from the server's own count, that the old connection closed and exactly one
new one opened, that the tab came back (with "Restore tabs on startup" off, the default) and that the toast says so.
Then it opens a second instance with another data folder (its window on top) and starts `SSHTermaq.exe --data-dir
<the first one's> test@127.0.0.1:<port>`: the tab must open in the first window, not in the top one. Prints `PASS` /
`FAIL` lines.

### `shot.mjs`: the UI in a browser, with the mock backend

Builds the UI (with the mock backend) into `ui\.preview\<name>`, serves it on a random local port, opens it in
headless Microsoft Edge and takes a screenshot. Several runs with different `--name`s can run side by side. No
native build is needed.

```powershell
cd ui
node scripts/shot.mjs --name sidebar --out ..\artifacts\sidebar.png --theme light
```

| Option | Default | Meaning |
|---|---|---|
| `--name NAME` | `default` | build folder under `ui\.preview` |
| `--out FILE` | `artifacts\<name>.png` | screenshot path |
| `--steps FILE` | none | ES module `default async (page, { out, port })` run before the screenshot |
| `--width`, `--height` | 1400 × 900 | window size |
| `--theme dark\|light` | dark | colour scheme the page sees |
| `--no-build` | off | reuse the previous build of that name |
| `--keep-storage` | off | don't clear the mock backend's browser storage before the screenshot |

Known issue: after printing `screenshot: <path>` the script does not exit by itself; press Ctrl+C. The screenshot is
complete at that point. A Node `DEP0190` deprecation warning at the start is harmless.

## 8. What is not covered

- **UI behaviour:** the browser tests (section 5) cover the welcome screen, the session editor, the sidebar, the
  command palette, terminals and split panes, against the mock backend. Since 2026-11-14 also the SFTP panel, the file manager, import and export, the key
  generator and the settings dialog. Not covered yet: the host key and password / 2FA prompts, the color scheme editor,
  drag and drop, and the host-specific code (the real WebView2 / WKWebView bridge client). They are checked by hand with the scripts in section 7.
- **Native parts without tests:** the main window and WebView2 host (`platform/win/ui/HostWindowWin`; its policy, `app/HostController`, is tested), the core bridge handlers
  (`app/CoreHandlers`: sessions, settings, known hosts, window, ...), native file dialogs (`platform/ui/Dialogs`), the import
  bridge handlers (`importers/ImportHandlers`) and the serial port transport (needs hardware). The logic underneath
  most of them (storage, importers) is tested.
- **Serial ports with real hardware: never confirmed.** On Windows no serial session has been tested with a real
  COM port (USB adapter: FTDI, CP210x, CH340) at all; on macOS only through pseudo-terminals in the `platform` tests,
  never with an adapter. Whoever has an adapter: connect a serial session (line settings, typing both ways, Break)
  and record the result here (release checklist, sections 2 and 3, has the steps).
- **No continuous integration:** nothing runs the tests automatically.

## 9. Adding tests

**Native:** add a `TEST_CASE("<area>: what it checks", "[<module>][<area>]")` to a `.cpp` file in
`native/tests/<module>/`. A new `.cpp` in an existing folder is built automatically. A new module needs one
`st_test(<module> <libraries>)` line in `CMakeLists.txt`. Use `TempDir` for files and `TestSshServer` or the other
test servers for anything that needs a network peer. If a test needs an external tool, check for it first and call
`SKIP("<tool> not installed")` when it is missing.

**UI:** add `<file>.test.ts` next to the code, using `describe` / `it` / `expect` from `vitest`. Tests run in Node
without a DOM, so keep the logic you test in plain functions (outside React components).
