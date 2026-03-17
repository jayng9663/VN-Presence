#include "logger.hpp"
#include "lutris_db.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstring>

// ─── Helpers ───
namespace {

	std::string toLower(std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), ::tolower);
		return s;
	}

} // namespace

// ─── defaultDbPath ───
std::filesystem::path LutrisDB::defaultDbPath() {
	const char* home = std::getenv("HOME");
	return std::filesystem::path(home ? home : "") /
		".local/share/lutris/pga.db";
}

// ─── getPlaytime ───

// Normalize both typographic and full-width dashes/hyphens to plain hyphen
// so "サクラノ詩－..." matches "サクラノ詩 -..." in Lutris DB
static std::string normalizeDashes(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		unsigned char c0 = s[i];
		// En/em/figure/horizontal bar: U+2012-U+2015 → E2 80 {92-95}
		if (c0 == 0xE2 && i+2 < s.size() &&
				(unsigned char)s[i+1] == 0x80 &&
				(unsigned char)s[i+2] >= 0x92 && (unsigned char)s[i+2] <= 0x95) {
			out += '-'; i += 3;
		}
		// Full-width hyphen-minus U+FF0D → EF BC 8D
		else if (c0 == 0xEF && i+2 < s.size() &&
				(unsigned char)s[i+1] == 0xBC &&
				(unsigned char)s[i+2] == 0x8D) {
			out += '-'; i += 3;
		}
		// Full-width space U+3000 → E3 80 80  →  keep as regular space
		else if (c0 == 0xE3 && i+2 < s.size() &&
				(unsigned char)s[i+1] == 0x80 &&
				(unsigned char)s[i+2] == 0x80) {
			out += ' '; i += 3;
		}
		else {
			out += (char)c0; ++i;
		}
	}
	return out;
}

/**
 * Open ~/.local/share/lutris/pga.db and find the playtime for gameName.
 * Lutris stores playtime as REAL hours; we return seconds.
 * Matching is case-insensitive substring after dash normalisation.
 **/
std::optional<int64_t> LutrisDB::getPlaytime(const std::string& gameName)
{
	auto dbPath = defaultDbPath();

	std::error_code ec;
	if (!std::filesystem::exists(dbPath, ec)) {
		LOG_DEBUG("Lutris DB not found at " << dbPath.string());
		return std::nullopt;
	}

	sqlite3* db = nullptr;
	if (sqlite3_open_v2(dbPath.c_str(), &db,
				SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
		LOG_WARN("Cannot open Lutris DB: " << sqlite3_errmsg(db));
		sqlite3_close(db);
		return std::nullopt;
	}

	// SELECT all names + playtime, then do case-insensitive substring match
	// in C++ so we handle Japanese/Unicode names that SQLite LIKE won't match.
	const char* sql = "SELECT name, playtime FROM games WHERE playtime > 0";
	sqlite3_stmt* stmt = nullptr;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		LOG_WARN("Lutris DB prepare failed: " << sqlite3_errmsg(db));
		sqlite3_close(db);
		return std::nullopt;
	}

	// queryLow computed per-row now (after normalization)
	std::optional<int64_t> result;
	int64_t bestPlaytime = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char* nameRaw = reinterpret_cast<const char*>(
				sqlite3_column_text(stmt, 0));
		double  playtimeHours = sqlite3_column_double(stmt, 1);
		int64_t playtime      = static_cast<int64_t>(playtimeHours * 3600.0);
		if (!nameRaw) continue;

		std::string nameLow = toLower(normalizeDashes(nameRaw));
		std::string qLow    = toLower(normalizeDashes(gameName));

		// Match if either is a substring of the other
		bool match = nameLow.find(qLow) != std::string::npos
			|| qLow.find(nameLow) != std::string::npos;

		if (match && playtime > bestPlaytime) {
			bestPlaytime = playtime;
			result       = playtime;
			LOG_DEBUG("Lutris playtime match: \"" << nameRaw
					<< "\"  hours=" << playtimeHours
					<< "  seconds=" << playtime);
		}
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);

	if (!result)
		LOG_DEBUG("Lutris: no playtime found for \"" << gameName << "\"");

	return result;
}

// ─── formatPlaytime ───
/** Convert seconds to "Xh Ym" (or "Ym" when hours == 0). **/
std::string LutrisDB::formatPlaytime(int64_t seconds)
{
	if (seconds <= 0) return "";

	int64_t hours   = seconds / 3600;
	int64_t minutes = (seconds % 3600) / 60;

	char buf[32];
	if (hours > 0)
		std::snprintf(buf, sizeof(buf), "%ldh %ldm", (long)hours, (long)minutes);
	else
		std::snprintf(buf, sizeof(buf), "%ldm", (long)minutes);

	return buf;
}
