#pragma once
#include <string>
#include <optional>
#include <vector>

/**
 * A VN game process that was found running on the system.
 * Populated by ProcessScanner::scan().
 **/
struct VnProcess {
	std::string gameName;   ///< Human-readable title to search on VNDB
	std::string exePath;    ///< Full Linux path to the executable being run
	int         pid = 0;    ///< Linux process ID
	std::string source;     ///< Detection method: "lutris" | "wine-exe" | "gamescope" |
									///<                   "steam-native" | "native-engine" | "steam-arg"
};

/**
 * Scans /proc for running VN game processes.
 *
 * Works regardless of window focus — gamescope, nested compositors, and
 * minimised windows are all detected correctly.
 *
 * Detection strategy:
 *   1. **lutris** — python3 lutris-wrapper <name> ...
 **/
class ProcessScanner {
	public:
		/**
		 * Scan /proc and return all candidate VN processes sorted by
		 * detection-source priority (lutris first, steam-arg last).
		 **/
		[[nodiscard]] static std::vector<VnProcess> scan();

		/**
		 * Convenience wrapper — return the highest-priority candidate or nullopt.
		 **/
		[[nodiscard]] static std::optional<VnProcess> findFirst();

	private:
		/** Read /proc/<pid>/cmdline and split on NUL bytes. **/
		static std::vector<std::string> readCmdline(int pid);
};
