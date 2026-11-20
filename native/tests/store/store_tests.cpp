// Unit tests for the store module: Store (sessions/folders/settings/state), KnownHosts, Credentials, atomic file IO.
#include "core/Crypto.h"
#include "core/Strings.h"
#include "platform/sys/Fs.h"
#include "store/Credentials.h"
#include "store/JsonFile.h"
#include "store/KnownHosts.h"
#include "store/Store.h"
#include "store/Vault.h"

#include "TestServers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace st;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) { return sys::readFile(p).value_or(std::string()); }

void spit(const fs::path& p, const std::string& s) { sys::writeFileAtomic(p, s); }

/** SSH wire string: uint32 big-endian length + bytes. */
std::string sshString(std::string_view s) {
    std::string out;
    uint32_t n = static_cast<uint32_t>(s.size());
    out.push_back(static_cast<char>((n >> 24) & 0xff));
    out.push_back(static_cast<char>((n >> 16) & 0xff));
    out.push_back(static_cast<char>((n >> 8) & 0xff));
    out.push_back(static_cast<char>(n & 0xff));
    out.append(s);
    return out;
}

std::string fakeKeyBlob(const std::string& type, char fill) { return sshString(type) + sshString(std::string(32, fill)); }

size_t countFiles(const fs::path& dir, std::string_view contains) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (sys::fromPath(e.path().filename()).find(contains) != std::string::npos) ++n;
    return n;
}

Json sessionDoc(const std::string& name, const std::string& folder = {}) {
    return Json{{"name", name}, {"host", name + ".example.test"}, {"folder", folder}};
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* Atomic file IO                                                            */
/* ------------------------------------------------------------------------ */

TEST_CASE("store io: atomic write replaces content and leaves no temp files", "[store][io]") {
    test::TempDir tmp;
    fs::path f = tmp.path() / "sub" / "file.json";
    sys::writeFileAtomic(f, "first");
    REQUIRE(slurp(f) == "first");
    sys::writeFileAtomic(f, "second, longer content");
    REQUIRE(slurp(f) == "second, longer content");
    sys::writeFileAtomic(f, "");
    REQUIRE(sys::readFile(f).has_value());
    REQUIRE(sys::readFile(f)->empty());
    REQUIRE(countFiles(f.parent_path(), ".tmp-") == 0);
    REQUIRE_FALSE(sys::readFile(tmp.path() / "missing.json").has_value());
}

TEST_CASE("store io: loadJson tolerates BOM and comments, backs up corrupt files", "[store][io]") {
    test::TempDir tmp;
    fs::path f = tmp.path() / "x.json";
    spit(f, "\xEF\xBB\xBF{ // comment\n \"a\": 1 }");
    Json j;
    REQUIRE(storeio::loadJson(f, j) == storeio::LoadStatus::Ok);
    REQUIRE(j["a"] == 1);

    spit(f, "{ not json");
    REQUIRE(storeio::loadJson(f, j) == storeio::LoadStatus::Corrupt);
    REQUIRE_FALSE(fs::exists(f));
    REQUIRE(countFiles(tmp.path(), "x.json.corrupt-") == 1);
    REQUIRE(storeio::loadJson(f, j) == storeio::LoadStatus::Missing);
}

/* ------------------------------------------------------------------------ */
/* Credentials                                                               */
/* ------------------------------------------------------------------------ */

TEST_CASE("credentials: DPAPI protect/unprotect roundtrip", "[store][credentials]") {
    REQUIRE(Credentials::protect("").empty());
    std::string blob = Credentials::protect("s3cret \xE2\x82\xAC");
    REQUIRE_FALSE(blob.empty());
    REQUIRE(blob.find("s3cret") == std::string::npos);
    REQUIRE(Credentials::unprotect(blob) == std::string("s3cret \xE2\x82\xAC"));
    REQUIRE(Credentials::unprotect("") == std::string());
    REQUIRE_FALSE(Credentials::unprotect("!!!not base64!!!").has_value());
    REQUIRE_FALSE(Credentials::unprotect(crypto::base64Encode("garbage blob")).has_value());
}

TEST_CASE("credentials: set/get/has/remove persist encrypted", "[store][credentials]") {
    test::TempDir tmp;
    fs::path f = tmp.path() / "credentials.json";
    std::vector<std::string> changed;
    {
        Credentials c(f);
        c.load();
        c.onChanged = [&](const std::string& key) { changed.push_back(key); };
        REQUIRE_FALSE(c.has("k"));
        REQUIRE_FALSE(c.get("k").has_value());
        c.set(Credentials::passwordKey("abc"), "hunter2");
        c.set("other", "value2");
        REQUIRE(c.has("session:abc:password"));
        REQUIRE(c.get("session:abc:password") == std::string("hunter2"));
        REQUIRE(changed.size() == 2);
        REQUIRE(changed[0] == "session:abc:password");
        c.set("other", "");  // empty secret removes
        REQUIRE_FALSE(c.has("other"));
        c.remove("missing");  // no-op, no callback
        REQUIRE(changed.size() == 3);
    }
    std::string disk = slurp(f);
    REQUIRE(disk.find("hunter2") == std::string::npos);
    REQUIRE(disk.find("session:abc:password") != std::string::npos);

    Credentials again(f);
    again.load();
    REQUIRE(again.get(Credentials::passwordKey("abc")) == std::string("hunter2"));
    again.remove(Credentials::passwordKey("abc"));
    Credentials third(f);
    third.load();
    REQUIRE_FALSE(third.has(Credentials::passwordKey("abc")));
}

TEST_CASE("credentials: undecryptable entries and corrupt files", "[store][credentials]") {
    test::TempDir tmp;
    fs::path f = tmp.path() / "credentials.json";
    spit(f, R"({"bad": "AAAA", "notstring": 5})");
    Credentials c(f);
    c.load();
    REQUIRE(c.has("bad"));
    REQUIRE_FALSE(c.has("notstring"));
    REQUIRE_FALSE(c.get("bad").has_value());

    spit(f, "garbage");
    c.load();
    REQUIRE_FALSE(c.has("bad"));
    REQUIRE(countFiles(tmp.path(), "credentials.json.corrupt-") == 1);
    c.set("k", "v");
    REQUIRE(c.get("k") == std::string("v"));
}

TEST_CASE("credentials: change callback runs outside the lock", "[store][credentials]") {
    test::TempDir tmp;
    Credentials c(tmp.path() / "credentials.json");
    c.load();
    bool ok = false;
    c.onChanged = [&](const std::string& key) {
        // Another thread must be able to use the object while the callback runs.
        auto fut = std::async(std::launch::async, [&] { return c.has(key); });
        ok = fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready && fut.get();
    };
    c.set("x", "y");
    REQUIRE(ok);
}

/* ------------------------------------------------------------------------ */
/* KnownHosts                                                                */
/* ------------------------------------------------------------------------ */

TEST_CASE("known hosts: add/check/find semantics", "[store][knownhosts]") {
    test::TempDir tmp;
    KnownHosts kh(tmp.path() / "known_hosts.json");
    kh.load();
    int changes = 0;
    kh.onChanged = [&] { ++changes; };

    const std::string ed = fakeKeyBlob("ssh-ed25519", 'a');
    const std::string ed2 = fakeKeyBlob("ssh-ed25519", 'b');
    const std::string rsa = fakeKeyBlob("ssh-rsa", 'c');

    REQUIRE(kh.check("Server.Test", 22, "ssh-ed25519", ed) == HostKeyCheck::Unknown);

    KnownHost e;
    e.host = "  Server.Test ";
    e.port = 22;
    e.key = crypto::base64Encode(ed);
    KnownHost stored = kh.add(e);
    REQUIRE(changes == 1);
    REQUIRE_FALSE(stored.id.empty());
    REQUIRE(stored.keyType == "ssh-ed25519");
    REQUIRE(stored.fingerprint == crypto::fingerprintSha256(ed));
    REQUIRE(stored.addedAt > 0);
    REQUIRE(stored.source == "user");
    REQUIRE(stored.host == "Server.Test");

    // raw blob or base64; host case-insensitive; empty key type derived from the blob
    REQUIRE(kh.check("server.test", 22, "ssh-ed25519", ed) == HostKeyCheck::Match);
    REQUIRE(kh.check("SERVER.TEST", 22, "", crypto::base64Encode(ed)) == HostKeyCheck::Match);
    REQUIRE(kh.check("server.test", 22, "ssh-ed25519", ed2) == HostKeyCheck::Mismatch);
    REQUIRE(kh.check("server.test", 2222, "ssh-ed25519", ed) == HostKeyCheck::Unknown);
    // A different key type than the stored ones counts as unknown (PuTTY behaviour)
    REQUIRE(kh.check("server.test", 22, "rsa-sha2-512", rsa) == HostKeyCheck::Unknown);

    // RSA signature algorithm names map to ssh-rsa
    KnownHost r;
    r.host = "[::1]";
    r.port = 22;
    r.keyType = "rsa-sha2-256";
    r.key = rsa;  // raw blob accepted on add
    KnownHost rs = kh.add(r);
    REQUIRE(rs.keyType == "ssh-rsa");
    REQUIRE(rs.key == crypto::base64Encode(rsa));
    REQUIRE(kh.check("::1", 22, "rsa-sha2-512", rsa) == HostKeyCheck::Match);
    REQUIRE(kh.check("[::1]", 22, "ssh-rsa", fakeKeyBlob("ssh-rsa", 'd')) == HostKeyCheck::Mismatch);
    REQUIRE(kh.find("::1", 22).size() == 1);

    // Replacing the key of the same host/port/type keeps a single entry
    e.key = crypto::base64Encode(ed2);
    kh.add(e);
    REQUIRE(kh.find("server.test", 22).size() == 1);
    REQUIRE(kh.check("server.test", 22, "ssh-ed25519", ed2) == HostKeyCheck::Match);
    REQUIRE(kh.check("server.test", 22, "ssh-ed25519", ed) == HostKeyCheck::Mismatch);

    // Invalid entries are rejected
    KnownHost bad;
    bad.host = "x";
    REQUIRE_THROWS(kh.add(bad));
    REQUIRE(kh.list().size() == 2);

    // Persistence
    KnownHosts again(tmp.path() / "known_hosts.json");
    again.load();
    REQUIRE(again.list().size() == 2);
    REQUIRE(again.check("server.test", 22, "ssh-ed25519", ed2) == HostKeyCheck::Match);

    // Remove
    const int before = changes;
    again.onChanged = [&] { ++changes; };
    again.remove({rs.id, "unknown-id"});
    REQUIRE(changes == before + 1);
    again.remove({"unknown-id"});
    REQUIRE(changes == before + 1);
    REQUIRE(again.list().size() == 1);
}

TEST_CASE("known hosts: addMany skips identical entries and invalid ones", "[store][knownhosts]") {
    test::TempDir tmp;
    KnownHosts kh(tmp.path() / "known_hosts.json");
    kh.load();
    int changes = 0;
    kh.onChanged = [&] { ++changes; };
    std::vector<KnownHost> in(3);
    in[0].host = "a.test";
    in[0].key = crypto::base64Encode(fakeKeyBlob("ssh-ed25519", '1'));
    in[0].source = "putty-import";
    in[1].host = "b.test";
    in[1].port = 2200;
    in[1].key = crypto::base64Encode(fakeKeyBlob("ecdsa-sha2-nistp256", '2'));
    in[2].host = "";  // invalid
    REQUIRE(kh.addMany(in) == 2);
    REQUIRE(changes == 1);
    REQUIRE(kh.addMany(in) == 0);
    REQUIRE(changes == 1);
    REQUIRE(kh.list().size() == 2);
    REQUIRE(kh.find("A.TEST", 22).at(0).source == "putty-import");
}

TEST_CASE("known hosts: corrupt file and invalid entries", "[store][knownhosts]") {
    test::TempDir tmp;
    fs::path f = tmp.path() / "known_hosts.json";
    Json doc = {{"hosts", Json::array({Json{{"host", "ok.test"}, {"port", 22}, {"key", crypto::base64Encode(fakeKeyBlob("ssh-ed25519", 'z'))}},
                                       Json{{"host", ""}, {"key", "AAAA"}}, Json{{"host", "nokey.test"}}})}};
    spit(f, doc.dump());
    KnownHosts kh(f);
    kh.load();
    REQUIRE(kh.list().size() == 1);
    REQUIRE_FALSE(kh.list()[0].fingerprint.empty());

    spit(f, "[[[");
    kh.load();
    REQUIRE(kh.list().empty());
    REQUIRE(countFiles(tmp.path(), "known_hosts.json.corrupt-") == 1);
}

TEST_CASE("known hosts: callback runs outside the lock", "[store][knownhosts]") {
    test::TempDir tmp;
    KnownHosts kh(tmp.path() / "known_hosts.json");
    kh.load();
    bool ok = false;
    kh.onChanged = [&] {
        auto fut = std::async(std::launch::async, [&] { return kh.list().size(); });
        ok = fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready && fut.get() == 1;
    };
    KnownHost e;
    e.host = "h.test";
    e.key = crypto::base64Encode(fakeKeyBlob("ssh-ed25519", 'q'));
    kh.add(e);
    REQUIRE(ok);
}

/* ------------------------------------------------------------------------ */
/* Store: sessions                                                           */
/* ------------------------------------------------------------------------ */

TEST_CASE("store: empty data dir loads defaults", "[store][sessions]") {
    test::TempDir tmp;
    Store store(tmp.path() / "data");
    store.load();
    REQUIRE(store.listSessionsJson().empty());
    REQUIRE(store.folders().empty());
    REQUIRE(store.settings().is_object());
    REQUIRE(store.getState("missing").is_null());
    REQUIRE(store.logDirectory() == tmp.path() / "data" / "logs");
    SessionConfig def = store.defaultSessionTemplate();
    REQUIRE(def.id == "default");
    REQUIRE(def.port == 22);
    REQUIRE(def.connection.terminalType == "xterm-256color");
}

TEST_CASE("store: save session normalizes, preserves unknown fields and persists", "[store][sessions]") {
    test::TempDir tmp;
    int changes = 0;
    std::string savedId;
    {
        Store store(tmp.path());
        store.load();
        store.onSessionsChanged = [&] { ++changes; };
        Json in = sessionDoc("Web", "/Prod//EU/");
        in["futureField"] = {{"x", 1}};
        in["ssh"] = {{"compression", true}, {"uiOnly", "keep"}};
        in["port"] = "not a number";  // wrong type -> repaired to default
        Json out = store.saveSession(in);
        savedId = out["id"].get<std::string>();
        REQUIRE_FALSE(savedId.empty());
        REQUIRE(out["folder"] == "Prod/EU");
        REQUIRE(out["port"] == 22);
        REQUIRE(out["futureField"]["x"] == 1);
        REQUIRE(out["ssh"]["compression"] == true);
        REQUIRE(out["ssh"]["uiOnly"] == "keep");
        REQUIRE(out["ssh"]["allocatePty"] == true);  // default filled in
        REQUIRE(out["createdAt"].get<int64_t>() > 0);
        REQUIRE(out["updatedAt"].get<int64_t>() >= out["createdAt"].get<int64_t>());
        REQUIRE(changes == 1);

        // Update keeps createdAt
        Json upd = out;
        upd["createdAt"] = 0;
        upd["host"] = "changed.test";
        Json out2 = store.saveSession(upd);
        REQUIRE(out2["createdAt"] == out["createdAt"]);
        REQUIRE(store.listSessionsJson().size() == 1);
        REQUIRE(changes == 2);

        auto typed = store.getSession(savedId);
        REQUIRE(typed.has_value());
        REQUIRE(typed->host == "changed.test");
        REQUIRE(typed->ssh.compression);
        REQUIRE_FALSE(store.getSession("nope").has_value());
    }
    Store again(tmp.path());
    again.load();
    auto doc = again.getSessionJson(savedId);
    REQUIRE(doc.has_value());
    REQUIRE((*doc)["futureField"]["x"] == 1);
    REQUIRE((*doc)["ssh"]["uiOnly"] == "keep");
    REQUIRE((*doc)["host"] == "changed.test");
}

TEST_CASE("store: typed save overlays onto the stored document", "[store][sessions]") {
    test::TempDir tmp;
    Store store(tmp.path());
    store.load();
    Json in = sessionDoc("A");
    in["uiExtra"] = 42;
    std::string id = store.saveSession(in)["id"];
    SessionConfig s = *store.getSession(id);
    s.port = 2222;
    s.appearance = {{"fontSize", 15}};
    SessionConfig out = store.saveSession(s);
    REQUIRE(out.port == 2222);
    auto doc = *store.getSessionJson(id);
    REQUIRE(doc["uiExtra"] == 42);
    REQUIRE(doc["appearance"]["fontSize"] == 15);

    SessionConfig fresh;
    fresh.name = "Fresh";
    fresh.host = "fresh.test";
    SessionConfig created = store.saveSession(fresh);
    REQUIRE_FALSE(created.id.empty());
    REQUIRE(store.listSessions().size() == 2);
}

TEST_CASE("store: saveSessions bulk and deleteSessions (removes saved passwords)", "[store][sessions]") {
    test::TempDir tmp;
    Store store(tmp.path());
    store.load();
    int changes = 0;
    store.onSessionsChanged = [&] { ++changes; };
    Json arr = Json::array({sessionDoc("one"), sessionDoc("two"), sessionDoc("three"), 5});
    arr[0]["id"] = "id-1";
    arr[1]["id"] = "id-2";
    store.saveSessions(arr);
    REQUIRE(changes == 1);
    REQUIRE(store.listSessionsJson().size() == 3);
    // Bulk move into a folder
    Json moved = Json::array({*store.getSessionJson("id-1"), *store.getSessionJson("id-2")});
    for (auto& s : moved) s["folder"] = "Moved";
    store.saveSessions(moved);
    REQUIRE((*store.getSessionJson("id-1"))["folder"] == "Moved");
    REQUIRE(store.listSessionsJson().size() == 3);

    store.credentials().set(Credentials::passwordKey("id-1"), "pw");
    store.deleteSessions({"id-1", "zzz"});
    REQUIRE(store.listSessionsJson().size() == 2);
    REQUIRE_FALSE(store.credentials().has(Credentials::passwordKey("id-1")));
    const int before = changes;
    store.deleteSessions({"zzz"});  // nothing deleted -> no notification
    REQUIRE(changes == before);
}

TEST_CASE("store: duplicate ids on disk are repaired on load", "[store][sessions]") {
    test::TempDir tmp;
    Json doc = {{"sessions", Json::array({Json{{"id", "dup"}, {"name", "a"}}, Json{{"id", "dup"}, {"name", "b"}}, "junk"})}};
    spit(tmp.path() / "sessions.json", doc.dump());
    Store store(tmp.path());
    store.load();
    auto list = store.listSessionsJson();
    REQUIRE(list.size() == 2);
    REQUIRE(list[0]["id"] != list[1]["id"]);
}

TEST_CASE("store: proxy password is DPAPI-encrypted at rest", "[store][sessions]") {
    test::TempDir tmp;
    Json in = sessionDoc("P");
    in["proxy"] = {{"type", "socks5"}, {"host", "proxy.test"}, {"password", "pr0xyPass"}};
    std::string id;
    {
        Store store(tmp.path());
        store.load();
        Json out = store.saveSession(in);
        id = out["id"];
        REQUIRE(out["proxy"]["password"] == "pr0xyPass");  // plain in memory / over the bridge
        store.updateSettings({{"defaultSession", {{"proxy", {{"password", "defPass"}}}}}});
    }
    std::string disk = slurp(tmp.path() / "sessions.json");
    REQUIRE(disk.find("pr0xyPass") == std::string::npos);
    REQUIRE(disk.find("\"enc:") != std::string::npos);
    std::string settingsDisk = slurp(tmp.path() / "settings.json");
    REQUIRE(settingsDisk.find("defPass") == std::string::npos);
    REQUIRE(settingsDisk.find("\"enc:") != std::string::npos);

    Store again(tmp.path());
    again.load();
    REQUIRE(again.getSession(id)->proxy.password == "pr0xyPass");
    REQUIRE(again.defaultSessionTemplate().proxy.password == "defPass");
    REQUIRE(again.settings()["defaultSession"]["proxy"]["password"] == "defPass");

    // Legacy plain-text value is accepted and re-encrypted on the next save
    Json raw = Json::parse(disk);
    raw["sessions"][0]["proxy"]["password"] = "legacyPlain";
    spit(tmp.path() / "sessions.json", raw.dump());
    Store legacy(tmp.path());
    legacy.load();
    REQUIRE(legacy.getSession(id)->proxy.password == "legacyPlain");
    legacy.touchSession(id);
    REQUIRE(slurp(tmp.path() / "sessions.json").find("legacyPlain") == std::string::npos);

    // Undecryptable value -> cleared, not crashing
    raw["sessions"][0]["proxy"]["password"] = "enc:AAAA";
    spit(tmp.path() / "sessions.json", raw.dump());
    Store broken(tmp.path());
    broken.load();
    REQUIRE(broken.getSession(id)->proxy.password.empty());
}

TEST_CASE("store: touchSession updates lastUsedAt without notification", "[store][sessions]") {
    test::TempDir tmp;
    Store store(tmp.path());
    store.load();
    std::string id = store.saveSession(sessionDoc("t"))["id"];
    int changes = 0;
    store.onSessionsChanged = [&] { ++changes; };
    store.touchSession(id);
    store.touchSession("unknown");
    REQUIRE(store.getSession(id)->lastUsedAt > 0);
    REQUIRE(changes == 0);
}

TEST_CASE("store: findSessionByName and sessionNameExists", "[store][sessions]") {
    test::TempDir tmp;
    Store store(tmp.path());
    store.load();
    Json a = sessionDoc("Router", "Lab");
    a["id"] = "lab-router";
    Json b = sessionDoc("router");
    b["id"] = "root-router";
    Json c = sessionDoc("Only In Folder", "Lab/Deep");
    c["id"] = "deep";
    store.saveSessions(Json::array({a, b, c}));

    REQUIRE(store.findSessionByName("ROUTER")->id == "root-router");
    REQUIRE(store.findSessionByName("lab/router")->id == "lab-router");
    REQUIRE(store.findSessionByName("  only in folder ")->id == "deep");
    REQUIRE(store.findSessionByName("Lab/Deep/Only In Folder")->id == "deep");
    REQUIRE_FALSE(store.findSessionByName("nothing").has_value());
    REQUIRE_FALSE(store.findSessionByName("").has_value());

    REQUIRE(store.sessionNameExists("Lab", "router"));
    REQUIRE(store.sessionNameExists("/Lab/", "ROUTER"));
    REQUIRE_FALSE(store.sessionNameExists("Lab", "only in folder"));
    REQUIRE(store.sessionNameExists("", "Router"));
}

/* ------------------------------------------------------------------------ */
/* Store: folders                                                            */
/* ------------------------------------------------------------------------ */

TEST_CASE("store: folders explicit + implied, rename re-parents, delete keeps or removes", "[store][folders]") {
    test::TempDir tmp;
    Store store(tmp.path());
    store.load();
    int changes = 0;
    store.onSessionsChanged = [&] { ++changes; };

    store.createFolder("/Empty/");
    store.createFolder("Empty");  // duplicate: no-op
    REQUIRE(changes == 1);
    REQUIRE_THROWS(store.createFolder("//"));

    Json s1 = sessionDoc("s1", "Prod/EU/DB");
    s1["id"] = "s1";
    Json s2 = sessionDoc("s2", "Prod");
    s2["id"] = "s2";
    Json s3 = sessionDoc("s3", "Production");  // prefix but not a child of Prod
    s3["id"] = "s3";
    store.saveSessions(Json::array({s1, s2, s3}));
    store.createFolder("Prod/US");

    REQUIRE(store.folders() == std::vector<std::string>{"Empty", "Prod", "Prod/EU", "Prod/EU/DB", "Prod/US", "Production"});

    // Rename re-parents sessions and sub-folders, leaves look-alike prefixes alone
    store.renameFolder("Prod", "Live/Main");
    REQUIRE((*store.getSessionJson("s1"))["folder"] == "Live/Main/EU/DB");
    REQUIRE((*store.getSessionJson("s2"))["folder"] == "Live/Main");
    REQUIRE((*store.getSessionJson("s3"))["folder"] == "Production");
    auto f = store.folders();
    REQUIRE(std::find(f.begin(), f.end(), "Live/Main/US") != f.end());
    REQUIRE(std::find(f.begin(), f.end(), "Prod") == f.end());
    REQUIRE_THROWS(store.renameFolder("", "x"));
    REQUIRE_THROWS(store.renameFolder("Live", ""));

    // Delete keeping contents: sessions + sub-folders move to the parent
    store.credentials().set(Credentials::passwordKey("s1"), "pw");
    store.deleteFolder("Live/Main", false);
    REQUIRE((*store.getSessionJson("s2"))["folder"] == "Live");
    REQUIRE((*store.getSessionJson("s1"))["folder"] == "Live/EU/DB");
    f = store.folders();
    REQUIRE(std::find(f.begin(), f.end(), "Live/US") != f.end());
    REQUIRE(std::find(f.begin(), f.end(), "Live/Main") == f.end());
    REQUIRE(store.credentials().has(Credentials::passwordKey("s1")));

    // Delete with sessions: everything beneath goes (and their saved passwords)
    store.deleteFolder("Live", true);
    REQUIRE_FALSE(store.getSessionJson("s1").has_value());
    REQUIRE_FALSE(store.getSessionJson("s2").has_value());
    REQUIRE(store.getSessionJson("s3").has_value());
    REQUIRE_FALSE(store.credentials().has(Credentials::passwordKey("s1")));
    REQUIRE(store.folders() == std::vector<std::string>{"Empty", "Production"});

    // Persisted
    Store again(tmp.path());
    again.load();
    REQUIRE(again.folders() == std::vector<std::string>{"Empty", "Production"});
}

TEST_CASE("store: deleting a top-level folder keeping sessions moves them to the root", "[store][folders]") {
    test::TempDir tmp;
    Store store(tmp.path());
    store.load();
    Json s = sessionDoc("x", "Top/Sub");
    s["id"] = "x";
    store.saveSession(s);
    store.deleteFolder("Top", false);
    REQUIRE((*store.getSessionJson("x"))["folder"] == "Sub");
    REQUIRE(store.folders() == std::vector<std::string>{"Sub"});
}

/* ------------------------------------------------------------------------ */
/* Store: settings / state                                                   */
/* ------------------------------------------------------------------------ */

TEST_CASE("store: settings deep merge, arrays replace, callback + persistence", "[store][settings]") {
    test::TempDir tmp;
    Json last;
    int calls = 0;
    {
        Store store(tmp.path());
        store.load();
        store.onSettingsChanged = [&](const Json& s) {
            last = s;
            ++calls;
        };
        store.updateSettings({{"appearance", {{"theme", "dark"}, {"uiScale", 1.25}}}, {"customSchemes", Json::array({1, 2, 3})}});
        Json r = store.updateSettings({{"appearance", {{"theme", "light"}}}, {"customSchemes", Json::array({9})}, {"x", nullptr}});
        REQUIRE(r["appearance"]["theme"] == "light");
        REQUIRE(r["appearance"]["uiScale"] == 1.25);
        REQUIRE(r["customSchemes"] == Json::array({9}));
        REQUIRE_FALSE(r.contains("x"));
        REQUIRE(calls == 2);
        REQUIRE(last == r);
        REQUIRE_THROWS(store.updateSettings(Json::array()));

        store.updateSettings({{"defaultSession", {{"port", 2022}, {"username", "me"}, {"uiField", true}}}, {"logging", {{"directory", "mylogs"}}}});
        SessionConfig def = store.defaultSessionTemplate();
        REQUIRE(def.port == 2022);
        REQUIRE(def.username == "me");
        REQUIRE(def.id == "default");
        REQUIRE(store.settings()["defaultSession"]["uiField"] == true);
        REQUIRE(store.settings()["defaultSession"]["ssh"]["allocatePty"] == true);  // normalized
        REQUIRE(store.logDirectory() == (tmp.path() / "mylogs").lexically_normal());
        // An absolute folder is used as it is (C:\... on Windows, /... on macOS).
        const fs::path absolute = (tmp.path() / "Logs" / "Here").lexically_normal();
        store.updateSettings({{"logging", {{"directory", sys::fromPath(absolute)}}}});
        REQUIRE(store.logDirectory() == absolute);
    }
    Store again(tmp.path());
    again.load();
    REQUIRE(again.settings()["appearance"]["theme"] == "light");
    REQUIRE(again.defaultSessionTemplate().port == 2022);
}

TEST_CASE("store: state keys get/set/remove and persist", "[store][state]") {
    test::TempDir tmp;
    {
        Store store(tmp.path());
        store.load();
        store.setState("window", {{"x", 10}, {"y", 20}, {"maximized", false}});
        store.setState("workspace", Json::array({1, 2}));
        store.setState("gone", 1);
        store.setState("gone", nullptr);
        REQUIRE(store.getState("window")["x"] == 10);
        REQUIRE(store.getState("gone").is_null());
        REQUIRE_THROWS(store.setState("", 1));
    }
    Store again(tmp.path());
    again.load();
    REQUIRE(again.getState("window")["y"] == 20);
    REQUIRE(again.getState("workspace") == Json::array({1, 2}));
    REQUIRE(again.getState("gone").is_null());
}

TEST_CASE("store: corrupt files are backed up and replaced by defaults", "[store][corrupt]") {
    test::TempDir tmp;
    spit(tmp.path() / "sessions.json", "{\"sessions\": [ {\"name\": ");
    spit(tmp.path() / "settings.json", "[1,2,3]");
    spit(tmp.path() / "state.json", "");
    spit(tmp.path() / "known_hosts.json", "nope");
    spit(tmp.path() / "credentials.json", "{{");
    Store store(tmp.path());
    store.load();
    REQUIRE(store.listSessionsJson().empty());
    REQUIRE(store.settings() == Json::object());
    REQUIRE(store.getState("x").is_null());
    REQUIRE(countFiles(tmp.path(), "sessions.json.corrupt-") == 1);
    REQUIRE(countFiles(tmp.path(), "settings.json.corrupt-") == 1);
    REQUIRE(countFiles(tmp.path(), "state.json.corrupt-") == 1);
    REQUIRE(countFiles(tmp.path(), "known_hosts.json.corrupt-") == 1);
    REQUIRE(countFiles(tmp.path(), "credentials.json.corrupt-") == 1);
    // The store keeps working afterwards
    store.saveSession(sessionDoc("new"));
    store.updateSettings({{"a", 1}});
    Store again(tmp.path());
    again.load();
    REQUIRE(again.listSessionsJson().size() == 1);
    REQUIRE(again.settings()["a"] == 1);
}

TEST_CASE("store: bare array sessions file (legacy shape) is accepted", "[store][corrupt]") {
    test::TempDir tmp;
    spit(tmp.path() / "sessions.json", R"([{"id":"a","name":"A","host":"h"}])");
    Store store(tmp.path());
    store.load();
    REQUIRE(store.listSessions().size() == 1);
    REQUIRE(store.getSession("a")->host == "h");
}

/* ------------------------------------------------------------------------ */
/* Concurrency                                                               */
/* ------------------------------------------------------------------------ */

TEST_CASE("store: concurrent writers smoke test", "[store][concurrency]") {
    test::TempDir tmp;
    Store store(tmp.path());
    store.load();
    std::atomic<int> notifications{0};
    store.onSessionsChanged = [&] { ++notifications; };
    constexpr int kThreads = 6, kPerThread = 15;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            try {
                for (int i = 0; i < kPerThread; ++i) {
                    Json s = sessionDoc("t" + std::to_string(t) + "-" + std::to_string(i), "T" + std::to_string(t));
                    std::string id = store.saveSession(s)["id"];
                    store.touchSession(id);
                    store.updateSettings({{"counter" + std::to_string(t), i}});
                    store.setState("k" + std::to_string(t), i);
                    store.credentials().set("c" + std::to_string(t), "v" + std::to_string(i));
                    (void)store.listSessions();
                    (void)store.folders();
                    KnownHost kh;
                    kh.host = "h" + std::to_string(t) + ".test";
                    kh.key = crypto::base64Encode(fakeKeyBlob("ssh-ed25519", static_cast<char>('a' + i)));
                    store.knownHosts().add(kh);
                }
            } catch (const std::exception&) {
                ++failures;
            }
        });
    }
    for (auto& th : threads) th.join();
    REQUIRE(failures == 0);
    REQUIRE(store.listSessionsJson().size() == kThreads * kPerThread);
    REQUIRE(notifications == kThreads * kPerThread);
    REQUIRE(countFiles(tmp.path(), ".tmp-") == 0);

    Store again(tmp.path());
    again.load();
    REQUIRE(again.listSessionsJson().size() == kThreads * kPerThread);
    REQUIRE(again.folders().size() == kThreads);
    REQUIRE(again.knownHosts().list().size() == kThreads);
    for (int t = 0; t < kThreads; ++t) {
        REQUIRE(again.settings()["counter" + std::to_string(t)] == kPerThread - 1);
        REQUIRE(again.getState("k" + std::to_string(t)) == kPerThread - 1);
        REQUIRE(again.credentials().get("c" + std::to_string(t)) == "v" + std::to_string(kPerThread - 1));
    }
}

TEST_CASE("vault: keys of Credentials (linked files and stored text)", "[store][vault]") {
    test::TempDir tmp;
    Credentials creds(tmp.path() / "credentials.json");
    creds.load();
    const fs::path file = tmp.path() / "vault.json";
    int changes = 0;
    std::string storedId, fileId;
    const std::string secret = "-----BEGIN OPENSSH PRIVATE KEY-----\nsecret-material\n-----END OPENSSH PRIVATE KEY-----\n";
    {
        Vault v(file, creds);
        v.load();
        v.onChanged = [&] { ++changes; };
        VaultKey stored;
        stored.name = "  work laptop ";
        stored.keyType = "ssh-ed25519";
        stored.fingerprint = "SHA256:abc";
        storedId = v.addKeyText(stored, secret).id;
        VaultKey linked;
        linked.name = "Router";
        linked.path = "C:/keys/router.ppk";
        fileId = v.addKeyFile(linked).id;
        CHECK(changes == 2);

        auto list = v.list();
        REQUIRE(list.size() == 2);
        CHECK(list[0].name == "Router");  // sorted by name, case-insensitively
        CHECK(list[1].name == "work laptop");
        CHECK(list[1].kind == "stored");
        CHECK(list[0].kind == "file");
        CHECK(v.storedKeyText(storedId) == secret);
        CHECK_FALSE(v.storedKeyText(fileId).has_value());  // a linked file's content is never stored

        // Names are unique, whatever the case; empty names are refused.
        VaultKey dup;
        dup.name = "WORK LAPTOP";
        CHECK_THROWS(v.addKeyText(dup, secret));
        CHECK_THROWS(v.rename(fileId, "work laptop"));
        CHECK_THROWS(v.rename(fileId, "  "));
        CHECK(v.rename(fileId, "home router").name == "home router");
    }
    // The secret is in credentials.json, encrypted; vault.json holds no key material.
    CHECK(slurp(file).find("secret-material") == std::string::npos);
    CHECK(slurp(tmp.path() / "credentials.json").find("secret-material") == std::string::npos);

    Vault again(file, creds);
    again.load();
    REQUIRE(again.list().size() == 2);
    CHECK(again.get(fileId)->name == "home router");
    CHECK(again.get(fileId)->path == "C:/keys/router.ppk");
    CHECK(again.storedKeyText(storedId) == secret);

    // Removing a stored key removes its secret too.
    again.remove(storedId);
    CHECK_FALSE(again.get(storedId).has_value());
    CHECK_FALSE(creds.has(Vault::secretKey(storedId)));
    again.remove("no-such-id");  // ignored

    // A corrupt file is backed up and the vault starts empty.
    spit(file, "{ not json");
    Vault broken(file, creds);
    broken.load();
    CHECK(broken.list().empty());
}

TEST_CASE("vault: logins (user name, password stored / asked / none, key, agent)", "[store][vault]") {
    test::TempDir tmp;
    Credentials creds(tmp.path() / "credentials.json");
    creds.load();
    const fs::path file = tmp.path() / "vault.json";
    std::string prodId;
    {
        Vault v(file, creds);
        v.load();
        VaultLogin prod;
        prod.name = " deploy@prod ";
        prod.username = " deploy ";
        prod.password = "stored";
        CHECK_THROWS(v.addLogin(prod, std::nullopt));  // "stored" needs the password
        prodId = v.addLogin(prod, std::string("hunter2")).id;
        CHECK(v.getLogin(prodId)->name == "deploy@prod");
        CHECK(v.getLogin(prodId)->username == "deploy");
        CHECK(v.loginPassword(prodId) == std::string("hunter2"));

        VaultLogin lab;
        lab.name = "admin@lab";
        lab.username = "admin";
        lab.password = "ask";
        lab.keyId = "some-key";
        lab.useAgent = true;
        const std::string labId = v.addLogin(lab, std::string("ignored")).id;
        CHECK_FALSE(v.loginPassword(labId).has_value());  // only a "stored" login has a password
        CHECK_FALSE(creds.has(Vault::loginSecretKey(labId)));

        VaultLogin dup = lab;
        dup.name = "ADMIN@LAB";
        CHECK_THROWS(v.addLogin(dup, std::nullopt));
        VaultLogin noUser = lab;
        noUser.name = "x";
        noUser.username = " ";
        CHECK_THROWS(v.addLogin(noUser, std::nullopt));
        // A key and a login may share a name (separate lists).
        VaultKey k;
        k.name = "admin@lab";
        k.path = "C:/k";
        CHECK_NOTHROW(v.addKeyFile(k));

        // Update: keep the stored password (none given), then a new one, then switch to "ask" (deletes it).
        VaultLogin p = *v.getLogin(prodId);
        p.username = "deployer";
        v.updateLogin(p, std::nullopt);
        CHECK(v.loginPassword(prodId) == std::string("hunter2"));
        v.updateLogin(p, std::string("s3cret"));
        CHECK(v.loginPassword(prodId) == std::string("s3cret"));
        p.password = "ask";
        v.updateLogin(p, std::nullopt);
        CHECK_FALSE(creds.has(Vault::loginSecretKey(prodId)));
        p.password = "stored";
        CHECK_THROWS(v.updateLogin(p, std::nullopt));  // back to "stored" needs a password again
        v.updateLogin(p, std::string("again"));

        auto list = v.listLogins();
        REQUIRE(list.size() == 2);
        CHECK(list[0].name == "admin@lab");
        CHECK(list[1].username == "deployer");
    }
    CHECK(slurp(file).find("again") == std::string::npos);
    CHECK(slurp(tmp.path() / "credentials.json").find("again") == std::string::npos);

    Vault again(file, creds);
    again.load();
    REQUIRE(again.listLogins().size() == 2);
    CHECK(again.loginPassword(prodId) == std::string("again"));
    again.removeLogin(prodId);
    CHECK_FALSE(again.getLogin(prodId).has_value());
    CHECK_FALSE(creds.has(Vault::loginSecretKey(prodId)));
}

TEST_CASE("vault: remembered key passphrases (per key, forgotten with the key)", "[store][vault]") {
    test::TempDir tmp;
    Credentials creds(tmp.path() / "credentials.json");
    creds.load();
    const fs::path file = tmp.path() / "vault.json";
    std::string linkedId, storedId;
    {
        Vault v(file, creds);
        v.load();
        VaultKey linked;
        linked.name = "linked";
        linked.path = "C:/keys/id_enc";
        linked.encrypted = true;
        linkedId = v.addKeyFile(linked).id;
        VaultKey stored;
        stored.name = "stored";
        stored.encrypted = true;
        storedId = v.addKeyText(stored, "-----BEGIN OPENSSH PRIVATE KEY-----\nAAAA\n-----END OPENSSH PRIVATE KEY-----\n").id;
        int changes = 0;
        v.onChanged = [&] { ++changes; };

        CHECK_FALSE(v.passphrase(linkedId).has_value());
        CHECK_FALSE(v.get(linkedId)->passphraseRemembered);
        v.rememberPassphrase(linkedId, "correct horse");
        v.rememberPassphrase(storedId, "battery staple");
        CHECK(changes == 2);
        CHECK(v.passphrase(linkedId) == std::string("correct horse"));
        CHECK(v.get(linkedId)->passphraseRemembered);
        CHECK(v.list()[0].passphraseRemembered);
        CHECK(v.rename(linkedId, "linked, renamed").passphraseRemembered);  // by id: a rename keeps it
        CHECK_THROWS(v.rememberPassphrase("no-such-key", "x"));
        CHECK_FALSE(v.passphrase("no-such-key").has_value());

        v.forgetPassphrase(linkedId);
        CHECK_FALSE(v.passphrase(linkedId).has_value());
        CHECK_FALSE(v.get(linkedId)->passphraseRemembered);
        const int before = changes;
        v.forgetPassphrase(linkedId);  // nothing to forget: no change event
        CHECK(changes == before);
    }
    // Only encrypted in credentials.json; vault.json does not even say that one is remembered.
    CHECK(slurp(file).find("battery") == std::string::npos);
    CHECK(slurp(file).find("passphraseRemembered") == std::string::npos);
    CHECK(slurp(tmp.path() / "credentials.json").find("battery") == std::string::npos);

    Vault again(file, creds);
    again.load();
    CHECK(again.passphrase(storedId) == std::string("battery staple"));
    again.remove(storedId);  // deleting the key forgets its passphrase too
    CHECK_FALSE(creds.has(Vault::passphraseSecretKey(storedId)));
}

TEST_CASE("vault: master password (set, lock, unlock, wrong, change, remove, reset)", "[store][vault][master]") {
    Vault::setKdfCost(1024, 1);  // fast for tests; the app uses 64 MiB, 3 passes
    test::TempDir tmp;
    Credentials creds(tmp.path() / "credentials.json");
    creds.load();
    const fs::path file = tmp.path() / "vault.json";
    const std::string keyText = "-----BEGIN OPENSSH PRIVATE KEY-----\nMASTERTEST\n-----END OPENSSH PRIVATE KEY-----\n";
    creds.set(Credentials::passwordKey("s1"), "session-own");  // a session's own password: never touched
    Vault v(file, creds);
    v.load();
    VaultKey stored;
    stored.name = "stored";
    const std::string storedId = v.addKeyText(stored, keyText).id;
    VaultKey linked;
    linked.name = "linked";
    linked.path = "C:/keys/id";
    const std::string linkedId = v.addKeyFile(linked).id;
    v.rememberPassphrase(linkedId, "pp-secret");
    VaultLogin l;
    l.name = "prod";
    l.username = "deploy";
    l.password = "stored";
    const std::string loginId = v.addLogin(l, std::string("hunter2")).id;

    CHECK(v.status() == "none");
    CHECK_FALSE(v.locked());
    CHECK_THROWS(v.setMaster("short"));
    v.setMaster("correct horse battery");
    CHECK(v.status() == "unlocked");
    CHECK(v.storedKeyText(storedId) == keyText);
    // Sealed under the OS protection: the raw value is "mp1:..." and does not hold the key text.
    const auto raw = creds.get(Vault::secretKey(storedId));
    REQUIRE(raw.has_value());
    CHECK(raw->rfind("mp1:", 0) == 0);
    CHECK(raw->find("MASTERTEST") == std::string::npos);
    CHECK(creds.get(Vault::loginSecretKey(loginId))->rfind("mp1:", 0) == 0);
    CHECK(creds.get(Credentials::passwordKey("s1")) == std::string("session-own"));
    CHECK_THROWS(v.setMaster("another one"));  // already set

    // Locked: the secrets cannot be read or written; the public parts and links still work.
    v.lock();
    CHECK(v.status() == "locked");
    CHECK(v.needsUnlock(Vault::secretKey(storedId)));
    CHECK_FALSE(v.needsUnlock(Vault::secretKey(linkedId)));  // a linked key has no secret
    CHECK_THROWS_AS(v.storedKeyText(storedId), VaultLocked);
    CHECK_THROWS_AS(v.loginPassword(loginId), VaultLocked);
    CHECK_THROWS_AS(v.passphrase(linkedId), VaultLocked);
    CHECK_THROWS_AS(v.addKeyText(VaultKey{.name = "new"}, keyText), VaultLocked);
    CHECK(v.list().size() == 2);
    VaultKey linked2;
    linked2.name = "linked 2";
    linked2.path = "C:/keys/id2";
    CHECK_NOTHROW(v.addKeyFile(linked2));

    CHECK_FALSE(v.unlock("wrong password"));
    CHECK(v.status() == "locked");
    CHECK(v.unlock("correct horse battery"));
    CHECK(v.loginPassword(loginId) == std::string("hunter2"));
    CHECK(v.passphrase(linkedId) == std::string("pp-secret"));

    // After a restart: locked again.
    {
        Vault restarted(file, creds);
        restarted.load();
        CHECK(restarted.status() == "locked");
        CHECK(restarted.unlock("correct horse battery"));
        CHECK(restarted.storedKeyText(storedId) == keyText);
    }

    // Change: the old password no longer works.
    CHECK_THROWS(v.changeMaster("wrong password", "new master pw"));
    v.changeMaster("correct horse battery", "new master pw");
    CHECK(v.status() == "unlocked");
    CHECK(v.storedKeyText(storedId) == keyText);
    v.lock();
    CHECK_FALSE(v.unlock("correct horse battery"));
    CHECK(v.unlock("new master pw"));

    // Remove: back to OS protection only.
    CHECK_THROWS(v.removeMaster("wrong password"));
    v.removeMaster("new master pw");
    CHECK(v.status() == "none");
    CHECK(creds.get(Vault::secretKey(storedId)) == keyText);
    CHECK_FALSE(creds.has(Vault::kMasterRecord));

    // Forgotten: reset deletes every secret; linked keys and logins stay (a stored password becomes "ask").
    v.setMaster("forgotten later");
    v.lock();
    v.reset();
    CHECK(v.status() == "none");
    CHECK_FALSE(v.get(storedId).has_value());
    CHECK(v.get(linkedId).has_value());
    CHECK_FALSE(v.get(linkedId)->passphraseRemembered);
    CHECK(v.getLogin(loginId)->password == "ask");
    CHECK(creds.keysWithPrefix("vault:").empty());
    CHECK(creds.get(Credentials::passwordKey("s1")) == std::string("session-own"));
}

TEST_CASE("vault: export and import (one sealed file, ids kept, merged)", "[store][vault][export]") {
    Vault::setKdfCost(1024, 1);
    test::TempDir tmp;
    const std::string keyText = "-----BEGIN OPENSSH PRIVATE KEY-----\nEXPORTTEST\n-----END OPENSSH PRIVATE KEY-----\n";
    Credentials creds(tmp.path() / "a-credentials.json");
    creds.load();
    Vault a(tmp.path() / "a-vault.json", creds);
    a.load();
    VaultKey stored;
    stored.name = "work";
    stored.fingerprint = "SHA256:work";
    const std::string storedId = a.addKeyText(stored, keyText).id;
    VaultKey linked;
    linked.name = "router";
    linked.path = "C:/keys/router.ppk";
    linked.fingerprint = "SHA256:router";
    const std::string linkedId = a.addKeyFile(linked).id;
    a.rememberPassphrase(linkedId, "pp-secret");
    VaultLogin l;
    l.name = "prod";
    l.username = "deploy";
    l.password = "stored";
    l.keyId = storedId;
    const std::string loginId = a.addLogin(l, std::string("hunter2")).id;

    CHECK_THROWS(a.exportData("short", true));
    const auto withSecrets = a.exportData("file password", true);
    CHECK(withSecrets.keys == 2);
    CHECK(withSecrets.logins == 1);
    CHECK(withSecrets.secrets == 3);  // the key text, the passphrase, the login password
    CHECK(withSecrets.text.find("EXPORTTEST") == std::string::npos);
    CHECK(withSecrets.text.find("hunter2") == std::string::npos);
    CHECK(withSecrets.text.find("router") == std::string::npos);  // even names and paths are inside the sealed part
    const auto noSecrets = a.exportData("file password", false);
    CHECK(noSecrets.secrets == 1);  // a stored key's text is the key itself: always in

    // Another computer: a fresh store with a master password, unlocked.
    Credentials creds2(tmp.path() / "b-credentials.json");
    creds2.load();
    Vault b(tmp.path() / "b-vault.json", creds2);
    b.load();
    VaultKey clash;
    clash.name = "work";  // a different key with the same name: the imported one is renamed
    clash.path = "C:/other";
    b.addKeyFile(clash);
    b.setMaster("master password");
    CHECK_THROWS_WITH(b.importData(withSecrets.text, "wrong password"), Catch::Matchers::ContainsSubstring("Wrong password"));
    CHECK_THROWS_WITH(b.importData("{\"hello\":1}", "file password"), Catch::Matchers::ContainsSubstring("Not a Credentials file"));
    b.lock();
    CHECK_THROWS_AS(b.importData(withSecrets.text, "file password"), VaultLocked);
    CHECK(b.listLogins().empty());  // nothing half done
    REQUIRE(b.unlock("master password"));

    const auto r = b.importData(withSecrets.text, "file password");
    CHECK(r.keysAdded == 2);
    CHECK(r.loginsAdded == 1);
    CHECK(r.secrets == 3);
    REQUIRE(r.renamed.size() == 1);
    CHECK(r.renamed[0] == "key work → work (2)");
    CHECK(b.get(storedId)->name == "work (2)");  // the id is kept: sessions that use it find it
    CHECK(b.storedKeyText(storedId) == keyText);
    CHECK(creds2.get(Vault::secretKey(storedId))->rfind("mp1:", 0) == 0);  // sealed with this store's master password
    CHECK(b.passphrase(linkedId) == std::string("pp-secret"));
    CHECK(b.getLogin(loginId)->keyId == storedId);
    CHECK(b.loginPassword(loginId) == std::string("hunter2"));

    // Again: everything is already here.
    const auto again = b.importData(withSecrets.text, "file password");
    CHECK(again.keysAdded == 0);
    CHECK(again.keysSkipped == 2);
    CHECK(again.loginsSkipped == 1);

    // Without its secrets, a login that stored its password asks for it.
    Credentials creds3(tmp.path() / "c-credentials.json");
    creds3.load();
    Vault c(tmp.path() / "c-vault.json", creds3);
    c.load();
    c.importData(noSecrets.text, "file password");
    CHECK(c.getLogin(loginId)->password == "ask");
    CHECK_FALSE(c.passphrase(linkedId).has_value());
    CHECK(c.storedKeyText(storedId) == keyText);
}
