#pragma once
#include <filesystem>
#include <string>
#include <unordered_set>

/**
 * Persistent ignore list stored at ~/.config/vn-discord-rpc/ignore.txt.
 *
 * Format: one entry per line, lines starting with # are comments.
 *
 * Matching rules:
 *   - Entries with fewer than 4 characters: **exact** match (case-insensitive)
 *   - Entries with 4+ characters: case-insensitive **substring** match
 *
 * The file is auto-created on first run with common false-positive entries
 * (Steam runtimes, Proton, launchers, etc.).  Edit it while the daemon is
 * running — changes are reloaded automatically on the next poll.
 *
 * To permanently ignore a detected title, either:
 *   - Add it manually to ignore.txt, or
 *   - Let the daemon auto-add it when no VNDB match is found.
 **/
class IgnoreList {
	public:
		/** Construct with an optional custom path (defaults to defaultPath()). **/
		explicit IgnoreList(std::filesystem::path path = defaultPath());

		/**
		 * Return true if the given game name should be suppressed.
		 * Reloads from disk automatically if the file has been modified.
		 **/
		[[nodiscard]] bool matches(const std::string& name);

		/**
		 * Append an entry to the list and persist it to disk immediately.
		 * No-op if the entry already exists.
		 **/
		void add(const std::string& entry);

		/** Return the default ignore list path. **/
		static std::filesystem::path defaultPath();

	private:
		void reload();
		void writeDefaults();

		std::filesystem::path           path_;
		std::unordered_set<std::string> entries_;   ///< Lower-case entries
		std::filesystem::file_time_type lastMtime_{};
};
