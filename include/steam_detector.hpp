#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/** A running Steam game found by SteamDetector::getRunningGames(). **/
struct SteamGame {
	int         appId = 0;  ///< Steam AppID
	int         pid   = 0;  ///< First process found carrying this AppID
	std::string name;       ///< Steam store name from appmanifest_<id>.acf
};

/**
 * Detects running Steam games on Linux.
 *
 * Strategy:
 *   1. Scan /proc/<pid>/environ for SteamAppId=<id>  (Steam sets this for
 *      every launched game process — works with Proton, native, and tools).
 *   2. Locate steamapps/appmanifest_<id>.acf across all Steam library paths
 *      (reads libraryfolders.vdf for custom library locations).
 *   3. Parse the "name" field from the ACF — this is the exact Steam store
 *      name, independent of install directory.
 **/
class SteamDetector {
	public:
		/**
		 * Return every running Steam game (one entry per distinct AppID).
		 * Games whose ACF cannot be read are omitted.  Empty when no Steam
		 * game is running.
		 **/
		[[nodiscard]] static std::vector<SteamGame> getRunningGames();

		/**
		 * Read total playtime for a given AppID from Steam local userdata.
		 * Reads ~/.local/share/Steam/userdata/<steamid>/config/localconfig.vdf
		 * @return Playtime in minutes, or 0 if not found.
		 **/
		[[nodiscard]] static int64_t getPlaytimeMinutes(int appId);

	private:
		/** Return all Steam library root paths (includes custom libraries). **/
		static std::vector<std::filesystem::path> libraryPaths();

		/** Parse a quoted string field from an ACF/VDF file. **/
		static std::string parseVdfField(const std::string& content,
				const std::string& key);
};
