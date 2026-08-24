// ============================================================================
// crypt.cpp - AES-256-GCM file envelope (see crypt.h for format)
// ============================================================================
#include "crypt.h"
#include "config.h"
#include <SD_MMC.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <esp_system.h>

static const uint8_t MAGIC[4] = {'P','C','E','1'};
#define SALT_LEN  32
#define NONCE_LEN 12
#define TAG_LEN   16
#define HEADER    (4 + 4 + SALT_LEN + NONCE_LEN)   // 56 bytes
#define PBKDF2_ITERS 10000

// Derive a 32-byte AES key from cfg.encPassword + salt.
// Uses the newer mbedtls 3.x API (pbkdf2_hmac_ext takes md_type directly).
static bool deriveKey(const uint8_t* salt, uint8_t outKey[32]) {
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        (const unsigned char*)cfg.encPassword, strlen(cfg.encPassword),
        salt, SALT_LEN, PBKDF2_ITERS, 32, outKey);
    return rc == 0;
}

bool aesEncryptFileData(const uint8_t* plain, size_t len, std::vector<uint8_t>& out) {
    if (!len) return false;
    out.assign(HEADER + len + TAG_LEN, 0);
    memcpy(out.data(), MAGIC, 4);                       // magic
    uint8_t* salt = out.data() + 8;
    uint8_t* nonce = out.data() + 8 + SALT_LEN;
    esp_fill_random(salt, SALT_LEN);                    // hw RNG
    esp_fill_random(nonce, NONCE_LEN);

    uint8_t key[32];
    if (!deriveKey(salt, key)) return false;

    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len,
                    nonce, NONCE_LEN, nullptr, 0,
                    plain, out.data() + HEADER,
                    TAG_LEN, out.data() + HEADER + len) == 0;
    mbedtls_gcm_free(&gcm);
    memset(key, 0, sizeof(key));
    return ok;
}

std::vector<uint8_t> aesDecryptFileData(const uint8_t* data, size_t len) {
    std::vector<uint8_t> empty;
    if (len <= HEADER + TAG_LEN || memcmp(data, MAGIC, 4) != 0) return empty;
    size_t ctLen = len - HEADER - TAG_LEN;
    const uint8_t* salt  = data + 8;
    const uint8_t* nonce = data + 8 + SALT_LEN;

    uint8_t key[32];
    if (!deriveKey(salt, key)) return empty;

    std::vector<uint8_t> pt(ctLen);
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) ||
             mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_DECRYPT, ctLen,
                   nonce, NONCE_LEN, nullptr, 0,
                   data + HEADER, pt.data(),
                   TAG_LEN, const_cast<uint8_t*>(data + HEADER + ctLen));
    mbedtls_gcm_free(&gcm);
    memset(key, 0, sizeof(key));
    if (rc != 0) return empty;      // wrong password or corrupted file
    return pt;
}

bool encryptToFile(const char* path, const String& plain) {
    std::vector<uint8_t> enc;
    if (!aesEncryptFileData((const uint8_t*)plain.c_str(), plain.length(), enc))
        return false;
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) return false;
    size_t w = f.write(enc.data(), enc.size());
    f.close();
    return w == enc.size();
}

bool decryptFromFile(const char* path, String& out) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    std::vector<uint8_t> buf(sz);
    size_t r = f.read(buf.data(), sz);
    f.close();
    if (r != sz) return false;
    auto pt = aesDecryptFileData(buf.data(), buf.size());
    if (pt.empty()) return false;
    out = String((const char*)pt.data());
    return true;
}
