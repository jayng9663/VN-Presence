#pragma once
#include <string_view>
#include <chrono>

/** Central configuration — edit this file to customise all daemon behaviour. **/
namespace config {

	/** Discord application ID. **/
	inline constexpr std::string_view DISCORD_APP_ID = "1482345564698841189";

	/** Activity type**/
	inline constexpr int DISCORD_ACTIVITY_TYPE = 0; // 0=Game, 1=Streaming, 2=Listening, 3=Watching

	/** What the member list shows after "Playing": 0=app name, 1=state line, 2=details (VN title). **/
	inline constexpr int DISCORD_STATUS_DISPLAY = 2;

	/** Base URL of the VNDB Kana REST API. **/
	inline constexpr std::string_view VNDB_API_URL         = "https://api.vndb.org/kana/vn";

	/** Base URL of the VNDB Kana Release API (fallback search). **/
	inline constexpr std::string_view VNDB_RELEASE_API_URL = "https://api.vndb.org/kana/release";

	/** Fields requested from VNDB Release per query. **/
	inline constexpr std::string_view VNDB_RELEASE_FIELDS  = "id,title,vns.id";

	/** Base URL used when building vndb.org deep-links. **/
	inline constexpr std::string_view VNDB_BASE_URL = "https://vndb.org/";

	/** Fields requested from VNDB per query.
	 * Keep minimal to stay within the API response-size limits.
	 **/
	inline constexpr std::string_view VNDB_FIELDS =
		"id,title,alttitle,image.url,image.sexual,image.violence,image.votecount,rating,released,length_minutes,devstatus";

	/** Maximum number of VNDB results returned per search (we only use index 0). **/
	inline constexpr int VNDB_MAX_RESULTS = 1;

	/** Covers rated at or above IMAGE_SEXUAL / IMAGE_VIOLENCE are suppressed.
	 * Covers with fewer than IMAGE_VOTECOUNT votes are suppressed too, since
	 * their ratings can't be trusted yet.
	 **/
	inline constexpr double IMAGE_SEXUAL   = 1.80;
	inline constexpr double IMAGE_VIOLENCE = 1.80;
	inline constexpr int    IMAGE_VOTECOUNT = 5;

	/** Minimum trigram-similarity score (0.0–1.0) needed to accept a VNDB result.
	 * Lower = more permissive, higher = stricter.
	 **/
	inline constexpr double VNDB_MIN_SIMILARITY = 0.35;

	/** How often the daemon scans /proc for running VN processes. **/
	inline constexpr std::chrono::seconds POLL_INTERVAL{5};

	/** How long to cache an in-memory VNDB result before re-querying. **/
	inline constexpr std::chrono::hours VNDB_CACHE_TTL{24};

	/** Wait before retrying a title after a VNDB network/server error.
	 * Starts at VNDB_RETRY_MIN and doubles on each failure, up to VNDB_RETRY_MAX.
	 **/
	inline constexpr std::chrono::seconds VNDB_RETRY_MIN{30};
	inline constexpr std::chrono::seconds VNDB_RETRY_MAX{600};

	/** Number of consecutive polls a title must be stable before acting on it. **/
	inline constexpr int STABLE_TITLE_POLLS = 2;

	/** Discord presence state string shown while reading. **/
	inline constexpr std::string_view RPC_STATE_READING  = "Reading";

	/** Discord presence state string shown while idle. **/
	inline constexpr std::string_view RPC_STATE_IDLE     = "Idle";

	/** Discord asset key for the small image (VNDB logo). Upload to Dev Portal. **/
	inline constexpr std::string_view RPC_SMALL_IMG_KEY  = "vndb_logo";

	/** Append the VNDB rating to the cover's hover text, e.g. "CLANNAD — ★ 8.73". **/
	inline constexpr bool RPC_SHOW_RATING = true;

	/** Tooltip text for the small image. **/
	inline constexpr std::string_view RPC_SMALL_IMG_TEXT = "VNDB";

	/** Details line shown when no VNDB title is matched. **/
	inline constexpr std::string_view RPC_DEFAULT_DETAILS = "Playing a Visual Novel";

	/** Use SQLite instead of CSV for the persistent cache.
	 * true  → ~/.config/vn-discord-rpc/cache.db  (SQLite) !! Experimental !!
	 * false → ~/.config/vn-discord-rpc/cache.csv (default)
	 **/
	inline constexpr bool CACHE_USE_DB = false;

} // namespace config
