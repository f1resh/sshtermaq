#include "store/Vault.h"

#include "core/Crypto.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "core/Uuid.h"
#include "store/JsonFile.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <stdexcept>
#include <thread>

namespace st {

namespace {

bool sameName(const std::string& a, const std::string& b) { return toLower(trim(a)) == toLower(trim(b)); }

constexpr std::string_view kSealed = "mp1:";
constexpr const char* kFileFormat = "SSHTermaq credentials";
constexpr const char* kFileAad = "SSHTermaq credentials v1";
/** Bounds for the KDF cost a file may ask for (a crafted file must not make us allocate gigabytes). */
constexpr uint32_t kMaxFileMemoryKiB = 1u << 18;  // 256 MiB
constexpr uint32_t kMaxFilePasses = 16;

/** `name`, or "name (2)", "name (3)"... the first one `taken` says is free. */
template <class Taken>
std::string freeName(const std::string& name, Taken taken) {
    if (!taken(name)) return name;
    for (int i = 2;; ++i) {
        std::string n = name + " (" + std::to_string(i) + ")";
        if (!taken(n)) return n;
    }
}
constexpr const char* kCheckText = "SSHTermaq Credentials master password check";
uint32_t gKdfMemoryKiB = 65536;  // 64 MiB
uint32_t gKdfPasses = 3;

bool isSealed(const std::string& v) { return v.compare(0, kSealed.size(), kSealed) == 0; }

/** The master record: {"v":1,"kdf":"argon2id","m":KiB,"t":passes,"p":lanes,"salt":b64,"check":b64}. */
struct MasterRecord {
    uint32_t m = 0, t = 0, p = 1;
    std::string salt, check;
};

std::optional<MasterRecord> parseRecord(const std::optional<std::string>& text) {
    if (!text) return std::nullopt;
    try {
        const Json j = Json::parse(*text);
        MasterRecord r;
        r.m = j.at("m").get<uint32_t>();
        r.t = j.at("t").get<uint32_t>();
        r.p = j.value("p", 1u);
        if (!crypto::base64Decode(j.at("salt").get<std::string>(), r.salt) ||
            !crypto::base64Decode(j.at("check").get<std::string>(), r.check))
            return std::nullopt;
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string deriveKey(const std::string& password, const MasterRecord& r) {
    std::string key, error;
    if (!crypto::argon2("ARGON2ID", password, r.salt, r.m, r.t, r.p, 32, key, error)) throw std::runtime_error(error);
    return key;
}

}  // namespace

Vault::Vault(std::filesystem::path file, Credentials& credentials) : file_(std::move(file)), credentials_(credentials) {}

void Vault::load() {
    Json root;
    std::vector<VaultKey> keys;
    std::vector<VaultLogin> logins;
    if (storeio::loadJson(file_, root) == storeio::LoadStatus::Ok) {
        if (root.is_object() && root.contains("keys") && root["keys"].is_array()) {
            size_t skipped = 0;
            for (const auto& item : root["keys"]) {
                try {
                    VaultKey k = item.get<VaultKey>();
                    if (k.id.empty() || trim(k.name).empty() || (k.kind != "file" && k.kind != "stored") ||
                        (k.kind == "file" && k.path.empty())) {
                        ++skipped;
                        continue;
                    }
                    keys.push_back(std::move(k));
                } catch (const std::exception&) {
                    ++skipped;
                }
            }
            if (root.contains("logins") && root["logins"].is_array()) {
                for (const auto& item : root["logins"]) {
                    try {
                        VaultLogin l = item.get<VaultLogin>();
                        if (l.id.empty() || trim(l.name).empty()) {
                            ++skipped;
                            continue;
                        }
                        logins.push_back(std::move(l));
                    } catch (const std::exception&) {
                        ++skipped;
                    }
                }
            }
            if (skipped) applog::warn("vault.json: skipped " + std::to_string(skipped) + " invalid entries");
        } else {
            storeio::backupCorrupt(file_, "unexpected document shape");
        }
    }
    std::lock_guard lk(mutex_);
    keys_ = std::move(keys);
    logins_ = std::move(logins);
}

std::vector<VaultKey> Vault::list() const {
    std::lock_guard lk(mutex_);
    std::vector<VaultKey> out = keys_;
    for (auto& k : out) k.passphraseRemembered = credentials_.has(passphraseSecretKey(k.id));
    std::sort(out.begin(), out.end(), [](const VaultKey& a, const VaultKey& b) { return toLower(a.name) < toLower(b.name); });
    return out;
}

std::optional<VaultKey> Vault::get(const std::string& id) const {
    std::lock_guard lk(mutex_);
    for (auto k : keys_) {
        if (k.id != id) continue;
        k.passphraseRemembered = credentials_.has(passphraseSecretKey(k.id));
        return k;
    }
    return std::nullopt;
}

bool Vault::nameTakenLocked(const std::string& name, const std::string& exceptId) const {
    return std::any_of(keys_.begin(), keys_.end(), [&](const VaultKey& k) { return k.id != exceptId && sameName(k.name, name); });
}

VaultKey Vault::add(VaultKey entry, const std::string& secretText) {
    entry.name = trim(entry.name);
    if (entry.name.empty()) throw std::runtime_error("The key needs a name");
    entry.id = newUuid();
    entry.createdAt = entry.updatedAt = nowMs();
    {
        std::lock_guard lk(mutex_);
        if (nameTakenLocked(entry.name, "")) throw std::runtime_error("A key named \"" + entry.name + "\" already exists");
        // The secret first: an entry must never point at a secret that was not written.
        if (entry.kind == "stored") putSecret(secretKey(entry.id), secretText);
        keys_.push_back(entry);
        try {
            saveLocked();
        } catch (...) {
            keys_.pop_back();
            if (entry.kind == "stored") credentials_.remove(secretKey(entry.id));
            throw;
        }
    }
    applog::info("credentials: added " + entry.kind + " key \"" + entry.name + "\" (" + entry.keyType + ")");
    notify();
    return entry;
}

VaultKey Vault::addKeyFile(VaultKey entry) {
    entry.kind = "file";
    if (trim(entry.path).empty()) throw std::runtime_error("No key file given");
    return add(std::move(entry), "");
}

VaultKey Vault::addKeyText(VaultKey entry, const std::string& text) {
    entry.kind = "stored";
    entry.path.clear();
    if (trim(text).empty()) throw std::runtime_error("No key text given");
    return add(std::move(entry), text);
}

VaultKey Vault::rename(const std::string& id, const std::string& name) {
    const std::string n = trim(name);
    if (n.empty()) throw std::runtime_error("The key needs a name");
    VaultKey out;
    {
        std::lock_guard lk(mutex_);
        auto it = std::find_if(keys_.begin(), keys_.end(), [&](const VaultKey& k) { return k.id == id; });
        if (it == keys_.end()) throw std::runtime_error("Key not found");
        if (nameTakenLocked(n, id)) throw std::runtime_error("A key named \"" + n + "\" already exists");
        it->name = n;
        it->updatedAt = nowMs();
        out = *it;
        out.passphraseRemembered = credentials_.has(passphraseSecretKey(id));
        saveLocked();
    }
    notify();
    return out;
}

void Vault::remove(const std::string& id) {
    bool removed = false;
    std::string name, kind;
    {
        std::lock_guard lk(mutex_);
        auto it = std::find_if(keys_.begin(), keys_.end(), [&](const VaultKey& k) { return k.id == id; });
        if (it == keys_.end()) return;
        name = it->name;
        kind = it->kind;
        keys_.erase(it);
        saveLocked();
        removed = true;
    }
    if (kind == "stored") credentials_.remove(secretKey(id));
    credentials_.remove(passphraseSecretKey(id));
    if (removed) {
        applog::info("credentials: removed key \"" + name + "\"");
        notify();
    }
}

std::optional<std::string> Vault::storedKeyText(const std::string& id) const {
    {
        std::lock_guard lk(mutex_);
        auto it = std::find_if(keys_.begin(), keys_.end(), [&](const VaultKey& k) { return k.id == id; });
        if (it == keys_.end() || it->kind != "stored") return std::nullopt;
    }
    return readSecret(secretKey(id));
}

/* ---- remembered passphrases ---- */

std::optional<std::string> Vault::passphrase(const std::string& id) const {
    if (!get(id)) return std::nullopt;
    return readSecret(passphraseSecretKey(id));
}

void Vault::rememberPassphrase(const std::string& id, const std::string& passphrase) {
    const auto k = get(id);
    if (!k) throw std::runtime_error("Key not found");
    putSecret(passphraseSecretKey(id), passphrase);
    applog::info("credentials: remembered the passphrase of key \"" + k->name + "\"");
    notify();
}

void Vault::forgetPassphrase(const std::string& id) {
    if (!credentials_.has(passphraseSecretKey(id))) return;
    credentials_.remove(passphraseSecretKey(id));
    const auto k = get(id);
    applog::info("credentials: forgot the passphrase of key \"" + (k ? k->name : id) + "\"");
    notify();
}

/* ---- logins ---- */

std::vector<VaultLogin> Vault::listLogins() const {
    std::lock_guard lk(mutex_);
    std::vector<VaultLogin> out = logins_;
    std::sort(out.begin(), out.end(), [](const VaultLogin& a, const VaultLogin& b) { return toLower(a.name) < toLower(b.name); });
    return out;
}

std::optional<VaultLogin> Vault::getLogin(const std::string& id) const {
    std::lock_guard lk(mutex_);
    for (const auto& l : logins_)
        if (l.id == id) return l;
    return std::nullopt;
}

bool Vault::loginNameTakenLocked(const std::string& name, const std::string& exceptId) const {
    return std::any_of(logins_.begin(), logins_.end(), [&](const VaultLogin& l) { return l.id != exceptId && sameName(l.name, name); });
}

void Vault::checkLogin(VaultLogin& e) {
    e.name = trim(e.name);
    e.username = trim(e.username);
    if (e.name.empty()) throw std::runtime_error("The login needs a name");
    if (e.username.empty()) throw std::runtime_error("The login needs a user name");
    if (e.password != "stored" && e.password != "ask" && e.password != "none") e.password = "ask";
}

VaultLogin Vault::addLogin(VaultLogin entry, const std::optional<std::string>& password) {
    checkLogin(entry);
    if (entry.password == "stored" && (!password || password->empty())) throw std::runtime_error("Enter the password to store");
    entry.id = newUuid();
    entry.createdAt = entry.updatedAt = nowMs();
    {
        std::lock_guard lk(mutex_);
        if (loginNameTakenLocked(entry.name, "")) throw std::runtime_error("A login named \"" + entry.name + "\" already exists");
        if (entry.password == "stored") putSecret(loginSecretKey(entry.id), *password);
        logins_.push_back(entry);
        try {
            saveLocked();
        } catch (...) {
            logins_.pop_back();
            credentials_.remove(loginSecretKey(entry.id));
            throw;
        }
    }
    applog::info("credentials: added login \"" + entry.name + "\"");
    notify();
    return entry;
}

VaultLogin Vault::updateLogin(VaultLogin entry, const std::optional<std::string>& password) {
    checkLogin(entry);
    VaultLogin out;
    {
        std::lock_guard lk(mutex_);
        auto it = std::find_if(logins_.begin(), logins_.end(), [&](const VaultLogin& l) { return l.id == entry.id; });
        if (it == logins_.end()) throw std::runtime_error("Login not found");
        if (loginNameTakenLocked(entry.name, entry.id)) throw std::runtime_error("A login named \"" + entry.name + "\" already exists");
        if (entry.password == "stored") {
            if (password && !password->empty())
                putSecret(loginSecretKey(entry.id), *password);
            else if (!credentials_.has(loginSecretKey(entry.id)))
                throw std::runtime_error("Enter the password to store");
        } else {
            credentials_.remove(loginSecretKey(entry.id));
        }
        entry.createdAt = it->createdAt;
        entry.updatedAt = nowMs();
        *it = entry;
        out = *it;
        saveLocked();
    }
    notify();
    return out;
}

void Vault::removeLogin(const std::string& id) {
    std::string name;
    {
        std::lock_guard lk(mutex_);
        auto it = std::find_if(logins_.begin(), logins_.end(), [&](const VaultLogin& l) { return l.id == id; });
        if (it == logins_.end()) return;
        name = it->name;
        logins_.erase(it);
        saveLocked();
    }
    credentials_.remove(loginSecretKey(id));
    applog::info("credentials: removed login \"" + name + "\"");
    notify();
}

std::optional<std::string> Vault::loginPassword(const std::string& id) const {
    {
        std::lock_guard lk(mutex_);
        auto it = std::find_if(logins_.begin(), logins_.end(), [&](const VaultLogin& l) { return l.id == id; });
        if (it == logins_.end() || it->password != "stored") return std::nullopt;
    }
    return readSecret(loginSecretKey(id));
}

void Vault::saveLocked() const {
    Json root{{"version", 1}, {"keys", Json::array()}, {"logins", Json::array()}};
    for (const auto& k : keys_) {
        Json j = k;
        j.erase("passphraseRemembered");  // a fact of credentials.json, not of this file
        root["keys"].push_back(std::move(j));
    }
    for (const auto& l : logins_) root["logins"].push_back(l);
    storeio::saveJson(file_, root);
}

void Vault::notify() {
    if (auto cb = onChanged) cb();
}

/* ---- export / import ---- */

Vault::ExportResult Vault::exportData(const std::string& password, bool includeSecrets) const {
    if (password.size() < 8) throw std::runtime_error("Use at least 8 characters for the file's password");
    ExportResult out;
    Json payload{{"exportedAt", nowMs()}, {"keys", Json::array()}, {"logins", Json::array()}};
    std::vector<std::string> wipeLater;
    for (const auto& k : list()) {
        Json j = k;
        j.erase("passphraseRemembered");
        if (k.kind == "stored") {
            auto text = readSecret(secretKey(k.id));  // VaultLocked propagates: unlock first
            if (!text) {
                out.skipped.push_back(k.name);
                continue;
            }
            j["text"] = *text;
            crypto::cleanse(*text);
            ++out.secrets;
        }
        if (includeSecrets) {
            if (auto pp = readSecret(passphraseSecretKey(k.id))) {
                j["passphrase"] = *pp;
                crypto::cleanse(*pp);
                ++out.secrets;
            }
        }
        payload["keys"].push_back(std::move(j));
        ++out.keys;
    }
    for (const auto& l : listLogins()) {
        Json j = l;
        if (includeSecrets && l.password == "stored") {
            if (auto pw = readSecret(loginSecretKey(l.id))) {
                j["storedPassword"] = *pw;
                crypto::cleanse(*pw);
                ++out.secrets;
            }
        }
        payload["logins"].push_back(std::move(j));
        ++out.logins;
    }
    const std::string salt = crypto::randomBytes(16);
    std::string key, error;
    if (!crypto::argon2("ARGON2ID", password, salt, gKdfMemoryKiB, gKdfPasses, 1, 32, key, error)) throw std::runtime_error(error);
    std::string plain = payload.dump();
    const std::string sealed = crypto::aesGcmSeal(key, plain, kFileAad);
    crypto::cleanse(plain);
    crypto::cleanse(key);
    const Json file{{"format", kFileFormat},
                    {"version", 1},
                    {"kdf", {{"name", "argon2id"}, {"m", gKdfMemoryKiB}, {"t", gKdfPasses}, {"p", 1}, {"salt", crypto::base64Encode(salt)}}},
                    {"cipher", "aes-256-gcm"},
                    {"data", crypto::base64Encode(sealed)}};
    out.text = file.dump(2) + "\n";
    applog::info("credentials: exported " + std::to_string(out.keys) + " keys, " + std::to_string(out.logins) + " logins, " +
                 std::to_string(out.secrets) + " secrets");
    return out;
}

Vault::ImportResult Vault::importData(const std::string& text, const std::string& password) {
    // The file: format, KDF parameters (bounded), the sealed payload.
    Json file;
    try {
        file = Json::parse(text);
    } catch (const std::exception&) {
        throw std::runtime_error("Not a Credentials file");
    }
    if (!file.is_object() || file.value("format", "") != kFileFormat) throw std::runtime_error("Not a Credentials file");
    if (file.value("version", 0) != 1) throw std::runtime_error("This Credentials file is from a newer version of the app");
    std::string salt, sealed;
    uint32_t m = 0, t = 0, lanes = 1;
    try {
        const Json& kdf = file.at("kdf");
        if (kdf.value("name", "") != "argon2id") throw std::runtime_error("kdf");
        m = kdf.at("m").get<uint32_t>();
        t = kdf.at("t").get<uint32_t>();
        lanes = kdf.value("p", 1u);
        if (!crypto::base64Decode(kdf.at("salt").get<std::string>(), salt) ||
            !crypto::base64Decode(file.at("data").get<std::string>(), sealed))
            throw std::runtime_error("b64");
    } catch (const std::exception&) {
        throw std::runtime_error("The Credentials file is damaged");
    }
    if (m == 0 || m > kMaxFileMemoryKiB || t == 0 || t > kMaxFilePasses || lanes == 0 || lanes > 8)
        throw std::runtime_error("The Credentials file asks for an unsupported key derivation");
    std::string key, error;
    if (!crypto::argon2("ARGON2ID", password, salt, m, t, lanes, 32, key, error)) throw std::runtime_error(error);
    auto plain = crypto::aesGcmOpen(key, sealed, kFileAad);
    crypto::cleanse(key);
    if (!plain) throw std::runtime_error("Wrong password, or the file is damaged");
    Json payload;
    try {
        payload = Json::parse(*plain);
    } catch (const std::exception&) {
        crypto::cleanse(*plain);
        throw std::runtime_error("The Credentials file is damaged");
    }
    crypto::cleanse(*plain);

    ImportResult r;
    std::vector<std::pair<std::string, std::optional<std::string>>> secrets;
    std::vector<VaultKey> newKeys;
    std::vector<VaultLogin> newLogins;
    {
        std::lock_guard lk(mutex_);
        std::map<std::string, std::string> keyIds;  // id in the file -> id here
        auto keyTaken = [&](const std::string& n) {
            return nameTakenLocked(n, "") || std::any_of(newKeys.begin(), newKeys.end(), [&](const VaultKey& k) { return sameName(k.name, n); });
        };
        for (const auto& item : payload.value("keys", Json::array())) {
            VaultKey k;
            try {
                k = item.get<VaultKey>();
            } catch (const std::exception&) {
                continue;
            }
            k.passphraseRemembered = false;
            if (k.id.empty() || trim(k.name).empty() || (k.kind != "file" && k.kind != "stored") || (k.kind == "file" && k.path.empty()))
                continue;
            const std::string keyText = item.value("text", "");
            if (k.kind == "stored" && keyText.empty()) continue;
            auto same = std::find_if(keys_.begin(), keys_.end(), [&](const VaultKey& x) {
                return x.id == k.id || (!k.fingerprint.empty() && x.fingerprint == k.fingerprint && x.kind == k.kind &&
                                        (x.kind == "stored" || x.path == k.path));
            });
            if (same != keys_.end()) {
                keyIds[k.id] = same->id;
                ++r.keysSkipped;
                continue;
            }
            const std::string name = freeName(trim(k.name), keyTaken);
            if (name != trim(k.name)) r.renamed.push_back("key " + trim(k.name) + " → " + name);
            k.name = name;
            keyIds[k.id] = k.id;
            if (k.kind == "stored") secrets.emplace_back(secretKey(k.id), prepareSecret(secretKey(k.id), keyText));
            if (const std::string pp = item.value("passphrase", ""); !pp.empty())
                secrets.emplace_back(passphraseSecretKey(k.id), prepareSecret(passphraseSecretKey(k.id), pp));
            newKeys.push_back(std::move(k));
            ++r.keysAdded;
        }
        auto loginTaken = [&](const std::string& n) {
            return loginNameTakenLocked(n, "") ||
                   std::any_of(newLogins.begin(), newLogins.end(), [&](const VaultLogin& l) { return sameName(l.name, n); });
        };
        for (const auto& item : payload.value("logins", Json::array())) {
            VaultLogin l;
            try {
                l = item.get<VaultLogin>();
                checkLogin(l);
            } catch (const std::exception&) {
                continue;
            }
            if (l.id.empty()) continue;
            if (std::any_of(logins_.begin(), logins_.end(), [&](const VaultLogin& x) { return x.id == l.id; })) {
                ++r.loginsSkipped;
                continue;
            }
            if (auto it = keyIds.find(l.keyId); it != keyIds.end()) l.keyId = it->second;
            const std::string name = freeName(l.name, loginTaken);
            if (name != l.name) r.renamed.push_back("login " + l.name + " → " + name);
            l.name = name;
            const std::string pw = item.value("storedPassword", "");
            if (l.password == "stored") {
                if (pw.empty())
                    l.password = "ask";  // the file has no password for it
                else
                    secrets.emplace_back(loginSecretKey(l.id), prepareSecret(loginSecretKey(l.id), pw));
            }
            newLogins.push_back(std::move(l));
            ++r.loginsAdded;
        }
        r.secrets = static_cast<int>(secrets.size());
        // The secrets first (one atomic write): an entry never points at a secret that was not written.
        if (!secrets.empty()) credentials_.apply(secrets);
        const size_t keysBefore = keys_.size(), loginsBefore = logins_.size();
        keys_.insert(keys_.end(), newKeys.begin(), newKeys.end());
        logins_.insert(logins_.end(), newLogins.begin(), newLogins.end());
        try {
            saveLocked();
        } catch (...) {
            keys_.resize(keysBefore);
            logins_.resize(loginsBefore);
            std::vector<std::pair<std::string, std::optional<std::string>>> undo;
            for (const auto& [name, value] : secrets) undo.emplace_back(name, std::nullopt);
            try {
                credentials_.apply(undo);
            } catch (...) {
            }
            throw;
        }
    }
    for (auto& [name, value] : secrets)
        if (value) crypto::cleanse(*value);
    applog::info("credentials: imported " + std::to_string(r.keysAdded) + " keys, " + std::to_string(r.loginsAdded) + " logins (" +
                 std::to_string(r.keysSkipped + r.loginsSkipped) + " already here)");
    if (r.keysAdded || r.loginsAdded) notify();
    return r;
}

/* ---- the master password ---- */

void Vault::setKdfCost(uint32_t memoryKiB, uint32_t passes) {
    gKdfMemoryKiB = memoryKiB;
    gKdfPasses = passes;
}

bool Vault::hasMaster() const { return credentials_.has(kMasterRecord); }

bool Vault::locked() const {
    if (!hasMaster()) return false;
    std::lock_guard lk(masterMutex_);
    return masterKey_.empty();
}

std::string Vault::status() const { return !hasMaster() ? "none" : locked() ? "locked" : "unlocked"; }

bool Vault::needsUnlock(const std::string& name) const { return locked() && credentials_.has(name); }

std::string Vault::seal(const std::string& key, const std::string& name, const std::string& value) const {
    return std::string(kSealed) + crypto::base64Encode(crypto::aesGcmSeal(key, value, name));
}

std::optional<std::string> Vault::open(const std::string& key, const std::string& name, const std::string& value) const {
    std::string sealed;
    if (!crypto::base64Decode(std::string_view(value).substr(kSealed.size()), sealed)) return std::nullopt;
    return crypto::aesGcmOpen(key, sealed, name);
}

std::string Vault::prepareSecret(const std::string& name, const std::string& value) const {
    if (!hasMaster()) return value;
    std::string key;
    {
        std::lock_guard lk(masterMutex_);
        if (masterKey_.empty()) throw VaultLocked();
        key = masterKey_;
    }
    std::string sealed = seal(key, name, value);
    crypto::cleanse(key);
    return sealed;
}

void Vault::putSecret(const std::string& name, const std::string& value) { credentials_.set(name, prepareSecret(name, value)); }

std::optional<std::string> Vault::readSecret(const std::string& name) const {
    auto raw = credentials_.get(name);
    // Without a master password everything is plain; with one, a value written before it was set (an interrupted
    // setMaster) is still plain and readable.
    if (!raw || !hasMaster() || !isSealed(*raw)) return raw;
    std::string key;
    {
        std::lock_guard lk(masterMutex_);
        if (masterKey_.empty()) throw VaultLocked();
        key = masterKey_;
    }
    auto plain = open(key, name, *raw);
    crypto::cleanse(key);
    crypto::cleanse(*raw);
    if (!plain) applog::warn("credentials: a secret does not open with the master key (corrupt entry)");
    return plain;
}

std::vector<std::string> Vault::secretNames() const {
    std::vector<std::string> out;
    for (auto& n : credentials_.keysWithPrefix("vault:"))
        if (n != kMasterRecord) out.push_back(std::move(n));
    return out;
}

std::optional<std::string> Vault::deriveChecked(const std::string& password) const {
    const auto rec = parseRecord(credentials_.get(kMasterRecord));
    if (!rec) throw std::runtime_error("The master password record cannot be read on this computer");
    std::string key = deriveKey(password, *rec);
    if (!crypto::aesGcmOpen(key, rec->check, kMasterRecord)) {
        crypto::cleanse(key);
        return std::nullopt;
    }
    return key;
}

void Vault::setMaster(const std::string& password) {
    if (hasMaster()) throw std::runtime_error("A master password is already set");
    if (password.size() < 8) throw std::runtime_error("Use at least 8 characters");
    MasterRecord r;
    r.m = gKdfMemoryKiB;
    r.t = gKdfPasses;
    r.salt = crypto::randomBytes(16);
    std::string key = deriveKey(password, r);
    const Json rec{{"v", 1},
                   {"kdf", "argon2id"},
                   {"m", r.m},
                   {"t", r.t},
                   {"p", r.p},
                   {"salt", crypto::base64Encode(r.salt)},
                   {"check", crypto::base64Encode(crypto::aesGcmSeal(key, kCheckText, kMasterRecord))}};
    std::vector<std::pair<std::string, std::optional<std::string>>> changes{{kMasterRecord, rec.dump()}};
    size_t sealedCount = 0;
    for (const auto& name : secretNames()) {
        auto plain = credentials_.get(name);
        if (!plain) continue;  // cannot be read on this account: left as it is
        changes.emplace_back(name, seal(key, name, *plain));
        crypto::cleanse(*plain);
        ++sealedCount;
    }
    credentials_.apply(changes);
    {
        std::lock_guard lk(masterMutex_);
        crypto::cleanse(masterKey_);
        masterKey_ = std::move(key);
        failures_ = 0;
    }
    applog::info("credentials: master password set (" + std::to_string(sealedCount) + " secrets sealed)");
    notify();
}

bool Vault::unlock(const std::string& password) {
    if (!hasMaster()) throw std::runtime_error("No master password is set");
    auto key = deriveChecked(password);
    if (!key) {
        int failures;
        {
            std::lock_guard lk(masterMutex_);
            failures = ++failures_;
        }
        applog::warn("credentials: wrong master password");
        // A short wait that grows with each wrong try (250 ms, 500 ms, 1 s ... at most 4 s); the data is never locked out.
        std::this_thread::sleep_for(std::chrono::milliseconds(250 << std::min(failures - 1, 4)));
        return false;
    }
    {
        std::lock_guard lk(masterMutex_);
        crypto::cleanse(masterKey_);
        masterKey_ = std::move(*key);
        failures_ = 0;
    }
    applog::info("credentials: unlocked");
    notify();
    return true;
}

void Vault::lock() {
    {
        std::lock_guard lk(masterMutex_);
        if (masterKey_.empty()) return;
        crypto::cleanse(masterKey_);
    }
    applog::info("credentials: locked");
    notify();
}

void Vault::changeMaster(const std::string& oldPassword, const std::string& newPassword) {
    if (!hasMaster()) throw std::runtime_error("No master password is set");
    if (newPassword.size() < 8) throw std::runtime_error("Use at least 8 characters");
    auto oldKey = deriveChecked(oldPassword);
    if (!oldKey) throw std::runtime_error("The current master password is wrong");
    MasterRecord r;
    r.m = gKdfMemoryKiB;
    r.t = gKdfPasses;
    r.salt = crypto::randomBytes(16);
    std::string key = deriveKey(newPassword, r);
    const Json rec{{"v", 1},
                   {"kdf", "argon2id"},
                   {"m", r.m},
                   {"t", r.t},
                   {"p", r.p},
                   {"salt", crypto::base64Encode(r.salt)},
                   {"check", crypto::base64Encode(crypto::aesGcmSeal(key, kCheckText, kMasterRecord))}};
    std::vector<std::pair<std::string, std::optional<std::string>>> changes{{kMasterRecord, rec.dump()}};
    for (const auto& name : secretNames()) {
        auto raw = credentials_.get(name);
        if (!raw) continue;
        auto plain = isSealed(*raw) ? open(*oldKey, name, *raw) : raw;
        if (!plain) continue;  // corrupt: left as it is
        changes.emplace_back(name, seal(key, name, *plain));
        crypto::cleanse(*plain);
    }
    crypto::cleanse(*oldKey);
    credentials_.apply(changes);
    {
        std::lock_guard lk(masterMutex_);
        crypto::cleanse(masterKey_);
        masterKey_ = std::move(key);
        failures_ = 0;
    }
    applog::info("credentials: master password changed");
    notify();
}

void Vault::removeMaster(const std::string& password) {
    if (!hasMaster()) throw std::runtime_error("No master password is set");
    auto key = deriveChecked(password);
    if (!key) throw std::runtime_error("The master password is wrong");
    std::vector<std::pair<std::string, std::optional<std::string>>> changes{{kMasterRecord, std::nullopt}};
    for (const auto& name : secretNames()) {
        auto raw = credentials_.get(name);
        if (!raw || !isSealed(*raw)) continue;
        auto plain = open(*key, name, *raw);
        if (!plain) continue;
        changes.emplace_back(name, std::move(*plain));
    }
    crypto::cleanse(*key);
    credentials_.apply(changes);
    for (auto& [name, value] : changes)
        if (value) crypto::cleanse(*value);
    {
        std::lock_guard lk(masterMutex_);
        crypto::cleanse(masterKey_);
        failures_ = 0;
    }
    applog::info("credentials: master password removed");
    notify();
}

void Vault::reset() {
    std::vector<std::pair<std::string, std::optional<std::string>>> changes{{kMasterRecord, std::nullopt}};
    for (const auto& name : secretNames()) changes.emplace_back(name, std::nullopt);
    credentials_.apply(changes);  // the secrets first: an entry never points at a secret that is half gone
    size_t keysRemoved = 0;
    {
        std::lock_guard lk(mutex_);
        keysRemoved = static_cast<size_t>(std::count_if(keys_.begin(), keys_.end(), [](const VaultKey& k) { return k.kind == "stored"; }));
        keys_.erase(std::remove_if(keys_.begin(), keys_.end(), [](const VaultKey& k) { return k.kind == "stored"; }), keys_.end());
        for (auto& l : logins_)
            if (l.password == "stored") l.password = "ask";
        saveLocked();
    }
    {
        std::lock_guard lk(masterMutex_);
        crypto::cleanse(masterKey_);
        failures_ = 0;
    }
    applog::warn("credentials: reset: every stored secret deleted (" + std::to_string(keysRemoved) + " stored keys)");
    notify();
}

}  // namespace st
