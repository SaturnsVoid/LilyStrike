// ============================================================================
// crypt.h - AES-256-GCM encryption helpers (mbedTLS, bundled with the core)
// ----------------------------------------------------------------------------
// Files written to SD by this device use a small envelope so we can version
// later:
//   bytes 0..7   : magic "PCE1" + 4 reserved zero bytes
//   bytes 8..39  : 32-byte random salt   (PBKDF2-HMAC-SHA256, 10k iters)
//   bytes 40..55 : 12-byte GCM nonce
//   rest         : ciphertext || 16-byte GCM tag
// Key is derived from cfg.encPassword. Wrong password => tag check fails.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <vector>

// Encrypt `plain` with a fresh salt+nonce; returns false on mbedtls failure.
bool aesEncryptFileData(const uint8_t* plain, size_t len, std::vector<uint8_t>& out);

// Decrypt an envelope produced above. Returns empty vector on auth failure.
std::vector<uint8_t> aesDecryptFileData(const uint8_t* data, size_t len);

// Convenience wrappers for whole SD files (path on SD_MMC).
bool encryptToFile(const char* path, const String& plain);
bool decryptFromFile(const char* path, String& out);
