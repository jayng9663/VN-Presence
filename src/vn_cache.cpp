#include "logger.hpp"
#include "vn_cache.hpp"
#include "config.hpp"

#include <sqlite3.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

// ── Helpers ──
namespace {

	int64_t nowUnix() {
		return std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
	}

	double toDouble(const std::string& s, double def = 0.0) {
		if (s.empty()) return def;
		try { return std::stod(s); } catch (...) { return def; }
	}

} // anonymous namespace

// ── CacheEntry::toVnInfo ──
VnInfo CacheEntry::toVnInfo() const {
	VnInfo v;
	v.id             = vndb_id;
	v.title          = title;
	v.alt_title      = alt_title;
	v.image_url      = image_url;
	v.image_sexual   = image_sexual;
	v.image_violence = image_violence;
	v.image_votecount = image_votecount;
	v.rating         = rating;
	v.released       = released;
	v.vndb_url       = "https://vndb.org/" + vndb_id;
	return v;
}

// ── Construction ──
VnCache::VnCache(std::filesystem::path path) : path_(std::move(path)) {
	std::error_code ec;
	std::filesystem::create_directories(path_.parent_path(), ec);
	if (config::CACHE_USE_DB)
		LOG_DEBUG("Cache backend: SQLite  path=" << defaultDbPath().string());
	else
		LOG_DEBUG("Cache backend: CSV  path=" << path_.string());
	reload();
}

std::filesystem::path VnCache::defaultPath() {
	const char* xdg = std::getenv("XDG_CONFIG_HOME");
	const char* home = std::getenv("HOME");
	std::filesystem::path base = xdg ? std::filesystem::path(xdg)
		: (std::filesystem::path(home ? home : "") / ".config");
	return base / "vn-discord-rpc" / "cache.csv";
}

std::filesystem::path VnCache::defaultDbPath() {
	const char* xdg = std::getenv("XDG_CONFIG_HOME");
	const char* home = std::getenv("HOME");
	std::filesystem::path base = xdg ? std::filesystem::path(xdg)
		: (std::filesystem::path(home ? home : "") / ".config");
	return base / "vn-discord-rpc" / "cache.db";
}

// ── SQLite helpers ──
namespace {

	static const char* DB_SCHEMA = R"(
CREATE TABLE IF NOT EXISTS cache (
    key            TEXT PRIMARY KEY,
    alias          TEXT DEFAULT '',
    vndb_id        TEXT DEFAULT '',
    title          TEXT DEFAULT '',
    alt_title      TEXT DEFAULT '',
    image_url      TEXT DEFAULT '',
    image_sexual   REAL DEFAULT 0,
    image_violence REAL DEFAULT 0,
	image_votecount REAL DEFAULT 0,
    rating         REAL DEFAULT 0,
    released       TEXT DEFAULT '',
    cached_at      INTEGER DEFAULT 0
);
)";

	void dbEnsureSchema(sqlite3* db) {
		char* err = nullptr;
		sqlite3_exec(db, DB_SCHEMA, nullptr, nullptr, &err);
		if (err) { LOG_ERR("SQLite schema error: " << err); sqlite3_free(err); }
	}

	sqlite3* dbOpen(const std::filesystem::path& path) {
		sqlite3* db = nullptr;
		if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
			LOG_ERR("Cannot open SQLite cache: " << sqlite3_errmsg(db));
			sqlite3_close(db);
			return nullptr;
		}
		dbEnsureSchema(db);
		return db;
	}

	// Read all rows from DB into the entries map
	void dbLoad(sqlite3* db, std::unordered_map<std::string, CacheEntry>& entries) {
		const char* sql = "SELECT key,alias,vndb_id,title,alt_title,image_url,"
			"image_sexual,image_violence,rating,released,cached_at FROM cache";
		sqlite3_stmt* stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
		entries.clear();
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			auto col = [&](int i) -> std::string {
				auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
				return t ? t : "";
			};
			CacheEntry e;
			e.key            = col(0);
			e.alias          = col(1);
			e.vndb_id        = col(2);
			e.title          = col(3);
			e.alt_title      = col(4);
			e.image_url      = col(5);
			e.image_sexual   = sqlite3_column_double(stmt, 6);
			e.image_violence = sqlite3_column_double(stmt, 7);
			e.image_votecount = sqlite3_column_int(stmt, 8);
			e.rating         = sqlite3_column_double(stmt, 9);
			e.released       = col(8);
			e.cached_at      = sqlite3_column_int64(stmt, 11);
			if (!e.key.empty()) entries[e.key] = std::move(e);
		}
		sqlite3_finalize(stmt);
	}

	// Upsert one entry
	void dbUpsert(sqlite3* db, const CacheEntry& e) {
		const char* sql = R"(
INSERT INTO cache(key,alias,vndb_id,title,alt_title,image_url,
                  image_sexual,image_violence,image_votecount,
				  rating,released,cached_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(key) DO UPDATE SET
  alias=excluded.alias, vndb_id=excluded.vndb_id, title=excluded.title,
  alt_title=excluded.alt_title, image_url=excluded.image_url,
  image_sexual=excluded.image_sexual, image_violence=excluded.image_violence,
  image_votecount=excluded.image_votecount, rating=excluded.rating,
  released=excluded.released, cached_at=excluded.cached_at;
)";
		sqlite3_stmt* stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
			LOG_ERR("SQLite upsert prepare: " << sqlite3_errmsg(db)); return;
		}
		auto bind = [&](int i, const std::string& s) {
			sqlite3_bind_text(stmt, i, s.c_str(), -1, SQLITE_TRANSIENT);
		};
		bind(1, e.key);  bind(2, e.alias);    bind(3, e.vndb_id);
		bind(4, e.title); bind(5, e.alt_title); bind(6, e.image_url);
		sqlite3_bind_double(stmt, 7, e.image_sexual);
		sqlite3_bind_double(stmt, 8, e.image_violence);
		sqlite3_bind_int(stmt, 9, e.image_votecount);
		sqlite3_bind_double(stmt, 10, e.rating);
		bind(11, e.released);
		sqlite3_bind_int64(stmt, 12, e.cached_at);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

} // anonymous namespace

// ── reload — re-read the CSV if it has changed on disk ──
/** Re-read the CSV from disk if its mtime has changed. **/
void VnCache::reload() {
	if (config::CACHE_USE_DB) {
		// SQLite handles its own consistency — just re-read every time
		sqlite3* db = dbOpen(defaultDbPath());
		if (!db) return;
		dbLoad(db, entries_);
		sqlite3_close(db);
		LOG_DEBUG("SQLite cache reloaded: " << entries_.size() << " entries");
		return;
	}

	std::error_code ec;
	if (!std::filesystem::exists(path_, ec)) return;

	auto mtime = std::filesystem::last_write_time(path_, ec);
	if (ec || mtime == lastMtime_) {
		LOG_DEBUG("Cache file unchanged — skip reload");
		return;
	}
	LOG_DEBUG("Cache file changed — reloading from disk");

	std::ifstream f(path_);
	if (!f) return;

	std::string content((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());
	entries_.clear();
	parse(content);
	lastMtime_ = mtime;
	LOG_INFO("Cache loaded " << entries_.size() << " entries from " << path_.string());
}

// ── CSV parsing ──

// Split one CSV row respecting double-quoted fields.
std::vector<std::string> VnCache::splitCsvRow(const std::string& line) {
	std::vector<std::string> fields;
	std::string field;
	bool inQuotes = false;

	for (size_t i = 0; i < line.size(); ++i) {
		char c = line[i];
		if (inQuotes) {
			if (c == '"') {
				if (i + 1 < line.size() && line[i+1] == '"') {
					field += '"'; ++i;  // escaped quote
				} else {
					inQuotes = false;
				}
			} else {
				field += c;
			}
		} else {
			if (c == '"') {
				inQuotes = true;
			} else if (c == ',') {
				fields.push_back(field);
				field.clear();
			} else {
				field += c;
			}
		}
	}
	fields.push_back(field);
	return fields;
}

void VnCache::parse(const std::string& csv) {
	std::istringstream ss(csv);
	std::string line;

	// Expected column order (must match serialise()):
	// key,alias,vndb_id,title,image_url,image_sexual,image_violence,rating,released,cached_at
	while (std::getline(ss, line)) {
		// Strip carriage return
		if (!line.empty() && line.back() == '\r') line.pop_back();
		// Skip blank lines and comment lines
		if (line.empty() || line[0] == '#') continue;
		// Skip header row (with or without UTF-8 BOM prefix \xEF\xBB\xBF)
		std::string stripped = line;
		if (stripped.size() >= 3 &&
				(unsigned char)stripped[0] == 0xEF &&
				(unsigned char)stripped[1] == 0xBB &&
				(unsigned char)stripped[2] == 0xBF)
			stripped = stripped.substr(3);
		if (stripped.rfind("key,", 0) == 0) continue;

		auto f = splitCsvRow(line);
		// Pad to 11 fields
		while (f.size() < 12) f.emplace_back();

		CacheEntry e;
		e.key            = f[0];
		e.alias          = f[1];
		e.vndb_id        = f[2];
		e.title          = f[3];
		e.alt_title      = f[4];
		e.image_url      = f[5];
		e.image_sexual   = toDouble(f[6]);
		e.image_violence = toDouble(f[7]);
		e.image_votecount = stoi(f[8]);
		e.rating         = toDouble(f[9]);
		e.released       = f[10];
		try { e.cached_at = f[11].empty() ? 0 : std::stoll(f[11]); } catch (...) {}

		if (!e.key.empty()) {
			LOG_DEBUG("Cache row: key=\"" << e.key
					<< "\"  alias=\"" << e.alias
					<< "\"  vndb_id=" << e.vndb_id
					<< "  title=\"" << e.title << "\""
					<< "  alt=\"" << e.alt_title << "\"");
			entries_[e.key] = std::move(e);
		}
	}
}

// ── CSV serialisation ──
std::string VnCache::quoteCsvField(const std::string& s) {
	// Quote if contains comma, quote, or newline
	if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
	std::string out = "\"";
	for (char c : s) {
		if (c == '"') out += "\"\"";
		else          out += c;
	}
	out += '"';
	return out;
}

std::string VnCache::serialise(const CacheEntry& e) {
	// key,alias,vndb_id,title,image_url,image_sexual,image_violence,rating,released,cached_at
	auto q = quoteCsvField;
	std::ostringstream ss;
	ss << q(e.key)          << ','
		<< q(e.alias)        << ','
		<< q(e.vndb_id)      << ','
		<< q(e.title)        << ','
		<< q(e.alt_title)    << ','
		<< q(e.image_url)    << ','
		<< e.image_sexual    << ','
		<< e.image_violence  << ','
		<< e.image_votecount << ','
		<< e.rating          << ','
		<< q(e.released)     << ','
		<< e.cached_at;
	return ss.str();
}

// ── save ──
/** Write all cache entries to disk with a UTF-8 BOM for spreadsheet compat. **/
void VnCache::save() const {
	if (config::CACHE_USE_DB) {
		sqlite3* db = dbOpen(defaultDbPath());
		if (!db) return;
		sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
		for (const auto& [key, e] : entries_) dbUpsert(db, e);
		sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
		sqlite3_close(db);
		LOG_DEBUG("SQLite cache saved: " << entries_.size() << " entries");
		return;
	}

	std::error_code ec;
	std::filesystem::create_directories(path_.parent_path(), ec);

	// Write a README next to the CSV the first time we create the directory
	auto readmePath = path_.parent_path() / "README.txt";
	if (!std::filesystem::exists(readmePath, ec)) {
		std::ofstream readme(readmePath);
		if (readme) {
			readme <<
				"vn-discord-rpc  —  cache.csv usage guide\n"
				"==========================================\n"
				"\n"
				"Open cache.csv in LibreOffice Calc, Excel, or any text editor.\n"
				"The daemon reloads it automatically whenever you save changes.\n"
				"\n"
				"COLUMNS\n"
				"  key            The game name detected from the process\n"
				"  alias          Redirect this key to a different search term\n"
				"  vndb_id        VNDB entry id (e.g. v562), SKIP, or empty\n"
				"  title          VNDB display title, romanized (auto-filled)\n"
				"  alt_title      Original script title, e.g. Japanese (auto-filled)\n"
				"  image_url      Cover image URL   (auto-filled)\n"
				"  image_sexual   VNDB sexual rating 0-3  (auto-filled)\n"
				"  image_violence VNDB violence rating 0-3 (auto-filled)\n"
				"  image_votecount VNDB image vote count (auto-filled)\n"
				"  rating         VNDB rating 0-100 (auto-filled)\n"
				"  released       Release date      (auto-filled)\n"
				"  cached_at      Unix timestamp of last update (auto-filled)\n"
				"\n"
				"EXAMPLES\n"
				"\n"
				"  Alias (fix wrong/garbled detection):\n"
				"    key            = CRACK≡TRICK!\n"
				"    alias          = Crack Trick\n"
				"    (leave all other columns empty)\n"
				"\n"
				"  Skip (suppress presence for a title):\n"
				"    key            = SomeLauncher\n"
				"    vndb_id        = SKIP\n"
				"    (leave all other columns empty)\n"
				"\n"
				"  Hard-link (bypass VNDB query, point directly to an entry):\n"
				"    key            = My VN Title\n"
				"    vndb_id        = v1234\n"
				"    (title/image/etc will be auto-filled on next detection)\n"
				"\n"
				"  Clear a no-match (force a retry next time the game is detected):\n"
				"    Delete the row for that key, or clear its vndb_id field.\n"
				"\n"
				"NOTE: Do not change the header row or add extra columns.\n";
		}
	}

	std::ofstream f(path_);
	if (!f) {
		LOG_ERR("Cannot write cache to " << path_.string());
		return;
	}
	LOG_DEBUG("Saving " << entries_.size() << " cache entries to " << path_.string());

	// UTF-8 BOM — makes LibreOffice Calc / Excel auto-detect encoding (required
	// for Japanese/CJK titles to display correctly without an import dialog).
	f << "\xEF\xBB\xBF";

	// Plain header row — no # comments inside the CSV so every spreadsheet
	// app opens the file cleanly without treating comments as data rows.
	f << "key,alias,vndb_id,title,alt_title,image_url,"
		"image_sexual,image_violence,rating,released,cached_at\n";

	for (const auto& [key, e] : entries_)
		f << serialise(e) << '\n';
}

// ── lookup ──
/**
 * Look up a key, normalising dashes before comparison.
 * Returns the entry (possibly with alias/vndb_id filled) or nullopt on miss.
 **/
std::optional<CacheEntry> VnCache::lookup(const std::string& key) {
	reload();

	LOG_DEBUG("Cache lookup: \"" << key << "\"  total_entries=" << entries_.size());
	auto it = entries_.find(key);
	if (it == entries_.end()) {
		LOG_DEBUG("Cache miss for \"" << key << "\"");
		return std::nullopt;
	}

	const CacheEntry& e = it->second;

	// If this row has an alias but is already fully populated (vndb_id filled),
	// return it directly — single row, no second lookup needed.
	// Only redirect when vndb_id is still empty (first-time alias resolution).
	if (!e.alias.empty() && !e.hasMatch() && !e.isSkip()) {
		auto it2 = entries_.find(e.alias);
		if (it2 != entries_.end()) return it2->second;
		// alias target row doesn't exist yet — return as-is so main.cpp
		// can query VNDB using e.alias as the search term
	}

	return e;
}

// ── store / storeNoMatch / storeAlias ──
void VnCache::store(const std::string& key, const VnInfo& vn) {
	CacheEntry e;
	e.key            = key;
	e.vndb_id        = vn.id;
	e.title          = vn.title;
	e.alt_title      = vn.alt_title;
	e.image_url      = vn.image_url;
	e.image_sexual   = vn.image_sexual;
	e.image_violence = vn.image_violence;
	e.image_votecount = vn.image_votecount;
	e.rating         = vn.rating;
	e.released       = vn.released;
	e.cached_at      = nowUnix();
	entries_[key]    = e;
	save();
}

void VnCache::storeNoMatch(const std::string& key) {
	// Store empty vndb_id — no match, but we tried
	CacheEntry e;
	e.key       = key;
	e.cached_at = nowUnix();
	entries_[key] = e;
	save();
	LOG_DEBUG("Cache stored no-match for \"" << key << "\"");
}

void VnCache::storeAlias(const std::string& key, const std::string& aliasTarget) {
	CacheEntry e;
	e.key       = key;
	e.alias     = aliasTarget;
	e.cached_at = nowUnix();
	entries_[key] = e;
	save();
}

void VnCache::storeAliasFilled(const std::string& key, const std::string& alias, const VnInfo& vn) {
	// Write a fully-populated row but keep the alias column so users can
	// see at a glance which rows are redirects vs. directly detected.
	CacheEntry e;
	e.key            = key;
	e.alias          = alias;        // preserved — shows the redirect in the CSV
	e.vndb_id        = vn.id;
	e.title          = vn.title;
	e.alt_title      = vn.alt_title;
	e.image_url      = vn.image_url;
	e.image_sexual   = vn.image_sexual;
	e.image_violence = vn.image_violence;
	e.image_votecount = vn.image_votecount;
	e.rating         = vn.rating;
	e.released       = vn.released;
	e.cached_at      = nowUnix();
	entries_[key]    = e;
	save();
}
