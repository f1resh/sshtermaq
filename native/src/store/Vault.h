#pragma once
// Credentials (the user-facing name; "vault" in code): keys a session can use instead of its own key file.
// Persisted as vault.json: { "version": 1, "keys": [VaultKey...], "logins": [VaultLogin...] } (the public part only).
//   - A "file" key is a link: only the path (and the key's public details, for the list) is stored; the file is read
//     at each connection and never copied, moved or deleted.
//   - A "stored" key's text (as given: format and passphrase protection kept) is encrypted in credentials.json under
//     "vault:key:<id>" (Credentials: DPAPI / Keychain).
//   - A key's remembered passphrase (either kind) is encrypted there too, as "vault:key:<id>:passphrase"; a login's
//     stored password as "vault:login:<id>:password".
//   - Optional master password: then each of those secrets is first sealed with a key derived from it (Argon2id ->
//     AES-256-GCM, the secret's name as associated data; the value becomes "mp1:<base64>") and then protected by the OS
//     as before. Its record (KDF parameters, salt, a check value) is the secret "vault:master". Locked after each start:
//     reading or writing a sealed secret throws VaultLocked until unlock(); the key lives in memory only until lock()
//     or quit. Setting, changing and removing it rewrite every secret in one atomic write of credentials.json.
// Thread-safe; writes are atomic; a corrupt file is backed up. Key material never goes into logs or messages.
#include "core/Types.h"
#include "store/Credentials.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace st {

/** A secret of Credentials is sealed with the master password and Credentials are locked. */
class VaultLocked : public std::runtime_error {
public:
    VaultLocked() : std::runtime_error("Credentials are locked: unlock them with the master password first") {}
};

class Vault {
public:
    Vault(std::filesystem::path file, Credentials& credentials);
    /** Loads vault.json (missing => empty; corrupt => backed up and empty; invalid entries skipped). */
    void load();

    std::vector<VaultKey> list() const;
    std::optional<VaultKey> get(const std::string& id) const;

    /**
     * Links a key file: `entry` holds the name, the path and the key's public details (inspected by the caller: the store
     * does not parse keys). Fills id, kind and the times. Throws std::runtime_error when the name is empty or taken.
     */
    VaultKey addKeyFile(VaultKey entry);
    /** Stores key text (pasted or generated), encrypted, with `entry`'s public details as for addKeyFile. */
    VaultKey addKeyText(VaultKey entry, const std::string& text);
    /** Throws when the id is unknown or the name is empty or taken. */
    VaultKey rename(const std::string& id, const std::string& name);
    /** Removes the entry and, for a stored key, its secret. Unknown ids are ignored. */
    void remove(const std::string& id);

    /** A stored key's text (decrypted); nullopt for a file key, an unknown id, or a secret this account cannot read. */
    std::optional<std::string> storedKeyText(const std::string& id) const;

    static std::string secretKey(const std::string& id) { return "vault:key:" + id; }

    /* ---- remembered passphrases (optional, per key; chosen at the passphrase prompt) ---- */
    /** The key's remembered passphrase; nullopt when none (or unknown id, or not readable on this account). */
    std::optional<std::string> passphrase(const std::string& id) const;
    /** Remembers the passphrase of key `id` (encrypted). Throws when the id is unknown. */
    void rememberPassphrase(const std::string& id, const std::string& passphrase);
    /** Forgets the key's passphrase (no-op when none). */
    void forgetPassphrase(const std::string& id);
    static std::string passphraseSecretKey(const std::string& id) { return "vault:key:" + id + ":passphrase"; }

    /* ---- logins: a user name and how to sign in (password stored / asked / none, a key, an agent) ---- */
    std::vector<VaultLogin> listLogins() const;
    std::optional<VaultLogin> getLogin(const std::string& id) const;
    /**
     * Adds a login. `password`: the password to store when `entry.password` is "stored" (required then). Fills id and
     * the times. Throws std::runtime_error when the name or user name is empty, the name is taken (among logins), or
     * a stored password is missing.
     */
    VaultLogin addLogin(VaultLogin entry, const std::optional<std::string>& password);
    /**
     * Updates the login with entry.id. `password`: a new password to store (nullopt or empty = keep the stored one).
     * Switching away from "stored" deletes the stored password.
     */
    VaultLogin updateLogin(VaultLogin entry, const std::optional<std::string>& password);
    /** Removes the login and its stored password. Unknown ids are ignored. */
    void removeLogin(const std::string& id);
    /** The login's stored password; nullopt unless it is "stored" and readable on this account. */
    std::optional<std::string> loginPassword(const std::string& id) const;
    static std::string loginSecretKey(const std::string& id) { return "vault:login:" + id + ":password"; }

    /* ---- the optional master password ---- */
    /** "none" (not set), "locked" or "unlocked". */
    std::string status() const;
    bool hasMaster() const;
    /** A master password is set and Credentials are locked. */
    bool locked() const;
    /** Reading the secret `name` needs unlock() first (it exists, and it is sealed while locked). */
    bool needsUnlock(const std::string& name) const;
    /** Sets the master password (none set yet; at least 8 characters) and seals every secret; leaves it unlocked. */
    void setMaster(const std::string& password);
    /** True when `password` is right: unlocked. A wrong one waits a little longer each time (no lockout) and is false. */
    bool unlock(const std::string& password);
    /** Forgets the master key (secrets stay sealed). */
    void lock();
    /** Re-seals every secret with a new master password. Throws when `oldPassword` is wrong. */
    void changeMaster(const std::string& oldPassword, const std::string& newPassword);
    /** Back to OS protection only. Throws when `password` is wrong. */
    void removeMaster(const std::string& password);
    /**
     * The master password is forgotten: deletes every secret of Credentials and the master record. Stored keys go
     * (their text was the secret); linked keys stay; logins stay, a stored password becomes "ask".
     */
    void reset();
    /** Connections that need to unlock take this first, so several of them ask once (see SshConnector). */
    std::timed_mutex& unlockPromptMutex() { return unlockPrompt_; }
    static constexpr const char* kMasterRecord = "vault:master";
    /** Argon2id cost for new master passwords (tests lower it). */
    static void setKdfCost(uint32_t memoryKiB, uint32_t passes);

    /* ---- export / import: one file, encrypted with a password chosen for it ---- */
    struct ExportResult {
        std::string text;  // the file's content
        int keys = 0, logins = 0, secrets = 0;
        /** Stored keys left out: their text cannot be read on this computer. */
        std::vector<std::string> skipped;
    };
    /**
     * The file for Credentials: every key (a stored key with its text, a linked key as its path) and login, and with
     * `includeSecrets` the logins' stored passwords and the keys' remembered passphrases. Sealed with `password` (at
     * least 8 characters; Argon2id -> AES-256-GCM). Throws VaultLocked when a needed secret is sealed and locked.
     */
    ExportResult exportData(const std::string& password, bool includeSecrets) const;

    struct ImportResult {
        int keysAdded = 0, keysSkipped = 0, loginsAdded = 0, loginsSkipped = 0, secrets = 0;
        /** "old → new" for items renamed because the name was taken. */
        std::vector<std::string> renamed;
    };
    /**
     * Adds what an exported file holds. Ids are kept (sessions that refer to them find them); a key or login already
     * here (same id; for keys also the same fingerprint) is skipped; a taken name gets " (2)"; a login whose stored
     * password is not in the file asks for it. All secrets in one atomic write. Throws when the password is wrong or
     * the file is not one of ours; VaultLocked when Credentials are locked.
     */
    ImportResult importData(const std::string& text, const std::string& password);

    /** Fired after any change (outside the lock, on the thread that made the change). */
    std::function<void()> onChanged;

private:
    VaultKey add(VaultKey entry, const std::string& secretText);
    void saveLocked() const;
    bool nameTakenLocked(const std::string& name, const std::string& exceptId) const;
    bool loginNameTakenLocked(const std::string& name, const std::string& exceptId) const;
    static void checkLogin(VaultLogin& entry);
    void notify();
    /** credentials.set of a secret, sealed when a master password is set (throws VaultLocked when locked). */
    void putSecret(const std::string& name, const std::string& value);
    /** The value as stored for `name`: sealed when a master password is set (throws VaultLocked when locked). */
    std::string prepareSecret(const std::string& name, const std::string& value) const;
    /** credentials.get of a secret, opened when sealed (throws VaultLocked when locked). */
    std::optional<std::string> readSecret(const std::string& name) const;
    std::string seal(const std::string& key, const std::string& name, const std::string& value) const;
    std::optional<std::string> open(const std::string& key, const std::string& name, const std::string& value) const;
    /** The master key for `password` from the stored record; nullopt when it is wrong. */
    std::optional<std::string> deriveChecked(const std::string& password) const;
    /** The names of the secrets of Credentials (without the master record). */
    std::vector<std::string> secretNames() const;

    std::filesystem::path file_;
    Credentials& credentials_;
    mutable std::mutex mutex_;
    std::vector<VaultKey> keys_;
    std::vector<VaultLogin> logins_;
    mutable std::mutex masterMutex_;  // masterKey_, failures_ (never taken before mutex_)
    std::string masterKey_;           // 32 bytes while unlocked
    int failures_ = 0;
    std::timed_mutex unlockPrompt_;
};

}  // namespace st
