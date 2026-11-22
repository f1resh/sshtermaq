#pragma once
// Small crypto/encoding helpers on top of OpenSSL.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace st::crypto {

/**
 * Argon2 (RFC 9106, version 0x13) via OpenSSL 3.2+ EVP_KDF. `variant`: "ARGON2ID", "ARGON2I" or "ARGON2D". False
 * with `error` set on bad parameters or a failure; `out` is wiped then.
 */
bool argon2(const char* variant, std::string_view pass, std::string_view salt, uint32_t memoryKiB, uint32_t passes,
            uint32_t lanes, size_t outLen, std::string& out, std::string& error);

/** AES-256-GCM with a random 12-byte nonce: returns nonce | ciphertext | 16-byte tag. `aad` is authenticated too. */
std::string aesGcmSeal(std::string_view key32, std::string_view plain, std::string_view aad);
/** Opens what aesGcmSeal made; nullopt when the key, the data or the aad do not match. */
std::optional<std::string> aesGcmOpen(std::string_view key32, std::string_view sealed, std::string_view aad);

/** Overwrites the string's bytes (OPENSSL_cleanse) and clears it. */
void cleanse(std::string& s);

std::string sha256(std::string_view data);
std::string sha1(std::string_view data);
std::string md5(std::string_view data);
/** HMAC-SHA-256 (32 raw bytes). */
std::string hmacSha256(std::string_view key, std::string_view data);
/** HMAC-SHA-1 (20 raw bytes). */
std::string hmacSha1(std::string_view key, std::string_view data);
std::string randomBytes(size_t n);

std::string base64Encode(std::string_view data, bool pad = true);
/** Accepts padded/unpadded input, ignores whitespace. Returns false on invalid characters. */
bool base64Decode(std::string_view b64, std::string& out);

/** OpenSSH-style fingerprint of an SSH wire-format public key blob: "SHA256:<unpadded base64>". */
std::string fingerprintSha256(std::string_view keyBlob);
/** Legacy fingerprint: "MD5:aa:bb:..." */
std::string fingerprintMd5(std::string_view keyBlob);

/** Reads the leading SSH string (uint32 length + bytes) of a key blob = the key type, e.g. "ssh-ed25519". */
std::string keyTypeOfBlob(std::string_view keyBlob);

}  // namespace st::crypto
