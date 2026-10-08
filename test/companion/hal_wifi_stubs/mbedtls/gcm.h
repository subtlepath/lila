#pragma once
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
inline constexpr int MBEDTLS_CIPHER_ID_AES = 2, MBEDTLS_GCM_ENCRYPT = 1;
struct mbedtls_gcm_context {
  std::array<uint8_t, 32> key{};
  bool ready = false;
};
inline void mbedtls_gcm_init(mbedtls_gcm_context* context) { *context = {}; }
inline void mbedtls_gcm_free(mbedtls_gcm_context* context) { *context = {}; }
inline int mbedtls_gcm_setkey(mbedtls_gcm_context* context, int cipher, const unsigned char* key, unsigned bits) {
  if (cipher != MBEDTLS_CIPHER_ID_AES || bits != 256) return -1;
  std::copy_n(key, 32, context->key.begin());
  context->ready = true;
  return 0;
}
inline int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context* context, int mode, size_t length, const unsigned char* nonce,
                                     size_t nonceLength, const unsigned char* aad, size_t aadLength,
                                     const unsigned char* input, unsigned char* output, size_t tagLength,
                                     unsigned char* tag) {
  if (!context->ready || mode != MBEDTLS_GCM_ENCRYPT || nonceLength != 12 || tagLength != 16) return -1;
  auto* cipher = EVP_CIPHER_CTX_new();
  if (!cipher) return -1;
  int count = 0, final = 0;
  const bool ok = EVP_EncryptInit_ex(cipher, EVP_aes_256_gcm(), nullptr, context->key.data(), nonce) == 1 &&
                  EVP_EncryptUpdate(cipher, nullptr, &count, aad, aadLength) == 1 &&
                  EVP_EncryptUpdate(cipher, output, &count, input, length) == 1 &&
                  EVP_EncryptFinal_ex(cipher, output + count, &final) == 1 &&
                  EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG, tagLength, tag) == 1;
  EVP_CIPHER_CTX_free(cipher);
  return ok ? 0 : -1;
}
inline int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context* context, size_t length, const unsigned char* nonce,
                                    size_t nonceLength, const unsigned char* aad, size_t aadLength,
                                    const unsigned char* tag, size_t tagLength, const unsigned char* input,
                                    unsigned char* output) {
  if (!context->ready || nonceLength != 12 || tagLength != 16) return -1;
  auto* cipher = EVP_CIPHER_CTX_new();
  if (!cipher) return -1;
  int count = 0, final = 0;
  const bool ok = EVP_DecryptInit_ex(cipher, EVP_aes_256_gcm(), nullptr, context->key.data(), nonce) == 1 &&
                  EVP_DecryptUpdate(cipher, nullptr, &count, aad, aadLength) == 1 &&
                  EVP_DecryptUpdate(cipher, output, &count, input, length) == 1 &&
                  EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_TAG, tagLength, const_cast<unsigned char*>(tag)) == 1 &&
                  EVP_DecryptFinal_ex(cipher, output + count, &final) == 1;
  EVP_CIPHER_CTX_free(cipher);
  return ok ? 0 : -1;
}
