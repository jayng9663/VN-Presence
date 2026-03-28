#include "logger.hpp"
#include "process_scanner.hpp"
#include "steam_detector.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ─── Internal helpers ───
namespace {

	std::string trim(const std::string& s) {
		auto a = s.find_first_not_of(" \t\n\r-_");
		auto b = s.find_last_not_of(" \t\n\r-_");
		return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
	}

	std::vector<std::string> splitCmdline(const std::string& raw) {
		std::vector<std::string> args;
		std::string cur;
		for (char c : raw) {
			if (c == '\0') {
				if (!cur.empty()) { args.push_back(cur); cur.clear(); }
			} else {
				cur += c;
			}
		}
		if (!cur.empty()) args.push_back(cur);
		return args;
	}

	bool endsWith(const std::string& s, const std::string& sfx) {
		if (sfx.size() > s.size()) return false;
		return std::equal(sfx.rbegin(), sfx.rend(), s.rbegin(),
				[](char a, char b){ return ::tolower(a) == ::tolower(b); });
	}

} // anonymous namespace

// ─── ProcessScanner::readCmdline ───
std::vector<std::string> ProcessScanner::readCmdline(int pid)
{
	std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
	if (!f) return {};
	std::string raw((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());
	return splitCmdline(raw);
}

// ─────────────────────────────────────────────────────────────────────────────
// ProcessScanner::scan
//
// Detection strategies in priority order:
//   1. lutris       — python3 .../lutris-wrapper <name> <digits>
//   2. steam-appid  — SteamAppId env var → appmanifest_<id>.acf "name" field
// ─────────────────────────────────────────────────────────────────────────────
std::vector<VnProcess> ProcessScanner::scan()
{
	std::vector<VnProcess> results;

	std::error_code ec;
	for (const auto& entry : fs::directory_iterator("/proc", ec)) {
		if (ec) break;

		const std::string name = entry.path().filename().string();
		if (name.empty() || !std::isdigit((unsigned char)name[0])) continue;

		int pid = 0;
		try { pid = std::stoi(name); } catch (...) { continue; }

		auto args = readCmdline(pid);
		if (args.empty()) continue;

		// ── 1. Lutris wrapper ──
		for (size_t i = 0; i < args.size(); ++i) {
			if (!endsWith(args[i], "lutris-wrapper")) continue;

			std::string gameName;
			for (size_t j = i + 1; j < args.size(); ++j) {
				bool allDigits = !args[j].empty() &&
					std::all_of(args[j].begin(), args[j].end(), ::isdigit);
				if (allDigits) break;
				if (!gameName.empty()) gameName += ' ';
				gameName += args[j];
			}
			gameName = trim(gameName);
			if (gameName.empty()) continue;

			std::string exePath;
			for (auto& a : args)
				if (endsWith(a, ".exe")) { exePath = a; break; }

			LOG_DEBUG("Lutris match: pid=" << pid
					<< "  name=\"" << gameName << "\""
					<< "  exe=" << exePath);
			results.push_back({ gameName, exePath, pid, "lutris" });
			goto next_pid;
		}

next_pid:;
	}

	// ── 2. Steam AppID ──
	// Always checked regardless of Lutris — both sources are returned so
	// the multi-candidate resolver in main can pick whichever is a VN.
	auto steamName = SteamDetector::getRunningGameName();
	if (steamName) {
		// Deduplicate: skip if a Lutris entry already carries the same name.
		bool alreadyPresent = std::any_of(results.begin(), results.end(),
				[&](const VnProcess& p){ return p.gameName == *steamName; });
		if (!alreadyPresent) {
			LOG_DEBUG("Steam-appid match: name=\"" << *steamName << "\"");
			results.push_back({ *steamName, "", 0, "steam-appid" });
		}
	}

	return results;
}

/** Return the highest-priority VN process, or nullopt if none running. **/
std::optional<VnProcess> ProcessScanner::findFirst()
{
	auto v = scan();
	if (v.empty()) {
		LOG_DEBUG("ProcessScanner: no VN processes found");
		return std::nullopt;
	}
	LOG_DEBUG("ProcessScanner found " << v.size() << " candidate(s):");
	for (const auto& p : v)
		LOG_DEBUG("  [" << p.source << "] pid=" << p.pid
				<< "  name=\"" << p.gameName << "\""
				<< "  exe=" << p.exePath);
	return v.front();
}
