#include "logger.hpp"
#include "steam_detector.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

// ─── Helpers ───
namespace {

	std::string getEnv(const char* k) {
		const char* v = std::getenv(k);
		return v ? v : "";
	}

	// Read an entire file into a string.
	std::string readFile(const fs::path& p) {
		std::ifstream f(p, std::ios::binary);
		if (!f) return {};
		std::ostringstream ss;
		ss << f.rdbuf();  // bulk read — much faster than istreambuf_iterator
		return std::move(ss).str();
	}

	// Split a NUL-separated /proc/<pid>/environ blob into KEY=VALUE pairs.
	// Returns the value for `key`, or "" if not found.
	std::string environValue(const std::string& blob, const std::string& key) {
		const std::string prefix = key + "=";
		size_t i = 0;
		while (i < blob.size()) {
			size_t end = blob.find('\0', i);
			if (end == std::string::npos) end = blob.size();
			// Compare in place — no per-entry copy
			if (end - i >= prefix.size() && blob.compare(i, prefix.size(), prefix) == 0)
				return blob.substr(i + prefix.size(), end - i - prefix.size());
			i = end + 1;
		}
		return {};
	}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// SteamDetector::parseVdfField
//
// Finds the first occurrence of:   "key"   "value"
// in Valve's KeyValues / ACF format and returns value.
// ─────────────────────────────────────────────────────────────────────────────
std::string SteamDetector::parseVdfField(const std::string& content,
		const std::string& key)
{
	// Pattern: "key"\t*"value"
	std::string needle = "\"" + key + "\"";
	size_t pos = content.find(needle);
	while (pos != std::string::npos) {
		size_t after = pos + needle.size();
		// Skip whitespace / tabs between key and value
		while (after < content.size() &&
				(content[after] == ' ' || content[after] == '\t'))
			++after;
		if (after < content.size() && content[after] == '"') {
			++after; // skip opening quote
			size_t end = content.find('"', after);
			if (end != std::string::npos)
				return content.substr(after, end - after);
		}
		pos = content.find(needle, pos + 1);
	}
	return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// SteamDetector::libraryPaths
//
// Returns all Steam library root paths, e.g.:
//   ~/.local/share/Steam/steamapps
//   /mnt/8THDD/SteamLibrary/steamapps
//
// Reads libraryfolders.vdf for custom paths.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<fs::path> SteamDetector::libraryPaths()
{
	// Library list rarely changes — read libraryfolders.vdf once and reuse it
	static std::vector<fs::path> cached;
	if (!cached.empty()) return cached;

	std::vector<fs::path> paths;

	// Default Steam install location
	std::string home = getEnv("HOME");
	fs::path defaultLib = fs::path(home) / ".local/share/Steam/steamapps";
	if (fs::exists(defaultLib))
		paths.push_back(defaultLib);

	// Parse libraryfolders.vdf for additional libraries
	fs::path vdfPath = defaultLib / "libraryfolders.vdf";
	std::string vdf = readFile(vdfPath);
	if (vdf.empty()) {
		// Try alternate location
		vdfPath = fs::path(home) / ".steam/steam/steamapps/libraryfolders.vdf";
		vdf = readFile(vdfPath);
	}

	if (!vdf.empty()) {
		// Each library block has a "path" field
		size_t pos = 0;
		std::string needle = "\"path\"";
		while ((pos = vdf.find(needle, pos)) != std::string::npos) {
			pos += needle.size();
			// Skip whitespace
			while (pos < vdf.size() &&
					(vdf[pos] == ' ' || vdf[pos] == '\t')) ++pos;
			if (pos < vdf.size() && vdf[pos] == '"') {
				++pos;
				size_t end = vdf.find('"', pos);
				if (end != std::string::npos) {
					fs::path lib = fs::path(vdf.substr(pos, end - pos)) / "steamapps";
					if (fs::exists(lib) &&
							std::find(paths.begin(), paths.end(), lib) == paths.end()) {
						paths.push_back(lib);
						LOG_DEBUG("Steam library: " << lib.string());
					}
				}
			}
		}
	}

	cached = paths;
	return paths;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: scan /proc for every running Steam game process.
// Returns one {appId, pid} per distinct AppID (first PID seen for that AppID),
// or an empty vector if nothing is running.
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::pair<int,int>> findRunningAppsAndPids()
{
	std::vector<std::pair<int,int>> found;

	// Steam infrastructure AppIDs — skip these
	static const std::vector<int> infraIds = {
		1070560,  // Steam Linux Runtime
		1391110,  // Steam Linux Runtime - Soldier
		1628350,  // Steam Linux Runtime - Sniper
		1493710,  // Proton Experimental
		1887720,  // Proton Hotfix
		2230260,  // Proton 8.0
		2348590,  // Proton 9.0
		3125380,  // Proton 10.0
		228980,   // Steamworks Common Redistributables
	};

	std::error_code ec;
	for (const auto& entry : fs::directory_iterator("/proc", ec)) {
		if (ec) break;
		const std::string name = entry.path().filename().string();
		if (name.empty() || !std::isdigit((unsigned char)name[0])) continue;

		std::string blob = readFile(entry.path() / "environ");
		if (blob.empty()) continue;

		std::string val = environValue(blob, "SteamAppId");
		if (val.empty() || val == "0") continue;

		int appId = 0;
		try { appId = std::stoi(val); } catch (...) { continue; }
		if (appId <= 0) continue;

		// Skip infrastructure
		if (std::find(infraIds.begin(), infraIds.end(), appId) != infraIds.end()) {
			LOG_DEBUG("Steam: skipping infrastructure AppId=" << appId);
			continue;
		}

		// A game usually spans several processes (reaper, wine, the exe) that
		// all carry the same SteamAppId — keep only the first one seen.
		bool seen = std::any_of(found.begin(), found.end(),
				[&](const std::pair<int,int>& f){ return f.first == appId; });
		if (seen) continue;

		int pid = 0;
		try { pid = std::stoi(name); } catch (...) {}

		LOG_DEBUG("Steam: found AppId=" << appId << "  pid=" << pid);
		found.emplace_back(appId, pid);
	}
	return found;
}

// ─── SteamDetector::getRunningGames ───
std::vector<SteamGame> SteamDetector::getRunningGames()
{
	std::vector<SteamGame> games;

	auto running = findRunningAppsAndPids();
	if (running.empty()) return games;

	auto libs = libraryPaths();
	if (libs.empty()) {
		LOG_DEBUG("Steam: no library paths found");
		return games;
	}

	// Log each AppId → name mapping once, not on every poll
	static std::map<int, std::string> loggedNames;

	for (const auto& [appId, pid] : running) {
		std::string acfName = "appmanifest_" + std::to_string(appId) + ".acf";
		std::string name;

		for (const auto& lib : libs) {
			std::string acf = readFile(lib / acfName);
			if (acf.empty()) continue;
			name = parseVdfField(acf, "name");
			if (!name.empty()) break;
		}

		if (name.empty()) {
			LOG_WARN("Steam: AppId=" << appId << " found in environ but no ACF in any library");
			continue;
		}

		auto it = loggedNames.find(appId);
		if (it == loggedNames.end() || it->second != name) {
			LOG_INFO("Steam: AppId=" << appId << "  name=\"" << name << "\"");
			loggedNames[appId] = name;
		}

		games.push_back({ appId, pid, name });
	}

	return games;
}

// Returns a view into `s` — no copy of the (possibly multi-MB) block.
static std::string_view extractBlock3(std::string_view s, size_t openBrace)
{
	int depth = 1;
	size_t i  = openBrace + 1;
	while (i < s.size() && depth > 0) {
		if      (s[i] == '{') ++depth;
		else if (s[i] == '}') --depth;
		++i;
	}
	return s.substr(openBrace + 1, i - openBrace - 2);
}

int64_t SteamDetector::getPlaytimeMinutes(int appId)
{
	if (appId <= 0) return 0;

	std::string home = getEnv("HOME");
	fs::path userdataDir = fs::path(home) / ".local/share/Steam/userdata";

	std::error_code ec;
	if (!fs::exists(userdataDir, ec)) return 0;

	std::string appNeedle = "\"" + std::to_string(appId) + "\"";

	// All known key names Steam has used across versions (minutes)
	static const std::vector<std::string> playtimeKeys = {
		"Playtime",           // current Steam client (as seen in the wild)
		"playtime_forever",   // older Steam client
		"playtime2_forever",  // transitional
		"PlaytimeForever",
		"Playtime2Forever",
	};

	for (const auto& userEntry : fs::directory_iterator(userdataDir, ec)) {
		if (ec) break;
		fs::path vdfPath = userEntry.path() / "config" / "localconfig.vdf";
		std::string vdf = readFile(vdfPath);
		if (vdf.empty()) continue;

		LOG_DEBUG("Steam playtime: reading " << vdfPath.string()
				<< " (" << vdf.size() << " bytes)");

		size_t appsPos = 0;
		while ((appsPos = vdf.find("\"apps\"", appsPos)) != std::string::npos) {
			size_t appsOpen = vdf.find('{', appsPos + 6);
			if (appsOpen == std::string::npos) { ++appsPos; continue; }

			std::string_view appsBlock = extractBlock3(vdf, appsOpen);

			size_t apos = 0;
			while ((apos = appsBlock.find(appNeedle, apos)) != std::string_view::npos) {
				bool preOk  = (apos == 0 || !std::isdigit((unsigned char)appsBlock[apos - 1]));
				size_t aend = apos + appNeedle.size();
				bool postOk = (aend >= appsBlock.size() || !std::isdigit((unsigned char)appsBlock[aend]));

				if (preOk && postOk) {
					size_t appOpen = appsBlock.find('{', aend);
					if (appOpen == std::string_view::npos) { ++apos; continue; }

					// Single app block is small — copy it for parseVdfField
					std::string appBlock(extractBlock3(appsBlock, appOpen));

					for (const auto& key : playtimeKeys) {
						std::string pt = parseVdfField(appBlock, key);
						if (!pt.empty()) {
							try {
								int64_t minutes = std::stoll(pt);
								if (minutes > 0) {
									LOG_DEBUG("Steam playtime: AppId=" << appId
											<< "  key=\"" << key << "\""
											<< "  minutes=" << minutes
											<< "  (" << (minutes/60) << "h "
											<< (minutes%60) << "m)");
									return minutes;
								}
							} catch (...) {}
						}
					}
				}
				apos += appNeedle.size();
			}
			appsPos += 6;
		}
	}

	LOG_DEBUG("Steam: no playtime found for AppId=" << appId);
	return 0;
}
