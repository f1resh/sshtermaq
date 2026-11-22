#include "core/Crypto.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <cctype>
#include <memory>
#include <stdexcept>

namespace st::crypto {

void cleanse(std::string& s) {
    if (!s.empty()) OPENSSL_cleanse(s.data(), s.size());
    s.clear();
}

namespace {
struct KdfDeleter {
    void operator()(EVP_KDF* k) const { EVP_KDF_free(k); }
};
struct KdfCtxDeleter {
    void operator()(EVP_KDF_CTX* c) const { EVP_KDF_CTX_free(c); }
};
struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
constexpr size_t kGcmNonce = 12, kGcmTag = 16;
}  // namespace

bool argon2(const char* variant, std::string_view pass, std::string_view salt, uint32_t memoryKiB, uint32_t passes,
            uint32_t lanes, size_t outLen, std::string& out, std::string& error) {
    if (passes == 0 || lanes == 0 || memoryKiB < 8 * lanes || memoryKiB > (1u << 22) || passes > 10000 || lanes > 255) {
        error = "Invalid Argon2 parameters";
        return false;
    }
    std::unique_ptr<EVP_KDF, KdfDeleter> k(EVP_KDF_fetch(nullptr, variant, nullptr));
    if (!k) {
        error = "Argon2 is not available in this OpenSSL build";
        return false;
    }
    std::unique_ptr<EVP_KDF_CTX, KdfCtxDeleter> ctx(EVP_KDF_CTX_new(k.get()));
    if (!ctx) {
        error = "Out of memory";
        return false;
    }
    uint32_t threads = 1, version = 0x13;
    // OpenSSL rejects a NULL password pointer even with length 0; point at a dummy byte instead.
    static const char kEmpty = 0;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, const_cast<char*>(pass.empty() ? &kEmpty : pass.data()),
                                          pass.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<char*>(salt.data()), salt.size()),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &passes),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memoryKiB),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_VERSION, &version),
        OSSL_PARAM_construct_end()};
    out.assign(outLen, '\0');
    if (EVP_KDF_derive(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), outLen, params) != 1) {
        cleanse(out);
        error = "Argon2 key derivation failed";
        return false;
    }
    return true;
}

std::string aesGcmSeal(std::string_view key32, std::string_view plain, std::string_view aad) {
    if (key32.size() != 32) throw std::invalid_argument("AES-256-GCM needs a 32-byte key");
    std::string out = randomBytes(kGcmNonce);
    out.resize(kGcmNonce + plain.size() + kGcmTag);
    auto* u = reinterpret_cast<unsigned char*>(out.data());
    std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> c(EVP_CIPHER_CTX_new());
    int len = 0, fin = 0;
    const bool ok =
        c && EVP_EncryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kGcmNonce), nullptr) == 1 &&
        EVP_EncryptInit_ex(c.get(), nullptr, nullptr, reinterpret_cast<const unsigned char*>(key32.data()), u) == 1 &&
        EVP_EncryptUpdate(c.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) == 1 &&
        EVP_EncryptUpdate(c.get(), u + kGcmNonce, &len, reinterpret_cast<const unsigned char*>(plain.data()), static_cast<int>(plain.size())) == 1 &&
        EVP_EncryptFinal_ex(c.get(), u + kGcmNonce + len, &fin) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kGcmTag), u + kGcmNonce + plain.size()) == 1;
    if (!ok) {
        cleanse(out);
        throw std::runtime_error("AES-256-GCM encryption failed");
    }
    return out;
}

std::optional<std::string> aesGcmOpen(std::string_view key32, std::string_view sealed, std::string_view aad) {
    if (key32.size() != 32 || sealed.size() < kGcmNonce + kGcmTag) return std::nullopt;
    const size_t n = sealed.size() - kGcmNonce - kGcmTag;
    const auto* s = reinterpret_cast<const unsigned char*>(sealed.data());
    std::string out(n, '\0');
    auto* u = reinterpret_cast<unsigned char*>(out.data());
    std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> c(EVP_CIPHER_CTX_new());
    int len = 0, fin = 0;
    const bool ok =
        c && EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kGcmNonce), nullptr) == 1 &&
        EVP_DecryptInit_ex(c.get(), nullptr, nullptr, reinterpret_cast<const unsigned char*>(key32.data()), s) == 1 &&
        EVP_DecryptUpdate(c.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) == 1 &&
        EVP_DecryptUpdate(c.get(), u, &len, s + kGcmNonce, static_cast<int>(n)) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kGcmTag), const_cast<unsigned char*>(s + kGcmNonce + n)) == 1 &&
        EVP_DecryptFinal_ex(c.get(), u + len, &fin) == 1;
    if (!ok) {
        cleanse(out);
        return std::nullopt;
    }
    return out;
}

static std::string digest(const EVP_MD* md, std::string_view data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!EVP_Digest(data.data(), data.size(), out, &len, md, nullptr)) throw std::runtime_error("digest failed");
    return std::string(reinterpret_cast<char*>(out), len);
}

std::string sha256(std::string_view data) { return digest(EVP_sha256(), data); }
std::string sha1(std::string_view data) { return digest(EVP_sha1(), data); }
std::string md5(std::string_view data) { return digest(EVP_md5(), data); }

static std::string hmacOf(const char* digestName, std::string_view key, std::string_view data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    size_t len = 0;
    if (!EVP_Q_mac(nullptr, "HMAC", nullptr, digestName, nullptr, key.data(), key.size(),
                   reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, sizeof out, &len))
        throw std::runtime_error("HMAC failed");
    return std::string(reinterpret_cast<char*>(out), len);
}

std::string hmacSha256(std::string_view key, std::string_view data) { return hmacOf("SHA256", key, data); }
std::string hmacSha1(std::string_view key, std::string_view data) { return hmacOf("SHA1", key, data); }

std::string randomBytes(size_t n) {
    std::string out(n, '\0');
    if (n && RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return out;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::string_view data, bool pad) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                     static_cast<unsigned char>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t v = static_cast<unsigned char>(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        if (pad) out += "==";
    } else if (rem == 2) {
        uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        if (pad) out.push_back('=');
    }
    return out;
}

bool base64Decode(std::string_view b64, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : b64) {
        if (c == '=') break;
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string fingerprintSha256(std::string_view keyBlob) { return "SHA256:" + base64Encode(sha256(keyBlob), false); }

std::string fingerprintMd5(std::string_view keyBlob) {
    static const char* d = "0123456789abcdef";
    std::string h = md5(keyBlob), out = "MD5:";
    for (size_t i = 0; i < h.size(); ++i) {
        if (i) out.push_back(':');
        unsigned char c = static_cast<unsigned char>(h[i]);
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 15]);
    }
    return out;
}

std::string keyTypeOfBlob(std::string_view keyBlob) {
    if (keyBlob.size() < 4) return {};
    uint32_t len = (static_cast<unsigned char>(keyBlob[0]) << 24) | (static_cast<unsigned char>(keyBlob[1]) << 16) |
                   (static_cast<unsigned char>(keyBlob[2]) << 8) | static_cast<unsigned char>(keyBlob[3]);
    if (len > keyBlob.size() - 4) return {};
    return std::string(keyBlob.substr(4, len));
}

}  // namespace st::crypto
