#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/**
 * Detects the currently running Steam game on Linux.
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
		 * Return the Steam store name of the currently running game, or nullopt
		 * when no Steam game is running or the ACF cannot be read.
		 **/
		[[nodiscard]] static std::optional<std::string> getRunningGameName();

		/**
		 * Return the AppID of the currently running Steam game, or 0 if none.
		 **/
		[[nodiscard]] static int getRunningAppId();

		/**
		 * Return the PID of the currently running Steam game process, or 0 if none.
		 * The same /proc scan used by getRunningAppId(); shares a helper internally.
		 **/
		[[nodiscard]] static int getRunningPid();

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
