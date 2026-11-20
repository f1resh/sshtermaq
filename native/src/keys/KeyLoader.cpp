#include "keys/KeyLoader.h"

#include "core/Crypto.h"
#include "keys/KeyFile.h"
#include "keys/SshWire.h"

namespace st {

namespace {
ssh_key loadFromText(const std::string& text, const std::string& path, const PassphraseCallback& askPassphrase,
                     std::string& error);
}  // namespace

ssh_key loadPrivateKey(const std::string& path, const PassphraseCallback& askPassphrase, std::string& error) {
    auto content = keys::readKeyFile(path);
    if (!content) {
        error = "Cannot read key file";
        return nullptr;
    }
    keys::ScopedWipe wc(*content);
    return loadFromText(*content, path, askPassphrase, error);
}

ssh_key loadPrivateKeyText(const std::string& content, const PassphraseCallback& askPassphrase, std::string& error) {
    return loadFromText(content, "", askPassphrase, error);
}

namespace {

/** `path`: the key file (for a "<path>.pub" sidecar of PPK files), or "" for key text. */
ssh_key loadFromText(const std::string& text, const std::string& path, const PassphraseCallback& askPassphrase,
                     std::string& error) {
    using namespace keys;
    const std::string* content = &text;
    const KeyFormat fmt = detectFormat(*content);

    if (fmt == KeyFormat::OpenSsh || fmt == KeyFormat::Pem) {
        // Let libssh load these directly (it understands every OpenSSH cipher/KDF and PEM flavour).
        if (fmt == KeyFormat::OpenSsh) {
            OpenSshContainer c;
            if (!parseOpenSshContainer(*content, c, error)) return nullptr;
            ScopedWipe wp(c.privateSection);
            // Give a precise message for key types libssh cannot use (ssh-dss, ssh-ed448).
            const std::string type = crypto::keyTypeOfBlob(c.publicBlob);
            if (!libsshSupportsKeyType(type)) {
                error = "Key type " + (type.empty() ? std::string("(unknown)") : type) + " is not supported";
                return nullptr;
            }
        }
        if (!isEncrypted(*content, fmt)) {
            SshKeyPtr key = importPrivateKeyText(*content, nullptr);
            if (!key) {
                error = "Invalid or unsupported private key";
                return nullptr;
            }
            return key.release();
        }
        for (int attempt = 1; attempt <= 3; ++attempt) {
            std::optional<std::string> pass = askPassphrase ? askPassphrase(attempt) : std::nullopt;
            if (!pass) {
                error = "Cancelled";
                return nullptr;
            }
            ScopedWipe wp(*pass);
            if (SshKeyPtr key = importPrivateKeyText(*content, pass->c_str())) return key.release();
        }
        error = "Wrong passphrase";
        return nullptr;
    }

    if (fmt == KeyFormat::Ppk2 || fmt == KeyFormat::Ppk3) {
        LoadedKey loaded;
        if (!loadKeyMaterial(*content, path, askPassphrase, 3, loaded, error)) return nullptr;
        SshKeyPtr key = sshKeyFromMaterial(loaded.material, error);
        return key.release();
    }

    error = "Unsupported key format";
    return nullptr;
}

}  // namespace

std::string detectKeyFormat(const std::string& fileContent) { return keys::formatName(keys::detectFormat(fileContent)); }

}  // namespace st
