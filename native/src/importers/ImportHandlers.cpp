// Bridge handlers: importer:*, keys:*, knownHosts:importOpenSsh.
#include "importers/ImportHandlers.h"

#include "bridge/Bridge.h"
#include "core/Strings.h"
#include "importers/Importers.h"
#include "keys/Keygen.h"
#include "platform/sys/Env.h"
#include "platform/sys/Fs.h"
#include "platform/sys/Time.h"
#include "platform/ui/Dialogs.h"
#include "store/Store.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace st {

namespace {

namespace fs = std::filesystem;

/** ~/.ssh/known_hosts (the default for the import dialog); empty when the home folder is unknown. */
std::string defaultKnownHostsPath() {
    const fs::path home = sys::homeDirectory();
    return home.empty() ? std::string() : sys::fromPath(home / ".ssh" / "known_hosts");
}

void writeBytes(const std::string& path, const std::string& bytes) {
    std::ofstream f(sys::toPath(path), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write " + path);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!f) throw std::runtime_error("Cannot write " + path);
}

/** Today's local date, "YYYY-MM-DD" (default file names). */
std::string todayIso() {
    std::tm tm{};
    sys::localTime(std::time(nullptr), tm);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::optional<std::string> pathOrDialog(const Json& args, size_t i, FileDialogOptions opts) {
    const std::string p = argStr(args, i);
    if (!p.empty()) return p;
    return ui::dialogs::openFile(opts);
}

/** A Credentials entry with its public details from an inspected key; the name defaults to the key's comment. */
VaultKey vaultEntry(const KeyInfo& info, const std::string& name, const std::string& fallbackName) {
    VaultKey k;
    k.name = trim(name).empty() ? (trim(info.comment).empty() ? fallbackName : trim(info.comment)) : trim(name);
    k.format = info.format;
    k.keyType = info.keyType;
    k.fingerprint = info.fingerprint;
    k.publicKeyOpenSsh = info.publicKeyOpenSsh;
    k.comment = info.comment;
    k.encrypted = info.encrypted;
    return k;
}

/** A login's password argument: a string (possibly empty) = given, anything else (null, missing) = not given. */
std::optional<std::string> optionalPassword(const Json& args, size_t i) {
    if (args.is_array() && i < args.size() && args[i].is_string()) return args[i].get<std::string>();
    return std::nullopt;
}

/** vault:list's entry: the stored public part, plus whether a linked file is missing now. */
Json vaultJson(const VaultKey& k) {
    Json j = k;
    std::error_code ec;
    j["missing"] = k.kind == "file" && !fs::is_regular_file(sys::toPath(k.path), ec);
    return j;
}

std::vector<SessionConfig> sessionsByIds(const Store& store, const std::vector<std::string>& ids) {
    std::vector<SessionConfig> out;
    for (const auto& id : ids)
        if (auto s = store.getSession(id)) out.push_back(*s);
    return out;
}

}  // namespace

void registerImportHandlers(Bridge& bridge, Store& store) {
    bridge.handleAsync("importer:scanPuttyRegistry", [&store](const Json&) { return toJson(importers::previewPuttyRegistry(store)); });

    bridge.handleAsync("importer:parsePuttyRegFile", [&store](const Json& a) -> Json {
        auto path = pathOrDialog(a, 0, {"Import PuTTY sessions from a .reg file", "", {{"Registry files", {"reg"}}, {"All files", {"*"}}}});
        if (!path) return nullptr;
        return toJson(importers::previewPuttyRegFile(*path, store));
    });

    bridge.handleAsync("importer:scanSshConfig", [&store](const Json& a) { return toJson(importers::previewSshConfig(argStr(a, 0), store)); });

    bridge.handleAsync("importer:parseJsonFile", [&store](const Json& a) -> Json {
        auto path = pathOrDialog(a, 0, {"Import an SSHTermaq backup", "", {{"JSON files", {"json"}}, {"All files", {"*"}}}});
        if (!path) return nullptr;
        return toJson(importers::previewJsonBackup(*path, store));
    });

    bridge.handleAsync("importer:apply", [&store](const Json& a) {
        const Json& req = jsonArg(a, 0);
        if (!req.is_object()) throw std::invalid_argument("import request must be an object");
        return Json(importers::applyImport(store, importRequestFromJson(req)));
    });

    bridge.handleAsync("importer:exportPuttyRegFile", [&store](const Json& a) -> Json {
        auto sessions = sessionsByIds(store, argStrList(a, 0));
        if (sessions.empty()) throw std::invalid_argument("No sessions selected");
        auto path = ui::dialogs::saveFile({"Export sessions for PuTTY", "putty-sessions.reg", {{"Registry files", {"reg"}}}});
        if (!path) return nullptr;
        writeBytes(*path, importers::exportPuttyReg(sessions));
        return *path;
    });

    bridge.handleAsync("importer:exportJsonFile", [&store](const Json& a) -> Json {
        const auto ids = argStrList(a, 0);
        Json arr = Json::array();
        std::vector<std::string> folders;
        for (const auto& id : ids)
            if (auto j = store.getSessionJson(id)) {
                arr.push_back(*j);
                const std::string f = j->value("folder", std::string());
                if (!f.empty() && std::find(folders.begin(), folders.end(), f) == folders.end()) folders.push_back(f);
            }
        if (arr.empty()) throw std::invalid_argument("No sessions selected");
        auto path = ui::dialogs::saveFile({"Export sessions", "sshtermaq-sessions.json", {{"JSON files", {"json"}}}});
        if (!path) return nullptr;
        writeBytes(*path, importers::exportJsonBackup(arr, folders));
        return *path;
    });

    bridge.handleAsync("keys:generate", [](const Json& a) {
        const Json& req = jsonArg(a, 0);
        return Json(keys::generateKey(req.is_object() ? req.get<KeygenRequest>() : KeygenRequest{}));
    });
    bridge.handleAsync("keys:inspect", [](const Json& a) { return Json(keys::inspectKey(argStr(a, 0), argStr(a, 1))); });
    bridge.handleAsync("keys:convert", [](const Json& a) { return Json(keys::convertKey(argStr(a, 0), argStr(a, 1), argStr(a, 2))); });

    /* ---- Credentials: keys (store/Vault.h). Key text goes UI -> host only when the user pastes it; never back. ---- */
    bridge.handleSync("vault:list", [&store](const Json&) {
        Json out = Json::array();
        for (const auto& k : store.vault().list()) out.push_back(vaultJson(k));
        return out;
    });
    // What a pasted text is, before it is stored (type, fingerprint, passphrase protected): the Add dialog's preview.
    bridge.handleAsync("vault:inspectText", [](const Json& a) { return Json(keys::inspectKeyText(argStr(a, 0), "", "")); });
    bridge.handleAsync("vault:addKeyText", [&store](const Json& a) {
        std::string text = argStr(a, 1);
        const KeyInfo info = keys::inspectKeyText(text, "", "");
        return vaultJson(store.vault().addKeyText(vaultEntry(info, argStr(a, 0), "pasted key"), text));
    });
    // A linked key file: the path given, or one chosen in the Open dialog (null when cancelled).
    bridge.handleAsync("vault:addKeyFile", [&store](const Json& a) -> Json {
        auto path = pathOrDialog(a, 1, {"Choose a private key file", "", {{"Private keys", {"*"}}}});
        if (!path) return nullptr;
        const KeyInfo info = keys::inspectKey(*path, "");
        VaultKey entry = vaultEntry(info, argStr(a, 0), sys::fromPath(sys::toPath(*path).filename()));
        entry.path = *path;
        return vaultJson(store.vault().addKeyFile(entry));
    });
    bridge.handleSync("vault:rename", [&store](const Json& a) { return vaultJson(store.vault().rename(argStr(a, 0), argStr(a, 1))); });
    bridge.handleSync("vault:delete", [&store](const Json& a) {
        store.vault().remove(argStr(a, 0));
        return Json(nullptr);
    });
    // A remembered passphrase is set only at the passphrase prompt (SshConnector); the page can only forget it.
    bridge.handleSync("vault:forgetPassphrase", [&store](const Json& a) {
        store.vault().forgetPassphrase(argStr(a, 0));
        return Json(nullptr);
    });
    // Logins: the password goes UI -> host when the user types it (to store); it never comes back.
    bridge.handleSync("vault:listLogins", [&store](const Json&) { return Json(store.vault().listLogins()); });
    bridge.handleAsync("vault:addLogin", [&store](const Json& a) {
        const Json& j = jsonArg(a, 0);
        return Json(store.vault().addLogin(j.is_object() ? j.get<VaultLogin>() : VaultLogin{}, optionalPassword(a, 1)));
    });
    bridge.handleAsync("vault:updateLogin", [&store](const Json& a) {
        const Json& j = jsonArg(a, 0);
        return Json(store.vault().updateLogin(j.is_object() ? j.get<VaultLogin>() : VaultLogin{}, optionalPassword(a, 1)));
    });
    // The master password: typed in the UI, sent once, never kept there; Argon2 is slow by design (async handlers).
    bridge.handleSync("vault:status", [&store](const Json&) { return Json(store.vault().status()); });
    bridge.handleAsync("vault:setMaster", [&store](const Json& a) {
        store.vault().setMaster(argStr(a, 0));
        return Json(nullptr);
    });
    bridge.handleAsync("vault:changeMaster", [&store](const Json& a) {
        store.vault().changeMaster(argStr(a, 0), argStr(a, 1));
        return Json(nullptr);
    });
    bridge.handleAsync("vault:removeMaster", [&store](const Json& a) {
        store.vault().removeMaster(argStr(a, 0));
        return Json(nullptr);
    });
    bridge.handleAsync("vault:unlock", [&store](const Json& a) { return Json(store.vault().unlock(argStr(a, 0))); });
    bridge.handleSync("vault:lock", [&store](const Json&) {
        store.vault().lock();
        return Json(nullptr);
    });
    bridge.handleSync("vault:reset", [&store](const Json&) {
        store.vault().reset();
        return Json(nullptr);
    });
    bridge.handleSync("vault:deleteLogin", [&store](const Json& a) {
        store.vault().removeLogin(argStr(a, 0));
        return Json(nullptr);
    });
    // A stored key written to a file the user chooses, readable by its owner only. Returns the path, null when cancelled.
    bridge.handleAsync("vault:exportKey", [&store](const Json& a) -> Json {
        const auto entry = store.vault().get(argStr(a, 0));
        if (!entry) throw std::runtime_error("Key not found");
        if (entry->kind != "stored") throw std::runtime_error("This key is a linked file: it is already a file");
        auto text = store.vault().storedKeyText(entry->id);
        if (!text) throw std::runtime_error("Cannot decrypt this key on this computer");
        std::string name = entry->name;
        for (char& c : name)
            if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos) c = '_';
        const bool ppk = entry->format == "ppk2" || entry->format == "ppk3";
        auto path = ui::dialogs::saveFile({"Export the key \"" + entry->name + "\"", name + (ppk ? ".ppk" : ""), {{"All files", {"*"}}}});
        if (!path) return nullptr;
        sys::writeFileAtomic(sys::toPath(*path), *text);
        std::error_code ec;
        fs::permissions(sys::toPath(*path), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        return Json(*path);
    });

    // Export / import of Credentials: one file sealed with a password typed for it (the payload never reaches the UI).
    bridge.handleAsync("vault:exportFile", [&store](const Json& a) -> Json {
        const auto data = store.vault().exportData(argStr(a, 0), a.size() > 1 && a[1].is_boolean() && a[1].get<bool>());
        std::optional<std::string> path;
        if (const std::string given = argStr(a, 2); !given.empty())
            path = given;
        else
            path = ui::dialogs::saveFile({"Export Credentials", "credentials-" + todayIso() + ".sshtcred",
                                          {{"Credentials", {"*.sshtcred"}}, {"All files", {"*"}}}});
        if (!path) return nullptr;
        sys::writeFileAtomic(sys::toPath(*path), data.text);
        std::error_code ec;
        fs::permissions(sys::toPath(*path), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        return Json{{"path", *path}, {"keys", data.keys}, {"logins", data.logins}, {"secrets", data.secrets}, {"skipped", data.skipped}};
    });
    bridge.handleAsync("vault:importFile", [&store](const Json& a) -> Json {
        auto path = pathOrDialog(a, 1, {"Import Credentials", "", {{"Credentials", {"*.sshtcred"}}, {"All files", {"*"}}}});
        if (!path) return nullptr;
        auto text = sys::readFile(sys::toPath(*path));
        if (!text) throw std::runtime_error("Cannot read " + *path);
        const auto r = store.vault().importData(*text, argStr(a, 0));
        return Json{{"path", *path},
                    {"keysAdded", r.keysAdded},
                    {"keysSkipped", r.keysSkipped},
                    {"loginsAdded", r.loginsAdded},
                    {"loginsSkipped", r.loginsSkipped},
                    {"secrets", r.secrets},
                    {"renamed", r.renamed}};
    });

    bridge.handleAsync("knownHosts:importOpenSsh", [&store](const Json& a) -> Json {
        auto path = pathOrDialog(a, 0, {"Import OpenSSH known_hosts", defaultKnownHostsPath(), {{"All files", {"*"}}}});
        if (!path) return 0;
        std::ifstream f(sys::toPath(*path), std::ios::binary);
        if (!f) throw std::runtime_error("Cannot read " + *path);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        int skipped = 0;
        auto hosts = importers::parseOpenSshKnownHosts(content, skipped);
        return store.knownHosts().addMany(hosts);
    });
}

}  // namespace st
