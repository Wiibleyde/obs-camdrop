#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace camdrop {

// Cryptographically secure random bytes (mbedTLS CTR-DRBG seeded from the OS entropy source).
bool random_bytes(uint8_t *out, size_t len);

// Uniform random integer in [0, bound).
uint32_t random_uniform(uint32_t bound);

// Random string drawn from `alphabet`.
std::string random_string(size_t len, const std::string &alphabet);

// Random numeric access code (digits only).
std::string random_digits(size_t len);

// Random identifier (hex).
std::string random_id(size_t bytes = 8);

// mbedTLS-compatible RNG callback (thread-safe), `ctx` is ignored.
int random_mbedtls(void *ctx, unsigned char *out, size_t len);

} // namespace camdrop
