#include "net/certificate.hpp"

#include "platform/platform.hpp"
#include "settings.hpp"
#include "util/random.hpp"

#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include <util/platform.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <tuple>

namespace camdrop {

std::string self_signed_cert_path()
{
	return config_file_path("camdrop-cert.pem");
}

std::string self_signed_key_path()
{
	return config_file_path("camdrop-key.pem");
}

bool read_text_file(const std::string &path, std::string &out)
{
	// os_fopen handles UTF-8 paths on every platform.
	FILE *f = os_fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return true;
}

bool write_text_file(const std::string &path, const std::string &data)
{
	FILE *f = os_fopen(path.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = (fclose(f) == 0) && ok;
	return ok;
}

namespace {

struct PkGuard {
	mbedtls_pk_context pk;
	PkGuard() { mbedtls_pk_init(&pk); }
	~PkGuard() { mbedtls_pk_free(&pk); }
};

struct CrtGuard {
	mbedtls_x509_crt crt;
	CrtGuard() { mbedtls_x509_crt_init(&crt); }
	~CrtGuard() { mbedtls_x509_crt_free(&crt); }
};

std::string mbed_error(int code)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "-0x%04X", static_cast<unsigned>(-code));
	return buf;
}

std::string format_time(time_t t)
{
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	char buf[32];
	snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		 tm.tm_min, tm.tm_sec);
	return buf;
}

bool generate_self_signed(const std::vector<std::string> &ips, std::string &cert_pem, std::string &key_pem,
			  std::string &error)
{
	PkGuard key;
	int ret = mbedtls_pk_setup(&key.pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
	if (ret == 0)
		ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key.pk), random_mbedtls, nullptr);
	if (ret != 0) {
		error = "key generation failed " + mbed_error(ret);
		return false;
	}

	mbedtls_x509write_cert crt;
	mbedtls_x509write_crt_init(&crt);

	const std::string host = platform::host_name();
	// Characters with a meaning in distinguished names are replaced.
	std::string safe_host = host;
	for (char &c : safe_host)
		if (c == ',' || c == '+' || c == ';' || c == '"' || c == '\\' || c == '<' || c == '>' || c == '=')
			c = '-';
	const std::string subject = "CN=Camdrop " + safe_host + ",O=Camdrop";

	mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
	mbedtls_x509write_crt_set_subject_key(&crt, &key.pk);
	mbedtls_x509write_crt_set_issuer_key(&crt, &key.pk);
	ret = mbedtls_x509write_crt_set_subject_name(&crt, subject.c_str());
	if (ret == 0)
		ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject.c_str());

	unsigned char serial[16];
	random_bytes(serial, sizeof(serial));
	serial[0] &= 0x7f; // positive
	serial[0] |= 0x01;
	if (ret == 0)
		ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));

	const time_t now = time(nullptr);
	// 800 days: Apple platforms reject TLS certificates valid for more than 825 days.
	const std::string not_before = format_time(now - 24 * 3600);
	const std::string not_after = format_time(now + 800LL * 24 * 3600);
	if (ret == 0)
		ret = mbedtls_x509write_crt_set_validity(&crt, not_before.c_str(), not_after.c_str());

	// CA:TRUE so that iOS / Android can install it as a trusted root if the user wants to.
	if (ret == 0)
		ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);
	if (ret == 0)
		ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
									MBEDTLS_X509_KU_KEY_CERT_SIGN);
	if (ret == 0)
		ret = mbedtls_x509write_crt_set_subject_key_identifier(&crt);
	if (ret == 0)
		ret = mbedtls_x509write_crt_set_authority_key_identifier(&crt);

	mbedtls_asn1_sequence eku{};
	eku.buf.tag = MBEDTLS_ASN1_OID;
	eku.buf.p = reinterpret_cast<unsigned char *>(const_cast<char *>(MBEDTLS_OID_SERVER_AUTH));
	eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
	eku.next = nullptr;
	if (ret == 0)
		ret = mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku);

	// Subject alternative names: DNS names + every local IPv4 address.
	std::vector<std::string> dns_names = {"localhost"};
	if (!host.empty() && host != "localhost") {
		dns_names.push_back(host);
		dns_names.push_back(host + ".local");
	}
	std::vector<std::array<uint8_t, 4>> ip_bytes;
	ip_bytes.push_back({127, 0, 0, 1});
	for (const auto &ip : ips) {
		std::array<uint8_t, 4> b{};
		if (platform::parse_ipv4(ip, b.data()))
			ip_bytes.push_back(b);
	}

	std::vector<mbedtls_x509_san_list> san(dns_names.size() + ip_bytes.size());
	size_t idx = 0;
	for (auto &name : dns_names) {
		auto &node = san[idx++];
		node = {};
		node.node.type = MBEDTLS_X509_SAN_DNS_NAME;
		node.node.san.unstructured_name.tag = MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_X509_SAN_DNS_NAME;
		node.node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(name.data());
		node.node.san.unstructured_name.len = name.size();
	}
	for (auto &b : ip_bytes) {
		auto &node = san[idx++];
		node = {};
		node.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
		node.node.san.unstructured_name.tag = MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_X509_SAN_IP_ADDRESS;
		node.node.san.unstructured_name.p = b.data();
		node.node.san.unstructured_name.len = b.size();
	}
	for (size_t i = 0; i + 1 < san.size(); i++)
		san[i].next = &san[i + 1];
	if (ret == 0 && !san.empty())
		ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, san.data());

	std::vector<unsigned char> buf(8192);
	if (ret == 0)
		ret = mbedtls_x509write_crt_pem(&crt, buf.data(), buf.size(), random_mbedtls, nullptr);
	mbedtls_x509write_crt_free(&crt);
	if (ret != 0) {
		error = "certificate generation failed " + mbed_error(ret);
		return false;
	}
	cert_pem = reinterpret_cast<const char *>(buf.data());

	std::fill(buf.begin(), buf.end(), 0);
	ret = mbedtls_pk_write_key_pem(&key.pk, buf.data(), buf.size());
	if (ret != 0) {
		error = "key export failed " + mbed_error(ret);
		return false;
	}
	key_pem = reinterpret_cast<const char *>(buf.data());
	std::fill(buf.begin(), buf.end(), 0);
	return true;
}

} // namespace

bool validate_certificate(const std::string &cert_pem, const std::string &key_pem, std::string &error)
{
	CrtGuard crt;
	PkGuard key;
	int ret = mbedtls_x509_crt_parse(&crt.crt, reinterpret_cast<const unsigned char *>(cert_pem.c_str()),
					 cert_pem.size() + 1);
	if (ret != 0) {
		error = "invalid certificate " + mbed_error(ret);
		return false;
	}
	ret = mbedtls_pk_parse_key(&key.pk, reinterpret_cast<const unsigned char *>(key_pem.c_str()),
				   key_pem.size() + 1, nullptr, 0, random_mbedtls, nullptr);
	if (ret != 0) {
		error = "invalid private key " + mbed_error(ret);
		return false;
	}
	ret = mbedtls_pk_check_pair(&crt.crt.pk, &key.pk, random_mbedtls, nullptr);
	if (ret != 0) {
		error = "the private key does not match the certificate " + mbed_error(ret);
		return false;
	}
	return true;
}

bool load_or_create_self_signed(const std::vector<std::string> &ips, bool regenerate, std::string &cert_pem,
				std::string &key_pem, std::string &error)
{
	const std::string cert_path = self_signed_cert_path();
	const std::string key_path = self_signed_key_path();

	if (!regenerate && read_text_file(cert_path, cert_pem) && read_text_file(key_path, key_pem)) {
		std::string err;
		CertificateInfo info;
		if (validate_certificate(cert_pem, key_pem, err) && describe_certificate(cert_pem, info)) {
			// Regenerate when the certificate expires within 30 days.
			CrtGuard crt;
			mbedtls_x509_crt_parse(&crt.crt, reinterpret_cast<const unsigned char *>(cert_pem.c_str()),
					       cert_pem.size() + 1);
			time_t soon = time(nullptr) + 30LL * 24 * 3600;
			std::tm tm{};
#ifdef _WIN32
			gmtime_s(&tm, &soon);
#else
			gmtime_r(&soon, &tm);
#endif
			const auto &to = crt.crt.valid_to;
			bool expiring = std::make_tuple(to.year, to.mon, to.day) <
					std::make_tuple(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
			if (!expiring)
				return true;
		}
	}

	if (!generate_self_signed(ips, cert_pem, key_pem, error))
		return false;
	if (!write_text_file(cert_path, cert_pem) || !write_text_file(key_path, key_pem)) {
		error = "cannot write the certificate to " + cert_path;
		return false;
	}
	return true;
}

static std::string dn_to_string(const mbedtls_x509_name *name)
{
	char buf[512];
	int n = mbedtls_x509_dn_gets(buf, sizeof(buf), name);
	return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

bool describe_certificate(const std::string &cert_pem, CertificateInfo &info)
{
	CrtGuard crt;
	if (mbedtls_x509_crt_parse(&crt.crt, reinterpret_cast<const unsigned char *>(cert_pem.c_str()),
				   cert_pem.size() + 1) != 0)
		return false;
	info.subject = dn_to_string(&crt.crt.subject);
	info.issuer = dn_to_string(&crt.crt.issuer);
	char date[16];
	snprintf(date, sizeof(date), "%04d-%02d-%02d", crt.crt.valid_to.year, crt.crt.valid_to.mon,
		 crt.crt.valid_to.day);
	info.valid_to = date;
	info.self_signed = crt.crt.subject_raw.len == crt.crt.issuer_raw.len &&
			   memcmp(crt.crt.subject_raw.p, crt.crt.issuer_raw.p, crt.crt.subject_raw.len) == 0;

	unsigned char hash[32];
	mbedtls_sha256(crt.crt.raw.p, crt.crt.raw.len, hash, 0);
	static const char hex[] = "0123456789ABCDEF";
	info.sha256.clear();
	for (int i = 0; i < 32; i++) {
		if (i)
			info.sha256.push_back(':');
		info.sha256.push_back(hex[hash[i] >> 4]);
		info.sha256.push_back(hex[hash[i] & 0xf]);
	}
	return true;
}

std::vector<uint8_t> certificate_der(const std::string &cert_pem)
{
	CrtGuard crt;
	if (mbedtls_x509_crt_parse(&crt.crt, reinterpret_cast<const unsigned char *>(cert_pem.c_str()),
				   cert_pem.size() + 1) != 0)
		return {};
	return std::vector<uint8_t>(crt.crt.raw.p, crt.crt.raw.p + crt.crt.raw.len);
}

} // namespace camdrop
