#pragma once
#include <stdint.h>
#include <stddef.h>

// Token cifrato con AES-256-GCM; chiave derivata dal PIN (simil-PBKDF2 via SHA-256).
// ciphertext[512] contiene con ampio margine il token OAuth (sk-ant-oat01-..., ~110 caratteri).
struct EncryptedBlob {
    uint8_t  iv[12];
    uint8_t  tag[16];
    uint8_t  salt[6];
    uint16_t len;
    uint8_t  ciphertext[512];
};

void deriveKey(const char* pin, const uint8_t* salt, size_t saltLen, uint8_t* keyOut32);
bool encryptToken(const char* plaintext, const char* pin, EncryptedBlob& blob);
bool decryptToken(const EncryptedBlob& blob, const char* pin, char* plainOut, size_t plainMaxLen);
