#pragma once
#include <string>
#include <optional>

#include <config.hpp>

/**
 * Metadata for a single visual novel returned by the VNDB API.
 * All string fields may be empty if the API did not return that field.
 **/
struct VnInfo {
	std::string id;             ///< VNDB entry ID, e.g. "v562"
	std::string title;          ///< Romanized title (always present)
	std::string alt_title;      ///< Original-script title, e.g. Japanese (may be empty)
	std::string image_url;      ///< Cover image URL (empty when content is explicit)
	std::string vndb_url;       ///< Full https://vndb.org/vXXX deep-link
	double      rating         = 0.0;   ///< VNDB community rating, 0–100 (0 = unrated)
	std::string released;             ///< Release date, "YYYY-MM-DD" or "YYYY" or empty
	int         length_minutes = 0;   ///< Estimated reading length in minutes

	/**
	 * VNDB image-content ratings on a 0–2 scale:
	 *   0 = Safe   1 = Suggestive   2 = Explicit
	 **/
	double image_sexual   = 0.0;  ///< Sexual content rating
	double image_violence = 0.0;  ///< Violence content rating
	int    image_votecount  = 0;  ///< VNDB Vote Count

	/**
	 * Return true when the cover image should be suppressed in Discord.
	 * Ratings are only trusted when backed by at least IMAGE_VOTECOUNT votes.
	 **/
	[[nodiscard]] bool isImageExplicit() const noexcept {
		if (image_votecount < config::IMAGE_VOTECOUNT) return false;
		return image_sexual >= config::IMAGE_SEXUAL || image_violence >= config::IMAGE_VIOLENCE;
	}
};

/**
 * HTTP client for the VNDB Kana REST API v1.
 *
 * Searches are fuzzy-matched using trigram similarity and cached in memory
 * for config::VNDB_CACHE_TTL minutes to avoid hammering the API.
 *
 * Usage:
 * @code
 *   VndbClient vndb;
 *   if (auto info = vndb.search("Clannad"))
 *       std::cout << info->title;
 * @endcode
 **/
class VndbClient {
public:
	VndbClient();
	~VndbClient();

	/**
	 * Search VNDB by title.
	 * Results are in-memory cached for config::VNDB_CACHE_TTL.
	 * @return Best-matching VnInfo, or nullopt on failure / no match.
	 **/
	[[nodiscard]] std::optional<VnInfo> search(const std::string& title);

private:
	[[nodiscard]] std::optional<VnInfo> doSearch(const std::string& title);

	/** Search the /release endpoint and return the linked VN, or nullopt. **/
	[[nodiscard]] std::optional<VnInfo> searchViaRelease(const std::string& title);

	/** Fetch a VN directly by its VNDB ID (e.g. "v1234"). **/
	[[nodiscard]] std::optional<VnInfo> fetchById(const std::string& vnId);

	static std::string buildRequestBody(const std::string& title);
	static std::string buildReleaseRequestBody(const std::string& title);
	static std::optional<VnInfo> parseResponse(const std::string& json,
											   const std::string& query);
	/** Compute trigram similarity between two strings (0.0–1.0). **/
	static double titleSimilarity(const std::string& a, const std::string& b);
};
