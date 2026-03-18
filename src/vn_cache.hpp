#pragma once
#include "vndb_client.hpp"
#include <string>
#include <optional>
#include <unordered_map>
#include <filesystem>

/** Sentinel stored in vndb_id to permanently suppress presence for a key. **/
inline constexpr std::string_view CACHE_SKIP = "SKIP";

/**
 * One row in the CSV cache.
 *
 * CSV column order:
 *   key, alias, vndb_id, title, alt_title, image_url,
 *   image_sexual, image_violence, rating, released, cached_at
 **/
struct CacheEntry {
	std::string key;            ///< Detected game name (the search term)
	std::string alias;          ///< If set, redirect lookup to this search term
	std::string vndb_id;        ///< "SKIP", a real id like "v562", or "" = no match
	std::string title;          ///< VNDB romanized title
	std::string alt_title;      ///< VNDB original-script title (Japanese/Chinese/Korean)
	std::string image_url;      ///< Cover image URL
	double      image_sexual   = 0.0;  ///< VNDB sexual rating (0–2)
	double      image_violence = 0.0;  ///< VNDB violence rating (0–2)
	double      rating         = 0.0;  ///< VNDB community rating (0–100)
	std::string released;              ///< Release date string
	int64_t     cached_at      = 0;    ///< Unix timestamp of last write

	/** Return true when this entry is explicitly skipped (vndb_id == "SKIP"). **/
	[[nodiscard]] bool isSkip()     const { return vndb_id == CACHE_SKIP; }

	/** Return true when a valid VNDB match is stored in this entry. **/
	[[nodiscard]] bool hasMatch()   const { return !vndb_id.empty() && !isSkip(); }

	/** Convert the cached data to a VnInfo struct for use in RpcManager. **/
	[[nodiscard]] VnInfo toVnInfo() const;
};

/**
 * Persistent CSV cache and user alias table.
 *
 * File: ~/.config/vn-discord-rpc/cache.csv
 *
 * The cache is automatically reloaded from disk whenever the file's mtime
 * changes, so edits made while the daemon is running take effect immediately.
 *
 * User-editable features:
 *   - **Alias**:    set alias column → redirects lookup to a different search term
 *   - **Skip**:     set vndb_id = SKIP → permanently suppresses presence
 *   - **Override**: fill vndb_id + title manually → bypasses VNDB query
 **/
class VnCache {
	public:
		/** Construct with an optional custom path (defaults to defaultPath()). **/
		explicit VnCache(std::filesystem::path path = defaultPath());
		~VnCache() = default;

		/**
		 * Look up a key in the cache.
		 * Dash variants (en/em/full-width) are normalised before comparison.
		 * @return CacheEntry if found, nullopt on cache miss.
		 **/
		[[nodiscard]] std::optional<CacheEntry> lookup(const std::string& key);

		/** Store a successful VNDB match keyed by the detected game name. **/
		void store(const std::string& key, const VnInfo& vn);

		/** Store a "no match" placeholder (legacy — prefer ignoreList.add()). **/
		void storeNoMatch(const std::string& key);

		/** Store a plain alias entry (key → aliasTarget, no VnInfo yet). **/
		void storeAlias(const std::string& key, const std::string& aliasTarget);

		/**
		 * Populate an alias row in-place after a successful VNDB lookup.
		 * The alias column is preserved so the redirect remains visible in the CSV.
		 **/
		void storeAliasFilled(const std::string& key, const std::string& alias,
				const VnInfo& vn);

		/** Reload from disk if the file has changed since last read. **/
		void reload();

		/** Write all in-memory entries to disk (UTF-8 BOM included). **/
		void save() const;

		/** Return the default cache file path. **/
		static std::filesystem::path defaultPath();

		/** Return the default SQLite DB path. **/
		static std::filesystem::path defaultDbPath();

	private:
		std::filesystem::path                       path_;
		std::unordered_map<std::string, CacheEntry> entries_;
		std::filesystem::file_time_type             lastMtime_{};

		void parse(const std::string& csv);
		[[nodiscard]] static std::string serialise(const CacheEntry& e);
		[[nodiscard]] static std::vector<std::string> splitCsvRow(const std::string& line);
		[[nodiscard]] static std::string quoteCsvField(const std::string& s);
};
