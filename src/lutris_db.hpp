#pragma once
#include <filesystem>
#include <optional>
#include <string>

/**
 * Reads playtime data from the Lutris SQLite database.
 *
 * Database location: ~/.local/share/lutris/pga.db
 * Table: games   Columns: name (TEXT), playtime (REAL, hours)
 *
 * The playtime is used as the Discord presence start timestamp so the
 * elapsed-time counter reflects total time played rather than session time.
 **/
class LutrisDB {
	public:
		/**
		 * Query total playtime for a game by name.
		 *
		 * Matching is case-insensitive substring (after full-width and typographic
		 * dash normalisation) so slight name differences between Lutris and VNDB
		 * are handled automatically.
		 *
		 * @param gameName  Title to search for (romanized or original script).
		 * @return          Total playtime in seconds, or nullopt if the Lutris DB
		 *                  is not found or the game has no recorded playtime.
		 **/
		[[nodiscard]] static std::optional<int64_t> getPlaytime(const std::string& gameName);

		/**
		 * Format a duration in seconds as a human-readable string.
		 * @param seconds  Duration to format.
		 * @return         "Xh Ym" when hours > 0, otherwise "Ym".
		 *                 Returns "" for zero or negative input.
		 **/
		[[nodiscard]] static std::string formatPlaytime(int64_t seconds);

		/** Return the default Lutris database path. **/
		static std::filesystem::path defaultDbPath();
};
