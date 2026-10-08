#pragma once
#include <openssl/evp.h>
#include <openssl/sha.h>
inline int mbedtls_sha256(const unsigned char* bytes, size_t count, unsigned char* output, int) {
  return SHA256(bytes, count, output) ? 0 : -1;
}
struct mbedtls_sha256_context {
  EVP_MD_CTX* value = nullptr;
};
inline void mbedtls_sha256_init(mbedtls_sha256_context* context) { context->value = EVP_MD_CTX_new(); }
inline void mbedtls_sha256_free(mbedtls_sha256_context* context) { EVP_MD_CTX_free(context->value); }
inline int mbedtls_sha256_starts(mbedtls_sha256_context* context, int) {
  return context->value && EVP_DigestInit_ex(context->value, EVP_sha256(), nullptr) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* context, const unsigned char* bytes, size_t count) {
  return EVP_DigestUpdate(context->value, bytes, count) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* context, unsigned char* output) {
  return EVP_DigestFinal_ex(context->value, output, nullptr) == 1 ? 0 : -1;
}
