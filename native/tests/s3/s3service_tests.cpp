// S3 sessions end to end, the way the UI drives them: conn:open with protocol "s3" (S3Transport: sign-in, the secret
// asked and saved, a wrong secret asked again, an untrusted certificate asked about and remembered, errors, access keys
// from a login in Credentials), then
// sftp:open / list / upload / download / presign through SftpService and the transfer engine.
#include "TestBridge.h"
#include "TestS3Server.h"
#include "TestServers.h"

#include "platform/Platform.h"
#include "platform/sys/Fs.h"
#include "session/SessionManager.h"
#include "sftp/SftpService.h"
#include "store/Store.h"
#include "store/Vault.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace st;
using namespace st::test;

namespace {

struct S3SessionFixture {
    TempDir data, local;
    std::unique_ptr<TestS3Server> server;
    std::unique_ptr<Store> store;
    BridgeHarness h;
    std::unique_ptr<SessionManager> sessions;
    std::unique_ptr<SftpService> sftp;
    SessionConfig cfg = makeDefaultSession();
    int conns = 0;

    explicit S3SessionFixture(TestS3Options o = {}) {
        h.dispatcher.uiThread = std::this_thread::get_id();  // the transports' prompts come from their own threads
        server = std::make_unique<TestS3Server>(o);
        server->createBucket("bucket");
        store = std::make_unique<Store>(data.path());
        store->load();
        sessions = std::make_unique<SessionManager>(*store, h.bridge);
        registerConnHandlers(h.bridge, *sessions);
        sftp = std::make_unique<SftpService>(*sessions, h.bridge);
        sftp->registerHandlers();
        cfg.protocol = Protocol::S3;
        cfg.s3.preset = "minio";
        cfg.s3.endpoint = server->endpoint();
        cfg.s3.auth.method = "keys";
        cfg.s3.auth.accessKeyId = "AKIDTEST";
        cfg.s3.systemProxy = false;
        cfg.connection.connectTimeoutSeconds = 5;
    }
    ~S3SessionFixture() {
        sftp->closeAll();
        sessions->closeAll();
    }

    /** `password`: the one-time password (`-pw`), '' = none. */
    std::string open(const std::string& password = "") {
        const std::string connId = "pane#" + std::to_string(++conns);
        ConnOpenRequest req;
        req.connId = connId;
        req.session = cfg;
        req.password = password;
        h.invoke("conn:open", Json::array({Json(req)}));
        return connId;
    }

    /** Waits for the nth prompt of `connId` of `kind` and returns it. */
    Json prompt(const std::string& connId, const std::string& kind, size_t nth = 1) {
        for (int i = 0; i < 1500; ++i) {
            size_t seen = 0;
            for (const auto& p : h.eventsOn("prompts:onPrompt"))
                if (p["args"][0]["connId"] == connId && p["args"][0]["kind"] == kind && ++seen == nth) return p["args"][0];
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::string seen;
        {
            std::lock_guard l(h.m);
            for (const auto& e : h.events) seen += e.dump().substr(0, 300) + "\n";
        }
        FAIL("no " + kind + " prompt for " + connId + "; events:\n" + seen);
    }

    void answer(const Json& prompt, Json response) {
        response["cancelled"] = false;
        h.send("prompts:respond", Json::array({prompt["id"], response}));
    }

    /** "connected" or "error"; the message in `message`. */
    std::string waitEnd(const std::string& connId, std::string* message = nullptr) {
        std::string status;
        REQUIRE(h.waitEvent([&](const Json& e) {
            if (e["ch"] != "conn:onStatus" || e["args"][0]["connId"] != connId) return false;
            const std::string s = e["args"][0]["status"];
            if (s != "connected" && s != "error" && s != "disconnected") return false;
            status = s;
            if (message) *message = e["args"][0].value("message", "");
            return true;
        }));
        return status;
    }

    /** The prompts of `connId` so far, by kind. */
    std::vector<std::string> promptKinds(const std::string& connId) {
        std::vector<std::string> out;
        for (const auto& p : h.eventsOn("prompts:onPrompt"))
            if (p["args"][0]["connId"] == connId) out.push_back(p["args"][0].value("kind", ""));
        return out;
    }

    /** A login in Credentials (user name = access key ID, password = secret); returns its id. */
    std::string addLogin(const std::string& name, const std::string& keyId, const std::string& mode,
                         const std::optional<std::string>& secret = std::nullopt) {
        VaultLogin l;
        l.name = name;
        l.username = keyId;
        l.password = mode;
        return store->vault().addLogin(l, secret).id;
    }

    Json waitDone(const std::string& transferId) {
        Json last;
        REQUIRE(h.waitEvent([&](const Json& e) {
            if (e["ch"] != "sftp:onProgress" || e["args"][0]["transferId"] != transferId) return false;
            const std::string s = e["args"][0]["state"];
            last = e["args"][0];
            return s == "done" || s == "error" || s == "cancelled";
        }, std::chrono::seconds(60)));
        return last;
    }
};

}  // namespace

TEST_CASE("s3 session: the secret is asked, saved, and the file manager works through the transfer engine") {
    S3SessionFixture f;
    f.server->put("bucket", "remote.txt", "from s3");
    const std::string conn = f.open();
    const Json p = f.prompt(conn, "password");
    CHECK(p["username"] == "AKIDTEST");
    CHECK(p["allowSave"] == true);
    f.answer(p, {{"values", {"test-secret"}}, {"remember", true}});
    REQUIRE(f.waitEnd(conn) == "connected");
    CHECK(f.store->credentials().get(Credentials::passwordKey(f.cfg.id)) == "test-secret");

    const Json opened = f.h.invoke("sftp:open", Json::array({conn, Json::object()}));
    CHECK(opened["protocol"] == "s3");
    CHECK(opened["home"] == "/");
    CHECK(opened["capabilities"]["rootIsBuckets"] == true);
    CHECK(opened["capabilities"]["chmod"] == false);
    const std::string id = opened["sftpId"];
    const Json root = f.h.invoke("sftp:list", Json::array({id, "/"}));
    REQUIRE(root.size() == 1);
    CHECK(root[0]["name"] == "bucket");

    // Upload a local file, then download the remote one.
    spit(f.local.path() / "up.txt", "to s3");
    const Json up = f.h.invoke("sftp:upload", Json::array({id, "/bucket", Json::array({sys::fromPath(f.local.path() / "up.txt")}), Json::object()}));
    CHECK(f.waitDone(up[0])["state"] == "done");
    CHECK(f.server->get("bucket", "up.txt")->data == "to s3");
    const Json down = f.h.invoke("sftp:download", Json::array({id, Json::array({"/bucket/remote.txt"}), sys::fromPath(f.local.path()), Json::object()}));
    CHECK(f.waitDone(down[0])["state"] == "done");
    CHECK(slurp(f.local.path() / "remote.txt") == "from s3");

    const std::string link = f.h.invoke("sftp:presign", Json::array({id, "/bucket/remote.txt", 600}));
    CHECK(link.find("X-Amz-Signature=") != std::string::npos);
    f.h.invoke("sftp:rename", Json::array({id, "/bucket/up.txt", "/bucket/renamed.txt"}));
    CHECK(f.server->get("bucket", "renamed.txt"));
    CHECK_THROWS_WITH(f.h.invoke("sftp:chmod", Json::array({id, "/bucket/renamed.txt", 0600})),
                      Catch::Matchers::ContainsSubstring("Permissions"));

    // The second connection uses the saved secret: no prompt.
    const std::string conn2 = f.open();
    REQUIRE(f.waitEnd(conn2) == "connected");
    size_t prompts = 0;
    for (const auto& e : f.h.eventsOn("prompts:onPrompt"))
        if (e["args"][0]["connId"] == conn2) ++prompts;
    CHECK(prompts == 0);
    CHECK(f.server->badSignatures() == 0);
}

TEST_CASE("s3 session: a wrong secret is asked again; a session bucket is checked") {
    S3SessionFixture f;
    f.cfg.s3.bucket = "bucket";
    f.cfg.s3.startPath = "/photos/";
    const std::string conn = f.open();
    f.answer(f.prompt(conn, "password"), {{"values", {"wrong"}}});
    const Json again = f.prompt(conn, "password", 2);
    CHECK(again.value("message", "").find("not accepted") != std::string::npos);
    f.answer(again, {{"values", {"test-secret"}}});
    REQUIRE(f.waitEnd(conn) == "connected");
    const Json opened = f.h.invoke("sftp:open", Json::array({conn, Json::object()}));
    CHECK(opened["home"] == "/photos");
    CHECK(opened["capabilities"]["rootIsBuckets"] == false);

    f.cfg.s3.bucket = "nobucket";
    const std::string conn2 = f.open();
    f.answer(f.prompt(conn2, "password"), {{"values", {"test-secret"}}});
    std::string message;
    CHECK(f.waitEnd(conn2, &message) == "error");
    CHECK(message.find("does not exist") != std::string::npos);
}

TEST_CASE("s3 session: an untrusted certificate is asked about, then remembered in Known hosts") {
    TestS3Options o;
    o.tls = true;
    S3SessionFixture f(o);
    f.store->credentials().set(Credentials::passwordKey(f.cfg.id), "test-secret");
    const std::string conn = f.open();
    const Json p = f.prompt(conn, "certificate");
    CHECK(p["sha256"] == f.server->http().certificateSha256());
    CHECK(p["subject"] == "st-test");
    CHECK(p["status"] == "unknown");
    // Before the answer only the certificate probe, which sends nothing signed: a HEAD without headers on Windows,
    // nothing at all on macOS (it stops after the TLS handshake; HttpSession::probeCertificate).
    if (std::string(sys::osName()) == "win32")
        CHECK(f.server->http().log() == std::vector<std::string>{"HEAD /"});
    else
        CHECK(f.server->http().log().empty());
    f.answer(p, {{"choice", "accept-save"}});
    REQUIRE(f.waitEnd(conn) == "connected");
    bool saved = false;
    for (const auto& k : f.store->knownHosts().list())
        if (k.keyType == "x509-sha256" && k.host == "127.0.0.1" && k.port == f.server->http().port()) saved = true;
    CHECK(saved);

    const std::string conn2 = f.open();
    REQUIRE(f.waitEnd(conn2) == "connected");
    for (const auto& e : f.h.eventsOn("prompts:onPrompt")) CHECK(e["args"][0]["connId"] != conn2);

    // Rejected: the session ends.
    S3SessionFixture g(o);
    const std::string conn3 = g.open();
    Json reject = g.prompt(conn3, "certificate");
    g.answer(reject, {{"choice", "reject"}});
    CHECK(g.waitEnd(conn3) != "connected");
}

TEST_CASE("s3 session: sign-in errors are readable") {
    S3SessionFixture f;
    f.cfg.s3.auth.method = "anonymous";
    std::string message;
    CHECK(f.waitEnd(f.open(), &message) == "error");
    CHECK(message.find("bucket") != std::string::npos);

    f.cfg.s3.auth.method = "env";
    ScopedEnv a("AWS_ACCESS_KEY_ID", "");
    CHECK(f.waitEnd(f.open(), &message) == "error");
    CHECK(message.find("AWS_ACCESS_KEY_ID") != std::string::npos);

    f.cfg.s3.auth.method = "keys";
    f.cfg.s3.endpoint = "ftp://nope";
    CHECK(f.waitEnd(f.open(), &message) == "error");
    CHECK(message.find("not an http") != std::string::npos);
}

// ---- Access keys from a login in Credentials (user name = access key ID, password = secret access key) ----

TEST_CASE("s3 session: a stored login signs in without a prompt, and wins over the session's own key", "[s3][vault]") {
    S3SessionFixture f;
    f.cfg.s3.auth.accessKeyId = "AKIDSESSION";  // the login's user name wins
    f.store->credentials().set(Credentials::passwordKey(f.cfg.id), "the-session-secret");
    f.cfg.s3.auth.loginId = f.addLogin("minio", " AKIDTEST ", "stored", std::string("test-secret"));

    const std::string conn = f.open();
    REQUIRE(f.waitEnd(conn) == "connected");
    CHECK(f.promptKinds(conn).empty());
    CHECK(f.server->badSignatures() == 0);
    // The session's own saved secret is left alone, and nothing new is saved for the session.
    CHECK(f.store->credentials().get(Credentials::passwordKey(f.cfg.id)) == std::string("the-session-secret"));

    // A second session with the same login: no prompt either.
    f.cfg.id = "another-session";
    f.cfg.s3.bucket = "bucket";
    const std::string conn2 = f.open();
    REQUIRE(f.waitEnd(conn2) == "connected");
    CHECK(f.promptKinds(conn2).empty());

    // A new secret saved in the login is what the next connection uses.
    VaultLogin l = *f.store->vault().getLogin(f.cfg.s3.auth.loginId);
    f.store->vault().updateLogin(l, std::string("an-old-secret"));
    const std::string conn3 = f.open();
    const Json p = f.prompt(conn3, "password");
    CHECK(p.value("message", "").find("not accepted") != std::string::npos);
    f.answer(p, {{"values", {"test-secret"}}});
    REQUIRE(f.waitEnd(conn3) == "connected");
}

TEST_CASE("s3 session: a login that asks every time: the prompt has no \"save\", and nothing is saved", "[s3][vault]") {
    S3SessionFixture f;
    f.cfg.s3.auth.accessKeyId = "";
    const std::string login = GENERATE(std::string("ask"), std::string("none"));
    f.cfg.s3.auth.loginId = f.addLogin("asking", "AKIDTEST", login);

    const std::string conn = f.open();
    const Json p = f.prompt(conn, "password");
    CHECK(p["username"] == "AKIDTEST");  // from the login, not asked
    CHECK(p["allowSave"] == false);
    f.answer(p, {{"values", {"test-secret"}}, {"remember", true}});  // even if the page sent "remember"
    REQUIRE(f.waitEnd(conn) == "connected");
    CHECK_FALSE(f.store->credentials().get(Credentials::passwordKey(f.cfg.id)).has_value());
    CHECK_FALSE(f.store->vault().loginPassword(f.cfg.s3.auth.loginId).has_value());
    CHECK(f.store->vault().getLogin(f.cfg.s3.auth.loginId)->password == login);
    for (const auto& k : f.promptKinds(conn)) CHECK(k == "password");  // no "Access key ID" question
}

TEST_CASE("s3 session: a stored login with a wrong secret is asked again, and the login is not changed", "[s3][vault]") {
    S3SessionFixture f;
    f.cfg.s3.auth.loginId = f.addLogin("stale", "AKIDTEST", "stored", std::string("an-old-secret"));
    const std::string conn = f.open();
    const Json p = f.prompt(conn, "password");
    CHECK(p.value("message", "").find("secret access key was not accepted") != std::string::npos);
    CHECK(p["allowSave"] == false);
    f.answer(p, {{"values", {"test-secret"}}, {"remember", true}});
    REQUIRE(f.waitEnd(conn) == "connected");
    CHECK(f.store->vault().loginPassword(f.cfg.s3.auth.loginId) == std::string("an-old-secret"));
    CHECK_FALSE(f.store->credentials().get(Credentials::passwordKey(f.cfg.id)).has_value());
}

TEST_CASE("s3 session: a login with a wrong access key ID says so", "[s3][vault]") {
    S3SessionFixture f;
    f.cfg.s3.auth.loginId = f.addLogin("typo", "AKIDTYPO", "stored", std::string("test-secret"));
    const std::string conn = f.open();
    const Json p = f.prompt(conn, "password");
    CHECK(p.value("message", "").find("access key ID was not accepted") != std::string::npos);
    CHECK(p["username"] == "AKIDTYPO");
    f.h.send("prompts:respond", Json::array({p["id"], Json{{"cancelled", true}}}));
    CHECK(f.waitEnd(conn) != "connected");
}

TEST_CASE("s3 session: a deleted login falls back to the session's own access key, as SSH does", "[s3][vault]") {
    S3SessionFixture f;
    f.cfg.s3.auth.loginId = f.addLogin("gone", "AKIDOTHER", "stored", std::string("other-secret"));
    f.store->vault().removeLogin(f.cfg.s3.auth.loginId);

    SECTION("the session's saved secret: no prompt") {
        f.store->credentials().set(Credentials::passwordKey(f.cfg.id), "test-secret");
        const std::string conn = f.open();
        REQUIRE(f.waitEnd(conn) == "connected");
        CHECK(f.promptKinds(conn).empty());
        CHECK(f.server->badSignatures() == 0);  // AKIDTEST, the session's own key
        bool logged = false;
        for (const auto& e : f.h.eventsOn("conn:onEventLog"))
            if (e.dump().find("no longer exists; using this session's own access key") != std::string::npos) logged = true;
        CHECK(logged);
    }
    SECTION("nothing saved: the secret is asked, and may be saved for the session") {
        const std::string conn = f.open();
        const Json p = f.prompt(conn, "password");
        CHECK(p["username"] == "AKIDTEST");
        CHECK(p["allowSave"] == true);
        f.answer(p, {{"values", {"test-secret"}}, {"remember", true}});
        REQUIRE(f.waitEnd(conn) == "connected");
        CHECK(f.store->credentials().get(Credentials::passwordKey(f.cfg.id)) == std::string("test-secret"));
    }
}

TEST_CASE("s3 session: the one-time password (-pw) wins over the login's stored secret", "[s3][vault]") {
    S3SessionFixture f;
    f.cfg.s3.auth.loginId = f.addLogin("stale", "AKIDTEST", "stored", std::string("an-old-secret"));
    const std::string conn = f.open("test-secret");
    REQUIRE(f.waitEnd(conn) == "connected");
    CHECK(f.promptKinds(conn).empty());
    CHECK(f.server->badSignatures() == 0);
}

TEST_CASE("s3 session: locked Credentials ask for the master password first", "[s3][vault][master]") {
    Vault::setKdfCost(1024, 1);
    S3SessionFixture f;
    f.cfg.s3.auth.loginId = f.addLogin("sealed", "AKIDTEST", "stored", std::string("test-secret"));
    Vault& vault = f.store->vault();
    vault.setMaster("master password");
    vault.lock();

    SECTION("a wrong master password asks again; the right one unlocks and signs in") {
        const std::string conn = f.open();
        f.answer(f.prompt(conn, "unlock"), {{"values", {"wrong one"}}});
        const Json again = f.prompt(conn, "unlock", 2);
        CHECK(again.value("message", "").find("Wrong master password") != std::string::npos);
        f.answer(again, {{"values", {"master password"}}});
        REQUIRE(f.waitEnd(conn) == "connected");
        for (const auto& k : f.promptKinds(conn)) CHECK(k == "unlock");
        CHECK(vault.status() == "unlocked");
    }

    SECTION("already unlocked: no prompt") {
        REQUIRE(vault.unlock("master password"));
        const std::string conn = f.open();
        REQUIRE(f.waitEnd(conn) == "connected");
        CHECK(f.promptKinds(conn).empty());
    }

    SECTION("cancelled: the connection stops, Credentials stay locked") {
        const std::string conn = f.open();
        const Json p = f.prompt(conn, "unlock");
        f.h.send("prompts:respond", Json::array({p["id"], Json{{"cancelled", true}}}));
        CHECK(f.waitEnd(conn) != "connected");
        for (const auto& k : f.promptKinds(conn)) CHECK(k == "unlock");
        CHECK(vault.status() == "locked");
        CHECK(f.server->count("ListBuckets") == 0);
    }

    SECTION("a login that asks every time needs no unlock") {
        VaultLogin l = *vault.getLogin(f.cfg.s3.auth.loginId);
        REQUIRE(vault.unlock("master password"));
        l.password = "ask";
        vault.updateLogin(l, std::nullopt);
        vault.lock();
        const std::string conn = f.open();
        f.answer(f.prompt(conn, "password"), {{"values", {"test-secret"}}});
        REQUIRE(f.waitEnd(conn) == "connected");
        for (const auto& k : f.promptKinds(conn)) CHECK(k == "password");
    }
}
