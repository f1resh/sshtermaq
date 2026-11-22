// Integration tests for the SSH transport against the in-process TestSshServer.
#include "TestExecProcess.h"
#include "TestNet.h"
#include "TestServers.h"

#include "core/Crypto.h"
#include "core/Strings.h"
#include "net/Proxy.h"
#include "net/Socket.h"
#include "platform/sys/Fs.h"
#include "session/Prompter.h"
#include "keys/Keygen.h"
#include "ssh/SshJump.h"
#include "ssh/SshTransport.h"
#include "store/Credentials.h"
#include "store/KnownHosts.h"
#include "store/Vault.h"

#include <catch2/catch_test_macros.hpp>

#include <libssh/libssh.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace st;
using namespace st::test;
using namespace std::chrono_literals;

namespace {

struct FakeListener : TransportListener {
    std::mutex m;
    std::condition_variable cv;
    std::string data;
    std::string local;
    std::vector<ConnStatus> statuses;
    std::vector<std::string> logs;
    bool closed = false;
    int closeCount = 0;
    CloseInfo info;

    void onData(std::string_view bytes) override {
        std::lock_guard l(m);
        data.append(bytes);
        cv.notify_all();
    }
    void onStatus(ConnStatus s, const std::string&) override {
        std::lock_guard l(m);
        statuses.push_back(s);
        cv.notify_all();
    }
    void onLog(LogLevel, const std::string& msg) override {
        std::lock_guard l(m);
        logs.push_back(msg);
    }
    void onClose(const CloseInfo& i) override {
        std::lock_guard l(m);
        closed = true;
        ++closeCount;
        info = i;
        cv.notify_all();
    }
    void onLocalOutput(const std::string& t) override {
        std::lock_guard l(m);
        local += t;
        cv.notify_all();
    }
    bool waitData(const std::string& needle, std::chrono::milliseconds t = 5s) {
        std::unique_lock l(m);
        return cv.wait_for(l, t, [&] { return data.find(needle) != std::string::npos; });
    }
    bool waitConnected(std::chrono::milliseconds t = 10s) {
        std::unique_lock l(m);
        return cv.wait_for(l, t, [&] {
            return closed || std::find(statuses.begin(), statuses.end(), ConnStatus::Connected) != statuses.end();
        }) && !closed;
    }
    bool waitClosed(std::chrono::milliseconds t = 10s) {
        std::unique_lock l(m);
        return cv.wait_for(l, t, [&] { return closed; });
    }
    std::string allLogs() {
        std::lock_guard l(m);
        return join(logs, "\n");
    }
};

struct FakePrompter : Prompter {
    std::function<PromptResponse(const Json&)> handler;
    std::mutex m;
    std::vector<Json> asked;
    PromptResponse prompt(const PromptInput& in) override {
        {
            std::lock_guard l(m);
            asked.push_back(in.json);
        }
        if (handler) return handler(in.json);
        PromptResponse r;
        r.cancelled = true;
        return r;
    }
    std::vector<std::string> kinds() {
        std::lock_guard l(m);
        std::vector<std::string> k;
        for (auto& j : asked) k.push_back(j.value("kind", ""));
        return k;
    }
};

PromptResponse accept(const std::string& choice) {
    PromptResponse r;
    r.cancelled = false;
    r.choice = choice;
    return r;
}

PromptResponse answer(std::vector<std::string> values) {
    PromptResponse r;
    r.cancelled = false;
    r.values = std::move(values);
    return r;
}

/** Everything a transport needs, with real KnownHosts / Credentials in a temp dir. */
struct Harness {
    TempDir dir;
    KnownHosts knownHosts{dir.path() / "known_hosts.json"};
    Credentials credentials{dir.path() / "credentials.json"};
    Vault vault{dir.path() / "vault.json", credentials};
    FakeListener listener;
    FakePrompter prompter;
    std::map<std::string, SessionConfig> saved;
    std::unique_ptr<Transport> transport;

    Harness() {
        knownHosts.load();
        credentials.load();
        vault.load();
        // By default accept unknown host keys once and type "secret" for password / kbd-int prompts.
        prompter.handler = [](const Json& p) {
            const std::string kind = p.value("kind", "");
            if (kind == "hostkey") return accept("accept-save");
            if (kind == "password") return answer({"secret"});
            if (kind == "kbdint") return answer(std::vector<std::string>(p["prompts"].size(), "secret"));
            PromptResponse r;
            return r;
        };
    }

    static SessionConfig session(const TestSshServer& s) {
        SessionConfig c = makeDefaultSession();
        c.host = "127.0.0.1";
        c.port = s.port();
        c.username = "test";
        c.ssh.auth.tryAgent = false;
        c.connection.keepaliveSeconds = 0;
        return c;
    }

    TransportContext context(const SessionConfig& c) {
        TransportContext ctx;
        ctx.connId = "test#1";
        ctx.session = c;
        ctx.cols = 80;
        ctx.rows = 24;
        ctx.listener = &listener;
        ctx.prompter = &prompter;
        ctx.knownHosts = &knownHosts;
        ctx.credentials = &credentials;
        ctx.vault = &vault;
        ctx.resolveSession = [this](const std::string& id) -> std::optional<SessionConfig> {
            auto it = saved.find(id);
            if (it == saved.end()) return std::nullopt;
            return it->second;
        };
        return ctx;
    }

    void start(const SessionConfig& c) {
        transport = makeSshTransport(context(c));
        transport->start();
    }

    ~Harness() { transport.reset(); }
};

std::string authorizedLine(ssh_key key) {
    char* b64 = nullptr;
    ssh_pki_export_pubkey_base64(key, &b64);
    std::string line = std::string(ssh_key_type_to_char(ssh_key_type(key))) + " " + b64 + " test@key";
    ssh_string_free_char(b64);
    return line;
}

}  // namespace

TEST_CASE("ssh: password authentication, shell echo and exit code", "[ssh]") {
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer server(o);
    Harness h;
    h.start(Harness::session(server));
    const bool connected = h.listener.waitConnected();
    INFO("logs:\n" << h.listener.allLogs() << "\nclose: " << h.listener.info.reason);
    REQUIRE(connected);
    REQUIRE(h.listener.waitData("$ "));
    h.transport->write("hello world\r");
    REQUIRE(h.listener.waitData("echo: hello world"));
    h.transport->write("exit 7\r");
    REQUIRE(h.listener.waitClosed());
    REQUIRE(h.listener.info.clean);
    REQUIRE(h.listener.info.hasExitCode);
    REQUIRE(h.listener.info.exitCode == 7);
    REQUIRE(h.listener.closeCount == 1);
    REQUIRE(server.waitForEvent("auth:password:ok", 1000));
    REQUIRE(server.waitForEvent("pty:xterm-256color:80x24", 1000));
}

TEST_CASE("ssh: unknown host key prompts once, then is trusted", "[ssh]") {
    TestSshServer server;
    Harness h;
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitConnected());
    auto kinds = h.prompter.kinds();
    REQUIRE(std::count(kinds.begin(), kinds.end(), "hostkey") == 1);
    REQUIRE(h.knownHosts.check("127.0.0.1", server.port(), server.hostKeyType(), [&] {
        std::string blob;
        crypto::base64Decode(server.hostKeyBase64(), blob);
        return blob;
    }()) == HostKeyCheck::Match);
    h.transport.reset();

    FakeListener second;
    h.prompter.asked.clear();
    TransportContext ctx = h.context(Harness::session(server));
    ctx.listener = &second;
    auto t = makeSshTransport(ctx);
    t->start();
    REQUIRE(second.waitConnected());
    kinds = h.prompter.kinds();
    REQUIRE(std::count(kinds.begin(), kinds.end(), "hostkey") == 0);
}

TEST_CASE("ssh: changed host key is reported as a mismatch and can be rejected", "[ssh]") {
    TestSshServer server;
    Harness h;
    KnownHost fake;
    fake.host = "127.0.0.1";
    fake.port = server.port();
    fake.keyType = "ssh-ed25519";
    {
        ssh_key other = nullptr;
        ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &other);
        char* b64 = nullptr;
        ssh_pki_export_pubkey_base64(other, &b64);
        fake.key = b64;
        ssh_string_free_char(b64);
        ssh_key_free(other);
    }
    h.knownHosts.add(fake);
    Json seen;
    h.prompter.handler = [&](const Json& p) {
        if (p.value("kind", "") == "hostkey") {
            seen = p;
            return accept("reject");
        }
        return answer({"secret"});
    };
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitClosed());
    REQUIRE(seen.value("status", "") == "mismatch");
    REQUIRE(seen["previous"].size() == 1);
    REQUIRE_FALSE(h.listener.info.error);  // user decision, not a network error
}

TEST_CASE("ssh: keyboard-interactive with several prompts", "[ssh]") {
    TestSshServerOptions o;
    o.allowPassword = false;
    o.kbdIntQuestions = {{"Password: ", "secret"}, {"Verification code: ", "123456"}};
    o.kbdIntEcho = {false, true};
    o.kbdIntName = "2FA";
    TestSshServer server(o);
    Harness h;
    Json kbd;
    h.prompter.handler = [&](const Json& p) {
        const std::string kind = p.value("kind", "");
        if (kind == "hostkey") return accept("accept-once");
        if (kind == "kbdint") {
            kbd = p;
            return answer({"secret", "123456"});
        }
        return PromptResponse{};
    };
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitConnected());
    REQUIRE(kbd["prompts"].size() == 2);
    REQUIRE(kbd["prompts"][1]["echo"] == true);
    REQUIRE(server.waitForEvent("auth:kbdint:ok", 1000));
}

TEST_CASE("ssh: public key authentication from an OpenSSH key file", "[ssh]") {
    TempDir keys;
    ssh_key key = nullptr;
    REQUIRE(ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &key) == SSH_OK);
    const auto keyPath = keys.path() / "id_ed25519";
    REQUIRE(ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, sys::fromPath(keyPath).c_str()) == SSH_OK);
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {authorizedLine(key)};
    ssh_key_free(key);
    TestSshServer server(o);
    Harness h;
    SessionConfig c = Harness::session(server);
    c.ssh.auth.privateKeyPath = sys::fromPath(keyPath);
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
}

TEST_CASE("ssh: a key from Credentials: stored, linked file, gone", "[ssh][vault]") {
    TempDir keys;
    ssh_key key = nullptr;
    REQUIRE(ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &key) == SSH_OK);
    const auto keyPath = keys.path() / "id_vault";
    REQUIRE(ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, sys::fromPath(keyPath).c_str()) == SSH_OK);
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {authorizedLine(key)};
    ssh_key_free(key);
    TestSshServer server(o);
    Harness h;
    SessionConfig c = Harness::session(server);
    c.ssh.auth.privateKeyPath = sys::fromPath(keys.path() / "does-not-exist");  // keyId wins over this

    SECTION("a stored key (its text, encrypted in credentials.json)") {
        std::ifstream f(keyPath, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        VaultKey entry;
        entry.name = "work laptop";
        c.ssh.auth.keyId = h.vault.addKeyText(entry, text).id;
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
    }

    SECTION("a linked key file") {
        VaultKey entry;
        entry.name = "linked";
        entry.path = sys::fromPath(keyPath);
        c.ssh.auth.keyId = h.vault.addKeyFile(entry).id;
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
    }

    SECTION("a key that was removed from Credentials: refused with a clear message, no fallback to the old path") {
        VaultKey entry;
        entry.name = "gone";
        entry.path = sys::fromPath(keyPath);
        c.ssh.auth.keyId = h.vault.addKeyFile(entry).id;
        h.vault.remove(c.ssh.auth.keyId);
        h.start(c);
        REQUIRE(h.listener.waitClosed());
        std::lock_guard lk(h.listener.m);
        REQUIRE(h.listener.local.find("no longer exists") != std::string::npos);
    }
}

TEST_CASE("ssh: a login from Credentials replaces the session's user and sign-in", "[ssh][vault]") {
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer server(o);  // user "test", password "secret"
    Harness h;
    SessionConfig c = Harness::session(server);
    c.username = "somebody-else";  // the login's user name wins
    h.credentials.set(Credentials::passwordKey(c.id), "wrong-session-password");

    SECTION("a stored password: no prompt, and the session's own saved password is not used") {
        VaultLogin l;
        l.name = "test login";
        l.username = "test";
        l.password = "stored";
        c.ssh.auth.loginId = h.vault.addLogin(l, std::string("secret")).id;
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        auto kinds = h.prompter.kinds();
        CHECK(std::count(kinds.begin(), kinds.end(), "password") == 0);
    }

    SECTION("\"ask every time\": the prompt has no \"remember\", and nothing is saved") {
        VaultLogin l;
        l.name = "asking";
        l.username = "test";
        l.password = "ask";
        c.ssh.auth.loginId = h.vault.addLogin(l, std::nullopt).id;
        bool allowSave = true;
        h.prompter.handler = [&](const Json& p) {
            const std::string kind = p.value("kind", "");
            if (kind == "hostkey") return accept("accept-once");
            if (kind == "password") {
                allowSave = p.value("allowSave", true);
                PromptResponse r = answer({"secret"});
                r.remember = true;  // even if the UI sent it, nothing may be saved
                return r;
            }
            return PromptResponse{};
        };
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        CHECK_FALSE(allowSave);
        CHECK(h.credentials.get(Credentials::passwordKey(c.id)) == std::string("wrong-session-password"));
        CHECK_FALSE(h.vault.loginPassword(c.ssh.auth.loginId).has_value());
    }

    SECTION("a deleted login: the session's own settings, with a message") {
        VaultLogin l;
        l.name = "gone";
        l.username = "test";
        l.password = "none";
        c.ssh.auth.loginId = h.vault.addLogin(l, std::nullopt).id;
        h.vault.removeLogin(c.ssh.auth.loginId);
        c.username = "test";
        h.credentials.set(Credentials::passwordKey(c.id), "secret");
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        std::lock_guard lk(h.listener.m);
        CHECK(h.listener.local.find("no longer exists") != std::string::npos);
    }
}

TEST_CASE("ssh: a login with a key from Credentials and no password", "[ssh][vault]") {
    TempDir keys;
    ssh_key key = nullptr;
    REQUIRE(ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &key) == SSH_OK);
    const auto keyPath = keys.path() / "id_login";
    REQUIRE(ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, sys::fromPath(keyPath).c_str()) == SSH_OK);
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {authorizedLine(key)};
    ssh_key_free(key);
    TestSshServer server(o);
    Harness h;
    VaultKey k;
    k.name = "login key";
    k.path = sys::fromPath(keyPath);
    VaultLogin l;
    l.name = "key login";
    l.username = "test";
    l.password = "none";
    l.keyId = h.vault.addKeyFile(k).id;
    SessionConfig c = Harness::session(server);
    c.username = "";  // would ask "login as": the login's user name is used instead
    c.ssh.auth.loginId = h.vault.addLogin(l, std::nullopt).id;
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
    auto kinds = h.prompter.kinds();
    CHECK(std::count(kinds.begin(), kinds.end(), "username") == 0);
}

TEST_CASE("ssh: encrypted key asks for the passphrase", "[ssh]") {
    TempDir keys;
    ssh_key key = nullptr;
    REQUIRE(ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &key) == SSH_OK);
    const auto keyPath = keys.path() / "id_enc";
    REQUIRE(ssh_pki_export_privkey_file(key, "pass phrase", nullptr, nullptr, sys::fromPath(keyPath).c_str()) == SSH_OK);
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {authorizedLine(key)};
    ssh_key_free(key);
    TestSshServer server(o);
    Harness h;
    int asked = 0;
    bool allowSave = true;
    h.prompter.handler = [&](const Json& p) {
        const std::string kind = p.value("kind", "");
        if (kind == "hostkey") return accept("accept-once");
        if (kind == "passphrase") {
            allowSave = p.value("allowSave", true);
            return answer({++asked == 1 ? "wrong" : "pass phrase"});
        }
        return PromptResponse{};
    };
    SessionConfig c = Harness::session(server);
    c.ssh.auth.privateKeyPath = sys::fromPath(keyPath);
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(asked == 2);
    CHECK_FALSE(allowSave);  // "Remember for this key" is for keys in Credentials only
}

TEST_CASE("ssh: a remembered passphrase of a key from Credentials", "[ssh][vault]") {
    TempDir keys;
    ssh_key key = nullptr;
    REQUIRE(ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &key) == SSH_OK);
    const auto keyPath = keys.path() / "id_remember";
    REQUIRE(ssh_pki_export_privkey_file(key, "pass phrase", nullptr, nullptr, sys::fromPath(keyPath).c_str()) == SSH_OK);
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {authorizedLine(key)};
    ssh_key_free(key);
    TestSshServer server(o);
    Harness h;
    VaultKey k;
    k.name = "remembered";
    k.path = sys::fromPath(keyPath);
    k.encrypted = true;
    const std::string keyId = h.vault.addKeyFile(k).id;
    SessionConfig c = Harness::session(server);
    c.ssh.auth.keyId = keyId;
    std::vector<Json> asked;
    auto handle = [&](bool remember) {
        h.prompter.handler = [&, remember](const Json& p) {
            const std::string kind = p.value("kind", "");
            if (kind == "hostkey") return accept("accept-once");
            if (kind == "passphrase") {
                asked.push_back(p);
                PromptResponse r = answer({"pass phrase"});
                r.remember = remember;
                return r;
            }
            return PromptResponse{};
        };
    };

    SECTION("\"Remember for this key\" at the prompt: saved once the key opens") {
        handle(true);
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].value("allowSave", false));
        CHECK(h.vault.passphrase(keyId) == std::string("pass phrase"));
        CHECK(h.vault.get(keyId)->passphraseRemembered);
    }

    SECTION("not ticked: nothing is saved") {
        handle(false);
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        CHECK_FALSE(h.vault.passphrase(keyId).has_value());
    }

    SECTION("a remembered passphrase: no prompt") {
        h.vault.rememberPassphrase(keyId, "pass phrase");
        handle(false);
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
        CHECK(asked.empty());
    }

    SECTION("a remembered passphrase that no longer opens the key: forgotten, then asked with a message") {
        h.vault.rememberPassphrase(keyId, "an old one");
        handle(false);
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].value("message", "").find("no longer opens") != std::string::npos);
        CHECK_FALSE(h.vault.passphrase(keyId).has_value());
    }
}

TEST_CASE("ssh: locked Credentials ask for the master password", "[ssh][vault][master]") {
    Vault::setKdfCost(1024, 1);
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer server(o);  // user "test", password "secret"
    Harness h;
    VaultLogin l;
    l.name = "sealed";
    l.username = "test";
    l.password = "stored";
    SessionConfig c = Harness::session(server);
    c.ssh.auth.loginId = h.vault.addLogin(l, std::string("secret")).id;
    h.vault.setMaster("master password");
    h.vault.lock();
    std::vector<Json> unlocks;
    int passwordPrompts = 0;
    auto handle = [&](std::vector<std::string> answers) {
        h.prompter.handler = [&, answers](const Json& p) {
            const std::string kind = p.value("kind", "");
            if (kind == "hostkey") return accept("accept-once");
            if (kind == "password") ++passwordPrompts;
            if (kind != "unlock") return PromptResponse{};
            unlocks.push_back(p);
            if (unlocks.size() > answers.size()) return PromptResponse{};  // cancelled
            return answer({answers[unlocks.size() - 1]});
        };
    };

    SECTION("a wrong master password asks again; the right one unlocks and signs in") {
        handle({"wrong one", "master password"});
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        REQUIRE(unlocks.size() == 2);
        CHECK(unlocks[0].value("title", "") == "Unlock Credentials");
        CHECK(unlocks[1].value("message", "").find("Wrong master password") != std::string::npos);
        CHECK(passwordPrompts == 0);
        CHECK(h.vault.status() == "unlocked");
    }

    SECTION("already unlocked: no prompt") {
        REQUIRE(h.vault.unlock("master password"));
        handle({});
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        CHECK(unlocks.empty());
    }

    SECTION("cancelled: the connection stops, Credentials stay locked") {
        PromptResponse cancel;
        cancel.cancelled = true;
        h.prompter.handler = [&](const Json& p) {
            const std::string kind = p.value("kind", "");
            if (kind == "hostkey") return accept("accept-once");
            if (kind == "unlock") unlocks.push_back(p);
            if (kind == "password") ++passwordPrompts;
            return cancel;
        };
        h.start(c);
        REQUIRE(h.listener.waitClosed());
        CHECK(unlocks.size() == 1);
        CHECK(passwordPrompts == 0);
        CHECK(h.vault.status() == "locked");
    }
}

TEST_CASE("ssh: saved password is used without prompting, wrong password retries", "[ssh]") {
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer server(o);
    Harness h;
    SessionConfig c = Harness::session(server);
    h.credentials.set(Credentials::passwordKey(c.id), "secret");
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    auto kinds = h.prompter.kinds();
    REQUIRE(std::count(kinds.begin(), kinds.end(), "password") == 0);
    h.transport.reset();

    // Wrong saved password -> prompt; then cancel.
    h.credentials.set(Credentials::passwordKey(c.id), "nope");
    FakeListener l2;
    int prompts = 0;
    h.prompter.handler = [&](const Json& p) {
        if (p.value("kind", "") == "hostkey") return accept("accept-once");
        if (p.value("kind", "") == "password") {
            ++prompts;
            PromptResponse r;  // cancel
            return r;
        }
        return PromptResponse{};
    };
    TransportContext ctx = h.context(c);
    ctx.listener = &l2;
    auto t = makeSshTransport(ctx);
    t->start();
    REQUIRE(l2.waitClosed());
    REQUIRE(prompts == 1);
    REQUIRE(l2.local.find("Access denied") != std::string::npos);
}

TEST_CASE("ssh: resize, environment and terminal type", "[ssh]") {
    TestSshServer server;
    Harness h;
    SessionConfig c = Harness::session(server);
    c.connection.environment = {{"LANG", "en_US.UTF-8"}};
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(h.listener.waitData("$ "));
    h.transport->resize(132, 43, 0, 0);
    REQUIRE(server.waitForEvent("window-change:132x43", 3000));
    h.transport->write("size\r");
    REQUIRE(h.listener.waitData("132x43"));
    h.transport->write("env LANG\r");
    REQUIRE(h.listener.waitData("en_US.UTF-8"));
    h.transport->write("term\r");
    REQUIRE(h.listener.waitData("xterm-256color"));
}

TEST_CASE("ssh: remote command and signals", "[ssh]") {
    TestSshServer server;
    {
        Harness h;
        SessionConfig c = Harness::session(server);
        c.ssh.remoteCommand = "echo from exec";
        h.start(c);
        REQUIRE(h.listener.waitClosed());
        REQUIRE(h.listener.data.find("from exec") != std::string::npos);
        REQUIRE(h.listener.info.exitCode == 0);
    }
    {
        Harness h;
        h.start(Harness::session(server));
        REQUIRE(h.listener.waitConnected());
        REQUIRE(h.listener.waitData("$ "));
        h.transport->write("signal\r");
        REQUIRE(server.waitForEvent("shell:signal", 3000));
        h.transport->sendSpecial("signal:INT");
        REQUIRE(h.listener.waitData("got signal INT"));
    }
}

TEST_CASE("ssh: large output is delivered completely", "[ssh]") {
    TestSshServer server;
    Harness h;
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitConnected());
    REQUIRE(h.listener.waitData("$ "));
    h.transport->write("big 2000000\r");
    REQUIRE(h.listener.waitData("END", 20s));
    std::lock_guard l(h.listener.m);
    REQUIRE(std::count(h.listener.data.begin(), h.listener.data.end(), 'x') >= 2000000);
}

TEST_CASE("ssh: local port forwarding", "[ssh][forward]") {
    TestEchoServer echo;
    TestSshServer server;
    Harness h;
    SessionConfig c = Harness::session(server);
    int freePort = 0;
    { auto l = net::listenLoopback(0, freePort); }
    PortForward f;
    f.id = "f1";
    f.type = ForwardType::Local;
    f.sourcePort = freePort;
    f.destHost = "127.0.0.1";
    f.destPort = echo.port();
    c.ssh.tunnels = {f};
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    std::this_thread::sleep_for(200ms);
    auto sock = net::connectTcp("127.0.0.1", freePort, 3000);
    REQUIRE(sock);
    REQUIRE(net::sendAll(sock.get(), "through the tunnel"));
    std::string got;
    for (int i = 0; i < 50 && got.size() < 18; ++i) {
        if (!net::waitReadable(sock.get(), 200)) continue;
        char buf[64];
        int n = recv(sock.get(), buf, sizeof buf, 0);
        if (n <= 0) break;
        got.append(buf, static_cast<size_t>(n));
    }
    REQUIRE(got == "through the tunnel");
    REQUIRE(server.waitForEvent("direct-tcpip:127.0.0.1:" + std::to_string(echo.port()), 1000));
}

/* ---- forwarding under backpressure: bounded buffers, no spinning, nothing lost ---- */

namespace {

char patternByte(size_t i) { return static_cast<char>((i * 7 + i / 65521) % 251); }

/** A session with one local forward to 127.0.0.1:targetPort; the forward's local port is set in `port`. */
void startForward(Harness& h, TestSshServer& server, int targetPort, int& port) {
    { auto l = net::listenLoopback(0, port); }
    SessionConfig c = Harness::session(server);
    PortForward f;
    f.id = "f1";
    f.type = ForwardType::Local;
    f.sourcePort = port;
    f.destHost = "127.0.0.1";
    f.destPort = targetPort;
    c.ssh.tunnels = {f};
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    std::this_thread::sleep_for(200ms);
}

/** Reads `n` bytes (fewer when the stream ends or nothing comes for 10 s), checking them against patternByte. */
size_t readPattern(net::Handle s, size_t n, size_t offset, bool& intact) {
    std::vector<char> buf(256 * 1024);
    size_t got = 0;
    intact = true;
    while (got < n) {
        if (!net::waitReadable(s, 10000)) break;
        const long r = sys::recvSome(s, buf.data(), std::min(buf.size(), n - got));
        if (r <= 0) break;
        for (long i = 0; i < r; ++i)
            if (buf[static_cast<size_t>(i)] != patternByte(offset + got + static_cast<size_t>(i))) intact = false;
        got += static_cast<size_t>(r);
    }
    return got;
}

}  // namespace

TEST_CASE("ssh: a forward to a peer that does not read holds the sender back without spinning", "[ssh][forward]") {
    int targetPort = 0;
    net::Socket target = net::listenLoopback(0, targetPort);
    TestSshServer server;
    Harness h;
    int port = 0;
    startForward(h, server, targetPort, port);
    net::Socket sock = net::connectTcp("127.0.0.1", port, 3000);
    REQUIRE(sock);
    net::Socket peer = net::acceptWithin(target.get(), 5000);
    REQUIRE(peer);

    // Send until nothing more goes: every buffer on the way is full, the SSH window shut.
    net::setNonBlocking(sock.get(), true);
    std::vector<char> chunk(65536);
    size_t sent = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - lastProgress < 1s && sent < (size_t{256} << 20)) {
        for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = patternByte(sent + i);
        const long n = sys::sendSome(sock.get(), chunk.data(), chunk.size());
        if (n > 0) {
            sent += static_cast<size_t>(n);
            lastProgress = std::chrono::steady_clock::now();
        } else {
            std::this_thread::sleep_for(10ms);
        }
    }
    INFO("sent " << sent);
    REQUIRE(sent < (size_t{256} << 20));

    // Stalled: the relay waits for the window instead of polling the socket it cannot read.
    const double cpu0 = net::processCpuSeconds();
    std::this_thread::sleep_for(2s);
    CHECK(net::processCpuSeconds() - cpu0 < 0.5);

    // Once the peer reads, everything arrives in order.
    bool intact = false;
    CHECK(readPattern(peer.get(), sent, 0, intact) == sent);
    CHECK(intact);
}

TEST_CASE("ssh: a forward to a slow local reader holds the far sender back", "[ssh][forward]") {
    constexpr size_t kTotal = size_t{64} << 20;
    int targetPort = 0;
    net::Socket target = net::listenLoopback(0, targetPort);
    std::atomic<size_t> blasted{0};
    std::thread blaster([&] {
        net::Socket peer = net::acceptWithin(target.get(), 5000);
        if (!peer) return;
        std::vector<char> chunk(65536);
        while (blasted < kTotal) {
            const size_t at = blasted;
            const size_t len = std::min(chunk.size(), kTotal - at);
            for (size_t i = 0; i < len; ++i) chunk[i] = patternByte(at + i);
            if (!net::sendAll(peer.get(), std::string_view(chunk.data(), len))) return;
            blasted += len;
        }
    });
    TestSshServer server;
    Harness h;
    int port = 0;
    startForward(h, server, targetPort, port);
    net::Socket sock = net::connectTcp("127.0.0.1", port, 3000);
    REQUIRE(sock);

    // Not reading: what the far end may send is bounded by the buffers on the way, not the whole stream.
    std::this_thread::sleep_for(2s);
    INFO("sent before the reader started: " << blasted.load());
    CHECK(blasted < kTotal / 2);

    bool intact = false;
    CHECK(readPattern(sock.get(), kTotal, 0, intact) == kTotal);
    CHECK(intact);
    blaster.join();
}

TEST_CASE("ssh: everything sent before the far end closes a forward arrives", "[ssh][forward]") {
    constexpr size_t kTotal = size_t{1} << 20;  // fits in the buffers on the way: the channel closes before we read
    int targetPort = 0;
    net::Socket target = net::listenLoopback(0, targetPort);
    std::thread sender([&] {
        net::Socket peer = net::acceptWithin(target.get(), 5000);
        if (!peer) return;
        std::string all(kTotal, '\0');
        for (size_t i = 0; i < kTotal; ++i) all[i] = patternByte(i);
        net::sendAll(peer.get(), all);
    });  // closes right after sending: the server closes the channel once it has passed everything on
    TestSshServer server;
    Harness h;
    int port = 0;
    startForward(h, server, targetPort, port);
    net::Socket sock = net::connectTcp("127.0.0.1", port, 3000);
    REQUIRE(sock);
    std::this_thread::sleep_for(1s);  // the channel is closed while much of the data still waits for us

    bool intact = false;
    CHECK(readPattern(sock.get(), kTotal, 0, intact) == kTotal);
    CHECK(intact);
    char b;
    CHECK(net::recvWithin(sock.get(), &b, 1, 5000) == 0);  // then the end of the stream
    sender.join();
}

TEST_CASE("ssh: dynamic SOCKS5 forwarding", "[ssh][forward]") {
    TestEchoServer echo;
    TestSshServer server;
    Harness h;
    SessionConfig c = Harness::session(server);
    int freePort = 0;
    { auto l = net::listenLoopback(0, freePort); }
    PortForward f;
    f.id = "d1";
    f.type = ForwardType::Dynamic;
    f.sourcePort = freePort;
    c.ssh.tunnels = {f};
    c.ssh.noShell = true;
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    std::this_thread::sleep_for(200ms);
    auto sock = net::connectTcp("127.0.0.1", freePort, 3000);
    REQUIRE(sock);
    REQUIRE(net::sendAll(sock.get(), std::string("\x05\x01\x00", 3)));
    char buf[64];
    REQUIRE(net::waitReadable(sock.get(), 3000));
    REQUIRE(recv(sock.get(), buf, 2, 0) == 2);
    REQUIRE(buf[1] == 0);
    std::string req("\x05\x01\x00\x01\x7f\x00\x00\x01", 8);
    req.push_back(static_cast<char>(echo.port() >> 8));
    req.push_back(static_cast<char>(echo.port() & 0xff));
    REQUIRE(net::sendAll(sock.get(), req));
    REQUIRE(net::waitReadable(sock.get(), 3000));
    REQUIRE(recv(sock.get(), buf, 10, 0) == 10);
    REQUIRE(buf[1] == 0);
    REQUIRE(net::sendAll(sock.get(), "socks!"));
    REQUIRE(net::waitReadable(sock.get(), 3000));
    int n = recv(sock.get(), buf, sizeof buf, 0);
    REQUIRE(std::string(buf, static_cast<size_t>(n)) == "socks!");
}

TEST_CASE("ssh: remote port forwarding", "[ssh][forward]") {
    TestEchoServer echo;
    TestSshServer server;
    Harness h;
    SessionConfig c = Harness::session(server);
    PortForward f;
    f.id = "r1";
    f.type = ForwardType::Remote;
    f.sourcePort = 0;  // server picks
    f.destHost = "127.0.0.1";
    f.destPort = echo.port();
    c.ssh.tunnels = {f};
    h.start(c);
    REQUIRE(h.listener.waitConnected());
    REQUIRE(server.waitForEvent("tcpip-forward:", 3000));
    const int port = server.lastRemoteForwardPort();
    REQUIRE(port > 0);
    auto sock = net::connectTcp("127.0.0.1", port, 3000);
    REQUIRE(sock);
    REQUIRE(net::sendAll(sock.get(), "reverse"));
    REQUIRE(net::waitReadable(sock.get(), 5000));
    char buf[64];
    int n = recv(sock.get(), buf, sizeof buf, 0);
    REQUIRE(std::string(buf, static_cast<size_t>(std::max(n, 0))) == "reverse");
}

TEST_CASE("ssh: close() during connect and before start", "[ssh]") {
    TestScriptServer silent;  // accepts TCP but never speaks SSH
    Harness h;
    SessionConfig c = makeDefaultSession();
    c.host = "127.0.0.1";
    c.port = silent.port();
    c.username = "test";
    c.connection.connectTimeoutSeconds = 30;
    h.start(c);
    REQUIRE(silent.waitForConnection(3000));
    const auto t0 = std::chrono::steady_clock::now();
    h.transport->close();
    REQUIRE(h.listener.waitClosed(5s));
    REQUIRE(std::chrono::steady_clock::now() - t0 < 3s);
    REQUIRE(h.listener.closeCount == 1);

    Harness h2;
    auto t = makeSshTransport(h2.context(c));
    t->close();
    t.reset();  // destroying a never-started transport must not hang
}

TEST_CASE("ssh: post() runs tasks with the session, and with nullptr after close", "[ssh]") {
    TestSshServer server;
    Harness h;
    h.start(Harness::session(server));
    REQUIRE(h.listener.waitConnected());
    auto* ssh = h.transport->asSsh();
    REQUIRE(ssh);
    std::promise<bool> got;
    ssh->post([&](ssh_session s) { got.set_value(s != nullptr); });
    REQUIRE(got.get_future().get());
    h.transport->close();
    REQUIRE(h.listener.waitClosed());
    std::promise<bool> after;
    ssh->post([&](ssh_session s) { after.set_value(s == nullptr); });
    REQUIRE(after.get_future().get());
}

TEST_CASE("ssh: jump host tunnel", "[ssh][jump]") {
    TestSshServer jumpServer;
    TestSshServer target;
    setSshJumpOpener(&openSshJumpTunnel);
    Harness h;
    SessionConfig jump = Harness::session(jumpServer);
    jump.name = "jump";
    h.saved[jump.id] = jump;
    SessionConfig c = Harness::session(target);
    c.proxy.type = ProxyType::Ssh;
    c.proxy.jumpSessionId = jump.id;
    c.proxy.proxyLocalhost = true;  // the target is 127.0.0.1, which bypasses proxies by default (PuTTY semantics)
    h.start(c);
    const bool connected = h.listener.waitConnected(15s);
    INFO("logs:\n" << h.listener.allLogs() << "\nclose: " << h.listener.info.reason);
    REQUIRE(connected);
    REQUIRE(jumpServer.waitForEvent("direct-tcpip:127.0.0.1:" + std::to_string(target.port()), 2000));
    REQUIRE(h.listener.waitData("$ "));
    h.transport->write("user\r");
    REQUIRE(h.listener.waitData("test"));
    INFO(h.listener.allLogs());
    REQUIRE(h.listener.allLogs().find("Jump: ") != std::string::npos);
}

TEST_CASE("ssh: authentication through Pageant (agent)", "[ssh][agent]") {
    const std::filesystem::path pageant = sys::toPath("C:\\Program Files\\PuTTY\\pageant.exe");
    if (!std::filesystem::exists(pageant)) SKIP("pageant.exe not installed");
    if (pageantRunning()) SKIP("a Pageant instance is already running; not touching it");

    // A throwaway key in PuTTY's own format (written by our PPK writer).
    TempDir dir;
    KeygenResult kr = keys::generateKey(KeygenRequest{"ed25519", 0, "agent-test", ""});
    const auto keyPath = dir.path() / "agent_key.ppk";
    {
        std::ofstream f(keyPath, std::ios::binary);
        f << kr.privateKeyPpk;
    }
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {kr.publicKeyOpenSsh};
    TestSshServer server(o);

    TestProcess agent(pageant, {sys::fromPath(keyPath)});
    for (int i = 0; i < 50 && !pageantRunning(); ++i) std::this_thread::sleep_for(100ms);
    REQUIRE(pageantRunning());
    std::this_thread::sleep_for(500ms);  // key loading

    Harness h;
    SessionConfig c = Harness::session(server);
    c.ssh.auth.tryAgent = true;
    c.ssh.auth.agent = AgentKind::Pageant;
    h.start(c);
    const bool connected = h.listener.waitConnected(40s);
    INFO(h.listener.allLogs());
    REQUIRE(connected);
    REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
    REQUIRE(h.listener.allLogs().find("Pageant has 1 SSH-2 key") != std::string::npos);
}

TEST_CASE("ssh: authentication through ssh-agent (SSH_AUTH_SOCK)", "[ssh][agent]") {
    TempDir dir;
    KeygenResult kr = keys::generateKey(KeygenRequest{"ed25519", 0, "agent-test", ""});
    const auto keyPath = dir.path() / "agent_key";
    {
        std::ofstream f(keyPath, std::ios::binary);
        f << kr.privateKeyOpenSsh;
    }
    std::string why;
    auto agent = startSshAgent(keyPath, why);
    if (!agent) SKIP(why);
    TestSshServerOptions o;
    o.allowPassword = false;
    o.allowKbdInt = false;
    o.authorizedKeys = {kr.publicKeyOpenSsh};
    TestSshServer server(o);

    Harness h;
    SessionConfig c = Harness::session(server);
    c.ssh.auth.tryAgent = true;
    c.ssh.auth.agent = AgentKind::Auto;
    h.start(c);
    const bool connected = h.listener.waitConnected(20s);
    INFO(h.listener.allLogs());
    REQUIRE(connected);
    REQUIRE(server.waitForEvent("auth:publickey:ok", 1000));
    REQUIRE(h.listener.allLogs().find("SSH agent has 1 SSH-2 key") != std::string::npos);
}

namespace {

/** A local X server for the X11 test: records the connection setup of every client that sends one, answers
 *  "X-SERVER-OK". It listens on a port above 6000, so display ":<port - 6000>" reaches it over TCP. */
struct FakeXServer {
    net::Socket listener;
    int port = 0;
    std::atomic<bool> stop{false};
    std::mutex m;
    std::vector<std::string> setups;
    std::thread thread;

    FakeXServer() {
        listener = net::listenLoopback(0, port);
        thread = std::thread([this] {
            while (!stop) {
                net::Socket c = net::acceptWithin(listener.get(), 100);
                if (!c) continue;
                std::string got;
                char buf[512];
                const auto end = std::chrono::steady_clock::now() + 3s;
                while (got.find("after-setup") == std::string::npos && std::chrono::steady_clock::now() < end) {
                    const int n = net::recvWithin(c.get(), buf, sizeof buf, 100);
                    if (n == 0) break;  // turned away by the client: nothing comes
                    if (n > 0) got.append(buf, static_cast<size_t>(n));
                }
                if (got.empty()) continue;
                {
                    std::lock_guard l(m);
                    setups.push_back(got);
                }
                net::sendAll(c.get(), "X-SERVER-OK");
                std::this_thread::sleep_for(300ms);
            }
        });
    }
    ~FakeXServer() {
        stop = true;
        thread.join();
    }
    std::vector<std::string> received() {
        std::lock_guard l(m);
        return setups;
    }
};

std::string xauthEntry(uint16_t family, const std::string& address, const std::string& number, const std::string& data) {
    auto be16 = [](size_t v) { return std::string{static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)}; };
    auto counted = [&](const std::string& s) { return be16(s.size()) + s; };
    return be16(family) + counted(address) + counted(number) + counted("MIT-MAGIC-COOKIE-1") + counted(data);
}

}  // namespace

TEST_CASE("ssh: X11 forwarding checks the cookie and hands the X server the real one", "[ssh][x11]") {
    FakeXServer x;
    REQUIRE(x.listener);
    REQUIRE(x.port > 6000);
    const std::string display = std::to_string(x.port - 6000);
    TempDir tmp;
    const std::string real = "the-real-cookie!";
    // Its socket /tmp/.X11-unix/X<n> does not exist: the client goes on to TCP. The display comes from DISPLAY.
    ScopedEnv displayEnv("DISPLAY", ":" + display);
    TestSshServer server;
    auto session = [&] {
        SessionConfig c = Harness::session(server);
        c.ssh.x11.enabled = true;
        c.ssh.x11.display = "";  // automatic
        return c;
    };
    const std::string expectedSetup = std::string("l\0\x0b\0\0\0\x12\0\x10\0\0\0", 12) + "MIT-MAGIC-COOKIE-1" +
                                      std::string(2, '\0') + real + "after-setup";

    SECTION("the real cookie from the Xauthority file") {
        sys::writeFileAtomic(tmp.path() / "xauth", xauthEntry(256, "elsewhere", display + "0", std::string(16, 'n')) +
                                       xauthEntry(256, sys::localHostName(), display, real));
        ScopedEnv xauth("XAUTHORITY", sys::fromPath(tmp.path() / "xauth"));
        Harness h;
        h.start(session());
        REQUIRE(h.listener.waitConnected());
        REQUIRE(h.listener.waitData("$ "));
        REQUIRE(server.waitForEvent("x11-req:MIT-MAGIC-COOKIE-1", 3000));

        h.transport->write("x11 good\r");
        REQUIRE(h.listener.waitData("x11 reply: X-SERVER-OK"));
        REQUIRE(x.received().size() == 1);
        CHECK(x.received()[0] == expectedSetup);

        // Without the fake cookie nothing reaches the X server.
        h.transport->write("x11 bad\r");
        REQUIRE(h.listener.waitData("wrong authorization"));
        h.transport->write("x11 none\r");
        REQUIRE(h.listener.waitData("no authorization"));
        std::this_thread::sleep_for(300ms);
        CHECK(x.received().size() == 1);
        INFO(h.listener.allLogs());
        CHECK(h.listener.allLogs().find("X11 connection refused") != std::string::npos);
    }
    SECTION("no cookie for the display: the X server gets none") {
        ScopedEnv xauth("XAUTHORITY", sys::fromPath(tmp.path() / "missing"));
        Harness h;
        h.start(session());
        REQUIRE(h.listener.waitConnected());
        REQUIRE(h.listener.waitData("$ "));
        h.transport->write("x11 good\r");
        REQUIRE(h.listener.waitData("x11 reply: X-SERVER-OK"));
        REQUIRE(x.received().size() == 1);
        CHECK(x.received()[0] == std::string("l\0\x0b\0\0\0\0\0\0\0\0\0", 12) + "after-setup");
        CHECK(h.listener.allLogs().find("no cookie for the local display") != std::string::npos);
    }
}

/* ---- a changed host key: no saved password, no forwarding, tunnels only when asked for (review 8) ---- */

namespace {

/** Stores another key than the server's for it, so the next connection sees a changed host key. */
void storeOtherHostKey(Harness& h, const TestSshServer& server) {
    KnownHost fake;
    fake.host = "127.0.0.1";
    fake.port = server.port();
    fake.keyType = "ssh-ed25519";
    ssh_key other = nullptr;
    ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &other);
    char* b64 = nullptr;
    ssh_pki_export_pubkey_base64(other, &b64);
    fake.key = b64;
    ssh_string_free_char(b64);
    ssh_key_free(other);
    h.knownHosts.add(fake);
}

/** A session with agent and X11 forwarding, a local tunnel to `echoPort` (its port in `localPort`) and a remote one. */
SessionConfig forwardingSession(const TestSshServer& server, int echoPort, int& localPort) {
    SessionConfig c = Harness::session(server);
    c.ssh.auth.agentForwarding = true;
    c.ssh.x11.enabled = true;
    c.ssh.x11.display = "localhost:42";
    { auto l = net::listenLoopback(0, localPort); }
    PortForward local;
    local.id = "l1";
    local.type = ForwardType::Local;
    local.sourcePort = localPort;
    local.destHost = "127.0.0.1";
    local.destPort = echoPort;
    PortForward remote;
    remote.id = "r1";
    remote.type = ForwardType::Remote;
    remote.sourcePort = 0;
    remote.destHost = "127.0.0.1";
    remote.destPort = echoPort;
    c.ssh.tunnels = {local, remote};
    return c;
}

/** Sends `text` through the local tunnel at `port`; true when it comes back. */
bool echoesThrough(int port, const std::string& text) {
    auto sock = net::connectTcp("127.0.0.1", port, 2000);
    if (!sock || !net::sendAll(sock.get(), text)) return false;
    std::string got;
    for (int i = 0; i < 50 && got.size() < text.size(); ++i) {
        if (!net::waitReadable(sock.get(), 100)) continue;
        char buf[256];
        const int n = recv(sock.get(), buf, sizeof buf, 0);
        if (n <= 0) break;
        got.append(buf, static_cast<size_t>(n));
    }
    return got == text;
}

bool hasSpecial(Transport& t, const std::string& id) {
    for (const auto& s : t.specials())
        if (s.id == id) return true;
    return false;
}

}  // namespace

TEST_CASE("ssh: after a changed host key: no saved password, no forwarding, tunnels when asked for", "[ssh][hostkey]") {
    for (const std::string choice : {"accept-once", "accept-save"}) {
        CAPTURE(choice);
        TestEchoServer echo;
        TestSshServerOptions o;
        o.allowKbdInt = false;
        TestSshServer server(o);
        Harness h;
        storeOtherHostKey(h, server);
        int localPort = 0;
        SessionConfig c = forwardingSession(server, echo.port(), localPort);
        h.credentials.set(Credentials::passwordKey(c.id), "secret");
        std::mutex pm;
        Json passwordPrompt;
        h.prompter.handler = [&](const Json& p) {
            const std::string kind = p.value("kind", "");
            if (kind == "hostkey") return accept(choice);
            if (kind == "password") {
                std::lock_guard l(pm);
                passwordPrompt = p;
                return answer({"secret"});
            }
            return PromptResponse{};
        };
        auto prompted = [&] {
            std::lock_guard l(pm);
            return passwordPrompt;
        };
        h.start(c);
        const bool connected = h.listener.waitConnected();
        INFO(h.listener.allLogs());
        REQUIRE(connected);
        REQUIRE(h.listener.waitData("$ "));

        // The saved password was not sent: the user typed it, told why.
        REQUIRE_FALSE(prompted().is_null());
        CHECK(prompted().dump().find("host key changed") != std::string::npos);
        CHECK(server.hasEvent("auth:password:ok"));
        // Neither agent nor X11 forwarding was asked for; no tunnel is open.
        CHECK_FALSE(server.hasEvent("auth-agent-req"));
        CHECK_FALSE(server.hasEvent("x11-req"));
        CHECK_FALSE(server.hasEvent("tcpip-forward"));
        CHECK_FALSE(net::connectTcp("127.0.0.1", localPort, 1000));
        {
            std::lock_guard l(h.listener.m);
            CHECK(h.listener.local.find("host key changed") != std::string::npos);
            CHECK(h.listener.local.find("Open this session's tunnels") != std::string::npos);
        }

        // The user opens the tunnels: both work, and the command is gone from the list. Forwarding stays off.
        REQUIRE(hasSpecial(*h.transport, "tunnels:open"));
        h.transport->sendSpecial("tunnels:open");
        REQUIRE(server.waitForEvent("tcpip-forward:", 3000));
        bool open = false;
        for (int i = 0; i < 20 && !open; ++i) {
            open = echoesThrough(localPort, "after asking");
            if (!open) std::this_thread::sleep_for(100ms);
        }
        CHECK(open);
        CHECK_FALSE(hasSpecial(*h.transport, "tunnels:open"));
        CHECK_FALSE(server.hasEvent("auth-agent-req"));
        CHECK_FALSE(server.hasEvent("x11-req"));
        h.transport.reset();

        // The next connection: accepted once, it asks again and is held back again; saved, everything is as before.
        {
            std::lock_guard l(pm);
            passwordPrompt = Json();
        }
        FakeListener second;
        TransportContext ctx = h.context(c);
        ctx.listener = &second;
        auto t = makeSshTransport(ctx);
        t->start();
        REQUIRE(second.waitConnected());
        REQUIRE(second.waitData("$ "));
        if (choice == "accept-save") {
            CHECK(prompted().is_null());  // the saved password
            CHECK(server.waitForEvent("auth-agent-req", 2000));
            CHECK_FALSE(hasSpecial(*t, "tunnels:open"));
        } else {
            CHECK_FALSE(prompted().is_null());
            CHECK(hasSpecial(*t, "tunnels:open"));
        }
    }
}

TEST_CASE("ssh: after a changed host key the one-time password is not sent either (keyboard-interactive)", "[ssh][hostkey]") {
    TestSshServer server;  // keyboard-interactive and password
    Harness h;
    storeOtherHostKey(h, server);
    std::mutex pm;
    Json kbdPrompt;
    h.prompter.handler = [&](const Json& p) {
        const std::string kind = p.value("kind", "");
        if (kind == "hostkey") return accept("accept-once");
        if (kind == "kbdint") {
            std::lock_guard l(pm);
            kbdPrompt = p;
            return answer(std::vector<std::string>(p["prompts"].size(), "secret"));
        }
        if (kind == "password") return answer({"secret"});
        return PromptResponse{};
    };
    TransportContext ctx = h.context(Harness::session(server));
    ctx.oneTimePassword = "secret";  // -pw
    h.transport = makeSshTransport(ctx);
    h.transport->start();
    REQUIRE(h.listener.waitConnected());
    std::lock_guard l(pm);
    REQUIRE_FALSE(kbdPrompt.is_null());
    CHECK(kbdPrompt.dump().find("host key changed") != std::string::npos);
}

TEST_CASE("ssh: a known or new host key keeps the saved password, forwarding and tunnels", "[ssh][hostkey]") {
    TestEchoServer echo;
    TestSshServerOptions o;
    o.allowKbdInt = false;
    TestSshServer server(o);
    Harness h;  // no stored key: a new one, accepted and saved
    int localPort = 0;
    SessionConfig c = forwardingSession(server, echo.port(), localPort);
    h.credentials.set(Credentials::passwordKey(c.id), "secret");
    for (int run = 0; run < 2; ++run) {  // a new key, then the same key again
        CAPTURE(run);
        FakeListener l;
        TransportContext ctx = h.context(c);
        ctx.listener = &l;
        auto t = makeSshTransport(ctx);
        t->start();
        REQUIRE(l.waitConnected());
        REQUIRE(l.waitData("$ "));
        auto kinds = h.prompter.kinds();
        CHECK(std::count(kinds.begin(), kinds.end(), "password") == 0);
        CHECK(server.waitForEvent("auth-agent-req", 2000));
        CHECK(server.waitForEvent("x11-req", 2000));
        CHECK(server.waitForEvent("tcpip-forward:", 2000));
        CHECK(echoesThrough(localPort, "as before"));
        CHECK_FALSE(hasSpecial(*t, "tunnels:open"));
        std::lock_guard lk(l.m);
        CHECK(l.local.find("host key changed") == std::string::npos);
    }
}

/* ---- dynamic tunnels: the SOCKS handshake is limited in size and time (review 9) ---- */

namespace {

struct SocksFixture {
    TestEchoServer echo;
    TestSshServer server;
    Harness h;
    int port = 0;

    SocksFixture() {
        SessionConfig c = Harness::session(server);
        { auto l = net::listenLoopback(0, port); }
        PortForward f;
        f.id = "d1";
        f.type = ForwardType::Dynamic;
        f.sourcePort = port;
        c.ssh.tunnels = {f};
        c.ssh.noShell = true;
        h.start(c);
        REQUIRE(h.listener.waitConnected());
        std::this_thread::sleep_for(200ms);
    }

    std::string echoPortBytes() const {
        return {static_cast<char>(echo.port() >> 8), static_cast<char>(echo.port() & 0xff)};
    }
};

/** Reads up to `n` bytes (fewer when the connection ends or nothing more comes within `timeoutMs`). */
std::string readSome(net::Handle s, size_t n, int timeoutMs = 3000) {
    std::string got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (got.size() < n && std::chrono::steady_clock::now() < until) {
        if (!net::waitReadable(s, 50)) continue;
        char buf[4096];
        const int r = recv(s, buf, static_cast<int>(std::min<size_t>(sizeof buf, n - got.size())), 0);
        if (r <= 0) break;
        got.append(buf, static_cast<size_t>(r));
    }
    return got;
}

/** True when the far side closed the connection within `timeoutMs`. */
bool closedByPeer(net::Handle s, int timeoutMs) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        if (!net::waitReadable(s, 50)) continue;
        char buf[256];
        if (recv(s, buf, sizeof buf, 0) <= 0) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("ssh: a SOCKS request longer than the limit is closed", "[ssh][forward][socks]") {
    SocksFixture f;
    auto sock = net::connectTcp("127.0.0.1", f.port, 3000);
    REQUIRE(sock);
    // SOCKS4 with a user id that never ends: before the fix it only ever grew.
    std::string req("\x04\x01", 2);
    req += f.echoPortBytes();
    req += std::string("\x7f\x00\x00\x01", 4);
    REQUIRE(net::sendAll(sock.get(), req + "user"));
    CHECK_FALSE(closedByPeer(sock.get(), 300));  // a short, still incomplete request waits
    (void)net::sendAll(sock.get(), std::string(kSocksMaxHandshake, 'u'));
    CHECK(closedByPeer(sock.get(), 3000));
    CHECK(f.h.listener.allLogs().find("longer than 1024 bytes") != std::string::npos);
    CHECK_FALSE(f.server.hasEvent("direct-tcpip"));
}

TEST_CASE("ssh: a SOCKS client that does not finish its request in time is closed", "[ssh][forward][socks]") {
    setSocksHandshakeTimeoutForTests(800);
    struct Reset {
        ~Reset() { setSocksHandshakeTimeoutForTests(0); }
    } reset;
    SocksFixture f;
    auto silent = net::connectTcp("127.0.0.1", f.port, 3000);   // sends nothing
    auto greeted = net::connectTcp("127.0.0.1", f.port, 3000);  // only the greeting
    REQUIRE(silent);
    REQUIRE(greeted);
    REQUIRE(net::sendAll(greeted.get(), std::string("\x05\x01\x00", 3)));
    REQUIRE(readSome(greeted.get(), 2) == std::string("\x05\x00", 2));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(closedByPeer(silent.get(), 5000));
    CHECK(closedByPeer(greeted.get(), 5000));
    CHECK(std::chrono::steady_clock::now() - t0 < 4s);
    CHECK(f.h.listener.allLogs().find("no complete SOCKS request in time") != std::string::npos);
}

TEST_CASE("ssh: SOCKS4, SOCKS4a and SOCKS5 requests still work, also slow ones and data right behind them", "[ssh][forward][socks]") {
    SocksFixture f;
    const std::string port = f.echoPortBytes();

    SECTION("SOCKS4") {
        auto s = net::connectTcp("127.0.0.1", f.port, 3000);
        REQUIRE(s);
        REQUIRE(net::sendAll(s.get(), std::string("\x04\x01", 2) + port + std::string("\x7f\x00\x00\x01", 4) + "user" + '\0'));
        const std::string reply = readSome(s.get(), 8);
        REQUIRE(reply.size() == 8);
        CHECK(reply[1] == 0x5a);
        REQUIRE(net::sendAll(s.get(), "four"));
        CHECK(readSome(s.get(), 4) == "four");
    }
    SECTION("SOCKS4a (a host name)") {
        auto s = net::connectTcp("127.0.0.1", f.port, 3000);
        REQUIRE(s);
        REQUIRE(net::sendAll(s.get(), std::string("\x04\x01", 2) + port + std::string("\x00\x00\x00\x01", 4) + '\0' +
                                          "127.0.0.1" + '\0'));
        const std::string reply = readSome(s.get(), 8);
        REQUIRE(reply.size() == 8);
        CHECK(reply[1] == 0x5a);
        REQUIRE(net::sendAll(s.get(), "four-a"));
        CHECK(readSome(s.get(), 6) == "four-a");
    }
    SECTION("SOCKS5, the request sent byte by byte") {
        auto s = net::connectTcp("127.0.0.1", f.port, 3000);
        REQUIRE(s);
        REQUIRE(net::sendAll(s.get(), std::string("\x05\x01\x00", 3)));
        REQUIRE(readSome(s.get(), 2) == std::string("\x05\x00", 2));
        const std::string req = std::string("\x05\x01\x00\x03\x09", 5) + "127.0.0.1" + port;
        for (char ch : req) {
            REQUIRE(net::sendAll(s.get(), std::string(1, ch)));
            std::this_thread::sleep_for(20ms);
        }
        const std::string reply = readSome(s.get(), 10);
        REQUIRE(reply.size() == 10);
        CHECK(reply[1] == 0);
        REQUIRE(net::sendAll(s.get(), "five"));
        CHECK(readSome(s.get(), 4) == "five");
    }
    SECTION("SOCKS5 with data right behind the request (more than the handshake limit)") {
        auto s = net::connectTcp("127.0.0.1", f.port, 3000);
        REQUIRE(s);
        REQUIRE(net::sendAll(s.get(), std::string("\x05\x01\x00", 3)));
        REQUIRE(readSome(s.get(), 2) == std::string("\x05\x00", 2));
        const std::string data(3 * kSocksMaxHandshake, 'd');
        REQUIRE(net::sendAll(s.get(), std::string("\x05\x01\x00\x01\x7f\x00\x00\x01", 8) + port + data));
        const std::string reply = readSome(s.get(), 10);
        REQUIRE(reply.size() == 10);
        CHECK(reply[1] == 0);
        CHECK(readSome(s.get(), data.size()) == data);
    }
}
