#include "logger.hpp"
#include "ignore_list.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>

namespace {
	std::string toLower(std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), ::tolower);
		return s;
	}
} // namespace

IgnoreList::IgnoreList(std::filesystem::path path) : path_(std::move(path)) {
	std::error_code ec;
	std::filesystem::create_directories(path_.parent_path(), ec);
	if (!std::filesystem::exists(path_, ec))
		writeDefaults();
	reload();
}

std::filesystem::path IgnoreList::defaultPath() {
	const char* xdg  = std::getenv("XDG_CONFIG_HOME");
	const char* home = std::getenv("HOME");
	std::filesystem::path base = xdg
	? std::filesystem::path(xdg)
	: std::filesystem::path(home ? home : "") / ".config";
	return base / "vn-discord-rpc" / "ignore.txt";
}

/** Re-read ignore.txt from disk if its mtime has changed. **/
void IgnoreList::reload() {
	std::error_code ec;
	if (!std::filesystem::exists(path_, ec)) return;

	auto mtime = std::filesystem::last_write_time(path_, ec);
	if (ec || mtime == lastMtime_) return;

	std::ifstream f(path_);
	if (!f) return;

	entries_.clear();
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		auto low = toLower(line);
		entries_.insert(low);
		LOG_DEBUG("Ignore entry: \"" << low << "\"");
	}
	lastMtime_ = mtime;
	LOG_INFO("Ignore list loaded: " << entries_.size()
	<< " entries from " << path_.string());
}

/**
 * Return true if name matches any ignore entry.
 * Short entries (< 4 chars) use exact match; longer entries use substring.
 **/
bool IgnoreList::matches(const std::string& name) {
	reload();
	std::string low = toLower(name);
	for (const auto& entry : entries_) {
		bool hit = (entry.size() < 4)
		? (low == entry)
		: (low.find(entry) != std::string::npos);
		if (hit) {
			LOG_DEBUG("Ignore match: \"" << name << "\" matched \"" << entry << "\"");
			return true;
		}
	}
	return false;
}

/** Append entry to the in-memory set and persist to disk immediately. **/
void IgnoreList::add(const std::string& entry) {
	auto low = toLower(entry);
	if (entries_.count(low)) return;
	entries_.insert(low);

	std::ofstream f(path_, std::ios::app);
	if (f) {
		f << entry << "\n";
		LOG_INFO("Added to ignore list: \"" << entry << "\"");
	}
}

// Default ignore.txt — written once on first run
void IgnoreList::writeDefaults() {
	std::ofstream f(path_);
	if (!f) return;
	f <<
	"# vn-discord-rpc — ignore list\n"
	"# One entry per line. Case-insensitive substring match.\n"
	"# If a detected game name contains any entry here it is silently skipped.\n"
	"# Lines starting with # are comments.\n"
	"# Edit while the daemon is running — reloaded automatically.\n"
	"#\n"
	"# ── Steam / Proton runtime infrastructure ────────────────────────────\n"
	"GE-Proton\n"
	"Proton Experimental\n"
	"Proton Hotfix\n"
	"SteamLinuxRuntime\n"
	"Steam Linux Runtime\n"
	"pressure-vessel\n"
	"steam-runtime\n"
	"#\n"
	"# ── Wine / compatibility layer helpers ───────────────────────────────\n"
	"umu-run\n"
	"umu-shim\n"
	"wine\n"
	"wineserver\n"
	"winemenubuilder\n"
	"#\n"
	"# ── Launchers & wrappers ─────────────────────────────────────────────\n"
	"lutris\n"
	"heroic\n"
	"bottles\n"
	"gamescope\n"
	"gamescopereaper\n"
	"steam\n"
	"EdgeUpdate\n"
	"update\n"
	"#\n"
	"# ── System processes that occasionally show up ────────────────────────\n"
	"python\n"
	"python3\n"
	"bash\n"
	"sh\n"
	"#\n"
	"# ── Add your own below ───────────────────────────────────────────────\n";

	LOG_INFO("Created default ignore list at " << path_.string());
}
