#include "logger.hpp"
#include "vndb_client.hpp"
#include "config.hpp"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <iostream>

using json = nlohmann::json;

// ── libcurl write callback — appends received bytes to a std::string ──
static size_t curlWriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata)
{
	auto* buf = reinterpret_cast<std::string*>(userdata);
	buf->append(reinterpret_cast<char*>(ptr), size * nmemb);
	return size * nmemb;
}

// ── Helpers ──
namespace {

	std::string toLower(std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), ::tolower);
		return s;
	}

	// Normalize full-width ASCII variants (U+FF01-U+FF5E) to half-width,
	// and full-width space (U+3000) to regular space.
	// This ensures "妹ぱらだいす！２" matches "妹ぱらだいす!2" in trigrams.
	std::string normalizeWidth(const std::string& s) {
		std::string out;
		out.reserve(s.size());
		size_t i = 0;
		while (i < s.size()) {
			unsigned char c0 = (unsigned char)s[i];
			// Full-width ASCII: U+FF01-FF3F -> EF BC 81-BF -> ASCII 0x21-0x3F
			if (c0 == 0xEF && i + 2 < s.size()) {
				unsigned char c1 = (unsigned char)s[i+1];
				unsigned char c2 = (unsigned char)s[i+2];
				if (c1 == 0xBC && c2 >= 0x81 && c2 <= 0xBF) {
					out += (char)(c2 - 0x81 + 0x21); i += 3; continue;
				}
				// Full-width ASCII: U+FF40-FF5E -> EF BD 80-9E -> ASCII 0x40-0x5E
				if (c1 == 0xBD && c2 >= 0x80 && c2 <= 0x9E) {
					out += (char)(c2 - 0x80 + 0x40); i += 3; continue;
				}
			}
			// Full-width space U+3000 -> E3 80 80
			if (c0 == 0xE3 && i + 2 < s.size() &&
					(unsigned char)s[i+1] == 0x80 && (unsigned char)s[i+2] == 0x80) {
				out += ' '; i += 3; continue;
			}
			out += (char)c0; ++i;
		}
		return out;
	}

	// Very simple trigram-based similarity (0.0-1.0).
	// Good enough to fuzzy-match "Clannad" against "CLANNAD" or minor variants.
	std::vector<std::string> trigrams(const std::string& s)
	{
		std::string t = toLower(normalizeWidth(s));
		std::vector<std::string> tg;
		if (t.size() < 3) {
			tg.push_back(t);
			return tg;
		}
		tg.reserve(t.size() - 2);
		for (size_t i = 0; i + 2 < t.size(); ++i)
			tg.push_back(t.substr(i, 3));
		return tg;
	}

	double trigramSimilarity(const std::string& a, const std::string& b)
	{
		if (a.empty() || b.empty()) return 0.0;
		if (toLower(normalizeWidth(a)) == toLower(normalizeWidth(b))) return 1.0;

		auto ta = trigrams(a);
		auto tb = trigrams(b);

		// Count intersection
		std::sort(ta.begin(), ta.end());
		std::sort(tb.begin(), tb.end());

		std::vector<std::string> inter;
		std::set_intersection(ta.begin(), ta.end(),
				tb.begin(), tb.end(),
				std::back_inserter(inter));

		double denom = static_cast<double>(ta.size() + tb.size());
		if (denom == 0) return 0.0;
		return 2.0 * static_cast<double>(inter.size()) / denom;
	}

} // anonymous namespace

// ── VndbClient ──
VndbClient::VndbClient() {
	curl_global_init(CURL_GLOBAL_DEFAULT);
}
VndbClient::~VndbClient() {
	curl_global_cleanup();
}

std::optional<VnInfo> VndbClient::search(const std::string& title)
{
	auto result = doSearch(title);
	if (!result) {
		LOG_INFO("VN search failed — trying release search for \"" << title << "\"");
		result = searchViaRelease(title);
	}
	return result;
}

// ─── HTTP POST to VNDB API ───
/** Issue an HTTP POST to the VNDB Kana API and return the parsed result. **/
std::optional<VnInfo> VndbClient::doSearch(const std::string& title)
{
	CURL* curl = curl_easy_init();
	if (!curl) {
		LOG_ERR("curl_easy_init() failed");
		return std::nullopt;
	}

	std::string responseBody;
	std::string requestBody = buildRequestBody(title);

	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: application/json");
	// Polite user-agent (VNDB's docs request this)
	headers = curl_slist_append(headers, "User-Agent: VN-Presence (github; contact via issues)");

	curl_easy_setopt(curl, CURLOPT_URL,            config::VNDB_API_URL.data());
	curl_easy_setopt(curl, CURLOPT_POST,           1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     requestBody.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)requestBody.size());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &responseBody);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	LOG_DEBUG("POST " << config::VNDB_API_URL << "  body=" << requestBody);
	CURLcode res = curl_easy_perform(curl);

	long httpCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	LOG_DEBUG("HTTP response: " << httpCode
			<< "  body_size=" << responseBody.size() << " bytes");

	if (res != CURLE_OK) {
		LOG_ERR("CURL error: " << curl_easy_strerror(res));
		return std::nullopt;
	}
	if (httpCode != 200) {
		LOG_ERR("HTTP " << httpCode << " for title: " << title);
		if (!responseBody.empty()) LOG_DEBUG("Response body: " << responseBody);
		return std::nullopt;
	}

	LOG_DEBUG("Raw VNDB response: " << responseBody);
	return parseResponse(responseBody, title);
}

// ─── Build JSON request body ───
/** Build the JSON request body for a VNDB /vn search query. **/
std::string VndbClient::buildRequestBody(const std::string& title)
{
	json body = {
		{"filters",  {"search", "=", title}},
		{"fields",   config::VNDB_FIELDS},
		{"results",  config::VNDB_MAX_RESULTS},
		{"sort",     "searchrank"},
	};
	return body.dump();
}

// ─── Parse JSON response ───
/**
 * Parse a raw VNDB JSON response and return the best-matching VnInfo.
 * Rejects results below config::VNDB_MIN_SIMILARITY.
 **/
std::optional<VnInfo> VndbClient::parseResponse(const std::string& jsonStr,
		const std::string& query)
{
	json root;
	try {
		root = json::parse(jsonStr);
	} catch (const json::exception& e) {
		LOG_ERR("JSON parse error: " << e.what());
		return std::nullopt;
	}

	if (!root.contains("results") || root["results"].empty())
		return std::nullopt;

	const auto& first = root["results"][0];

	// Helper: safely get a string field that may be null in the JSON
	auto safeStr = [](const json& obj, const std::string& key) -> std::string {
		if (!obj.contains(key) || obj[key].is_null()) return "";
		if (!obj[key].is_string()) return "";
		return obj[key].get<std::string>();
	};
	auto safeDouble = [](const json& obj, const std::string& key, double def) -> double {
		if (!obj.contains(key) || obj[key].is_null()) return def;
		if (!obj[key].is_number()) return def;
		return obj[key].get<double>();
	};
	auto safeInt = [](const json& obj, const std::string& key, int def) -> int {
		if (!obj.contains(key) || obj[key].is_null()) return def;
		if (!obj[key].is_number()) return def;
		return obj[key].get<int>();
	};

	VnInfo info;
	info.id        = safeStr(first, "id");
	info.title     = safeStr(first, "title");
	info.alt_title = safeStr(first, "alttitle");

	// Cover image — always parse ratings. Only store the URL when SFW.
	// RPC still shows even when cover is suppressed.
	if (first.contains("image") && !first["image"].is_null()) {
		const auto& img = first["image"];
		info.image_sexual   = safeDouble(img, "sexual",   0.0);
		info.image_violence = safeDouble(img, "violence", 0.0);
		info.image_votecount = safeInt(img, "votecount", 0);
		if (!info.isImageExplicit())
			info.image_url = safeStr(img, "url");
		else
			LOG_INFO("All covers explicit (sexual=" << info.image_sexual
					<< " violence=" << info.image_violence
					<< " votecount=" << info.image_votecount << ") — cover suppressed, RPC will still show");
	}

	info.rating         = safeDouble(first, "rating",         0.0);
	info.released       = safeStr   (first, "released");
	info.length_minutes = safeInt   (first, "length_minutes", 0);
	info.vndb_url       = std::string(config::VNDB_BASE_URL) + info.id;

	// Reject results that don't look like a good match
	double sim = titleSimilarity(query, info.title);
	double simAlt = info.alt_title.empty() ? 0.0
		: titleSimilarity(query, info.alt_title);
	double bestSim = std::max(sim, simAlt);

	if (bestSim < config::VNDB_MIN_SIMILARITY) {
		LOG_DEBUG("Low similarity (" << bestSim << ") between \"" << query << "\" and \"" << info.title << "\" — skipping");
		return std::nullopt;
	}

	LOG_INFO("VNDB matched \"" << info.title << "\" (sim=" << bestSim << ", id=" << info.id << ")");
	return info;
}

// ─── Build release JSON request body ───
std::string VndbClient::buildReleaseRequestBody(const std::string& title)
{
	json body = {
		{"filters",  {"search", "=", title}},
		{"fields",   config::VNDB_RELEASE_FIELDS},
		{"results",  config::VNDB_MAX_RESULTS},
		{"sort",     "searchrank"},
	};
	return body.dump();
}

// ─── Generic HTTP POST ───
static std::string httpPost(const std::string& url, const std::string& requestBody)
{
	CURL* curl = curl_easy_init();
	if (!curl) return {};

	std::string responseBody;
	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "User-Agent: VN-Presence/1.0 (github; contact via issues)");

	curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST,           1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     requestBody.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)requestBody.size());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &responseBody);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	CURLcode res = curl_easy_perform(curl);
	long httpCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		LOG_ERR("CURL error: " << curl_easy_strerror(res));
		return {};
	}
	if (httpCode != 200) {
		LOG_ERR("HTTP " << httpCode << " from " << url);
		return {};
	}
	return responseBody;
}

// ─── Search via /release, follow vns[].id back to the VN ───
std::optional<VnInfo> VndbClient::searchViaRelease(const std::string& title)
{
	std::string reqBody = buildReleaseRequestBody(title);
	LOG_DEBUG("Release POST body=" << reqBody);
	std::string resp = httpPost(std::string(config::VNDB_RELEASE_API_URL), reqBody);
	if (resp.empty()) return std::nullopt;

	json root;
	try { root = json::parse(resp); } catch (...) {
		LOG_ERR("Release JSON parse error");
		return std::nullopt;
	}

	if (!root.contains("results") || root["results"].empty()) {
		LOG_DEBUG("Release search: no results for \"" << title << "\"");
		return std::nullopt;
	}

	const auto& first = root["results"][0];

	// Extract first linked VN id
	std::string vnId;
	if (first.contains("vns") && first["vns"].is_array() && !first["vns"].empty()) {
		const auto& vn0 = first["vns"][0];
		if (vn0.contains("id") && vn0["id"].is_string())
			vnId = vn0["id"].get<std::string>();
	}

	if (vnId.empty()) {
		LOG_DEBUG("Release search: no VN relation found for \"" << title << "\"");
		return std::nullopt;
	}

	std::string releaseTitle;
	if (first.contains("title") && first["title"].is_string())
		releaseTitle = first["title"].get<std::string>();

	LOG_INFO("Release match: \"" << releaseTitle << "\" → VN id=" << vnId);
	return fetchById(vnId);
}

// ─── Fetch VN by ID ───
std::optional<VnInfo> VndbClient::fetchById(const std::string& vnId)
{
	json body = {
		{"filters",  {"id", "=", vnId}},
		{"fields",   config::VNDB_FIELDS},
		{"results",  1},
	};
	std::string reqBody = body.dump();
	LOG_DEBUG("fetchById POST body=" << reqBody);

	std::string resp = httpPost(std::string(config::VNDB_API_URL), reqBody);
	if (resp.empty()) return std::nullopt;

	// Re-use parseResponse with the vnId as query so similarity is skipped
	// (we already know the exact ID — just parse and return)
	json root;
	try { root = json::parse(resp); } catch (...) {
		LOG_ERR("fetchById JSON parse error");
		return std::nullopt;
	}

	if (!root.contains("results") || root["results"].empty()) {
		LOG_WARN("fetchById: no results for " << vnId);
		return std::nullopt;
	}

	// Parse manually (skip similarity check — we fetched by exact ID)
	const auto& first = root["results"][0];
	auto safeStr = [](const json& obj, const std::string& key) -> std::string {
		if (!obj.contains(key) || obj[key].is_null() || !obj[key].is_string()) return "";
		return obj[key].get<std::string>();
	};
	auto safeDouble = [](const json& obj, const std::string& key, double def) -> double {
		if (!obj.contains(key) || obj[key].is_null() || !obj[key].is_number()) return def;
		return obj[key].get<double>();
	};
	auto safeInt = [](const json& obj, const std::string& key, int def) -> int {
		if (!obj.contains(key) || obj[key].is_null() || !obj[key].is_number()) return def;
		return obj[key].get<int>();
	};

	VnInfo info;
	info.id        = safeStr(first, "id");
	info.title     = safeStr(first, "title");
	info.alt_title = safeStr(first, "alttitle");

	if (first.contains("image") && !first["image"].is_null()) {
		const auto& img = first["image"];
		info.image_sexual   = safeDouble(img, "sexual",   0.0);
		info.image_violence = safeDouble(img, "violence", 0.0);
		info.image_votecount = safeInt(img, "votecount", 0);
		if (!info.isImageExplicit())
			info.image_url = safeStr(img, "url");
		else
			LOG_INFO("Cover suppressed (sexual=" << info.image_sexual
					<< " violence=" << info.image_violence 
					<< " votecount=" << info.image_votecount << ")");
	}

	info.rating         = safeDouble(first, "rating",         0.0);
	info.released       = safeStr   (first, "released");
	info.length_minutes = safeInt   (first, "length_minutes", 0);
	info.vndb_url       = std::string(config::VNDB_BASE_URL) + info.id;

	LOG_INFO("fetchById: matched \"" << info.title << "\" (id=" << info.id << ")");
	return info;
}

// ─── Title similarity wrapper ───
double VndbClient::titleSimilarity(const std::string& a, const std::string& b) {
	return trigramSimilarity(a, b);
}
