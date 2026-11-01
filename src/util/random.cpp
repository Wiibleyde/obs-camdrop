#include "util/random.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include <mutex>

namespace camdrop {

namespace {

struct Rng {
	std::mutex mutex;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	bool ready = false;

	Rng()
	{
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		static const char pers[] = "camdrop-rng";
		ready = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
					      reinterpret_cast<const unsigned char *>(pers), sizeof(pers) - 1) == 0;
	}

	~Rng()
	{
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
	}
};

Rng &rng()
{
	static Rng instance;
	return instance;
}

} // namespace

bool random_bytes(uint8_t *out, size_t len)
{
	Rng &r = rng();
	std::lock_guard<std::mutex> lock(r.mutex);
	if (!r.ready)
		return false;
	while (len > 0) {
		size_t chunk = len > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : len;
		if (mbedtls_ctr_drbg_random(&r.drbg, out, chunk) != 0)
			return false;
		out += chunk;
		len -= chunk;
	}
	return true;
}

int random_mbedtls(void *, unsigned char *out, size_t len)
{
	return random_bytes(out, len) ? 0 : -1;
}

uint32_t random_uniform(uint32_t bound)
{
	if (bound <= 1)
		return 0;
	// Rejection sampling to avoid modulo bias.
	const uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);
	for (;;) {
		uint32_t v = 0;
		random_bytes(reinterpret_cast<uint8_t *>(&v), sizeof(v));
		if (v < limit)
			return v % bound;
	}
}

std::string random_string(size_t len, const std::string &alphabet)
{
	std::string out;
	out.reserve(len);
	for (size_t i = 0; i < len; i++)
		out.push_back(alphabet[random_uniform(static_cast<uint32_t>(alphabet.size()))]);
	return out;
}

std::string random_digits(size_t len)
{
	return random_string(len, "0123456789");
}

std::string random_id(size_t bytes)
{
	static const char hex[] = "0123456789abcdef";
	std::vector<uint8_t> buf(bytes);
	random_bytes(buf.data(), buf.size());
	std::string out;
	out.reserve(bytes * 2);
	for (uint8_t b : buf) {
		out.push_back(hex[b >> 4]);
		out.push_back(hex[b & 0xf]);
	}
	return out;
}

} // namespace camdrop
