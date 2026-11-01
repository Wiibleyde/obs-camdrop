#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace camdrop {

struct CertificateInfo {
	std::string subject;
	std::string issuer;
	std::string valid_to; // "YYYY-MM-DD"
	std::string sha256;   // colon separated fingerprint
	bool self_signed = false;
};

bool read_text_file(const std::string &path, std::string &out);
bool write_text_file(const std::string &path, const std::string &data);

// Loads (or generates on first use, or when `regenerate` is set) the self-signed certificate stored
// in the plugin config directory. The certificate covers localhost, the host name and the given IPs.
bool load_or_create_self_signed(const std::vector<std::string> &ips, bool regenerate, std::string &cert_pem,
				std::string &key_pem, std::string &error);

// Checks that a PEM certificate chain and private key can be parsed and belong together.
bool validate_certificate(const std::string &cert_pem, const std::string &key_pem, std::string &error);

bool describe_certificate(const std::string &cert_pem, CertificateInfo &info);

// DER bytes of the first certificate of a PEM chain (served at /camdrop.crt for iOS installation).
std::vector<uint8_t> certificate_der(const std::string &cert_pem);

std::string self_signed_cert_path();
std::string self_signed_key_path();

} // namespace camdrop
