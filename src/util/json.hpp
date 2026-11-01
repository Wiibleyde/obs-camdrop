#pragma once

// nlohmann/json is shipped with obs-deps (header-only).
#include <nlohmann/json.hpp>

namespace camdrop {
using json = nlohmann::json;

template<typename T> T json_get(const json &j, const char *key, T fallback)
{
	auto it = j.find(key);
	if (it == j.end() || it->is_null())
		return fallback;
	try {
		return it->get<T>();
	} catch (...) {
		return fallback;
	}
}
} // namespace camdrop
