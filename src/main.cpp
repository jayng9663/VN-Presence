#include "config.hpp"
#include "logger.hpp"
#include "process_scanner.hpp"
#include "vndb_client.hpp"
#include "vn_cache.hpp"
#include "ignore_list.hpp"
#include "rpc_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

static std::atomic<bool> g_running{true};
static void signalHandler(int) { g_running = false; }

static void sleepInterruptible(std::chrono::seconds d) {
	auto end = std::chrono::steady_clock::now() + d;
	while (g_running && std::chrono::steady_clock::now() < end)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

struct AppState {
	std::string lastSearchTitle;    ///< game-name key of the VN currently shown
	std::string lastDetectSource;   ///< source of the VN currently shown
	std::string candidateSetKey;    ///< debounce key: sorted candidate names joined by '|'
	int         stablePollCount = 0;
	bool        hasPresence     = false;
};

/** Print the startup banner and warn if APP_ID is unset. **/
static void printBanner()
{
	std::cout << R"(
  ╭──────────────────────────────────────╮
  │        vn-discord-rpc  v1.0.2        │
  │  Visual Novel Discord Rich Presence  │
  ╰──────────────────────────────────────╯
)" << std::flush;
	if (std::string(config::DISCORD_APP_ID) == "YOUR_DISCORD_APP_ID_HERE") LOG_WARN("DISCORD_APP_ID not set in src/config.hpp! ""Get one at: https://discord.com/developers/applications");
}

/**
 * Build a stable debounce key from a list of candidate processes.
 * Names are sorted so the key is order-independent.
 **/
static std::string makeCandidateKey(const std::vector<VnProcess>& procs)
{
	std::vector<std::string> names;
	names.reserve(procs.size());
	for (const auto& p : procs) names.push_back(p.gameName);
	std::sort(names.begin(), names.end());
	std::string key;
	for (const auto& n : names) {
		if (!key.empty()) key += '|';
		key += n;
	}
	return key;
}

/**
 * Return all currently running game candidates (Lutris + Steam combined).
 **/
static std::vector<VnProcess> detectCandidates()
{
	auto procs = ProcessScanner::scan();
	if (procs.empty()) {
		LOG_DEBUG("No game processes found");
	} else {
		LOG_DEBUG(procs.size() << " game candidate(s) found:");
		for (const auto& p : procs)
			LOG_DEBUG("  [" << p.source << "] pid=" << p.pid << "  name=\"" << p.gameName << "\"  starttime(clock ticks)=" << p.starttime);
	}
	return procs;
}

int main(int argc, char* argv[])
{
	bool verbose = false;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--verbose" || arg == "-v")   verbose = true;
		else if (arg == "--help" || arg == "-h") {
			std::cout <<
				"Usage: vn-discord-rpc [OPTIONS]\n\n"
				"Options:\n"
				"  -v, --verbose  Enable DEBUG-level logging\n"
				"  -h, --help     Show this message\n\n"
				"Cache file: " << VnCache::defaultPath().string() << "\n";
			return 0;
		}
	}

	if (verbose) Logger::get().setLevel(LogLevel::DEBUG);

	printBanner();
	LOG_INFO("Poll interval : " << config::POLL_INTERVAL.count() << "s  (process scan)");
	LOG_INFO("RPC interval  : 15s (Discord rate limit)");
	LOG_INFO("Cache file    : " << (config::CACHE_USE_DB
				? VnCache::defaultDbPath().string()
				: VnCache::defaultPath().string()));
	LOG_INFO("Ignore list   : " << IgnoreList::defaultPath().string());
	LOG_INFO("Log level     : " << (verbose ? "DEBUG" : "INFO"));

	std::signal(SIGINT,  signalHandler);
	std::signal(SIGTERM, signalHandler);

	VndbClient      vndb;
	VnCache         cache;
	IgnoreList      ignoreList;
	RpcManager      rpc((std::string(config::DISCORD_APP_ID)));
	AppState        state;

	rpc.connect();
	LOG_INFO("Entering main loop — Ctrl+C to quit");

	while (g_running) {
		if (!rpc.isConnected()) {
			if (!rpc.connect())
				sleepInterruptible(std::chrono::seconds(5));
		}
		rpc.runCallbacks();

		// ── Gather all running game candidates (Lutris + Steam) ──
		auto candidates = detectCandidates();

		// Remove entries the ignore list has already dismissed
		candidates.erase(
				std::remove_if(candidates.begin(), candidates.end(),
					[&](const VnProcess& p){ return ignoreList.matches(p.gameName); }),
				candidates.end());

		// ── Nothing running ──
		if (candidates.empty()) {
			if (state.hasPresence) {
				rpc.clearPresence();
				state.hasPresence       = false;
				state.lastSearchTitle.clear();
				state.lastDetectSource.clear();
				state.candidateSetKey.clear();
				state.stablePollCount   = 0;
				LOG_INFO("No games running — presence cleared");
			}
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}

		// ── Debounce: wait for the candidate set to be stable ──
		// The key is the sorted, pipe-joined list of game names so that
		// adding or removing any game resets the counter.
		std::string setKey = makeCandidateKey(candidates);
		if (setKey != state.candidateSetKey) {
			state.candidateSetKey = setKey;
			state.stablePollCount = 1;
			LOG_DEBUG("Debounce candidate set (1/" << config::STABLE_TITLE_POLLS
					<< "): [" << setKey << "]");
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}
		if (++state.stablePollCount < config::STABLE_TITLE_POLLS) {
			LOG_DEBUG("Debounce " << state.stablePollCount
					<< "/" << config::STABLE_TITLE_POLLS);
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}

		// ── Early-out: current VN still in the candidate set ──
		// If we already have a presence and the game that produced it is still
		// running, there is nothing to resolve — skip the VNDB / cache lookup
		// entirely and wait for the next poll.
		if (state.hasPresence && !state.lastSearchTitle.empty()) {
			bool stillRunning = std::any_of(candidates.begin(), candidates.end(),
					[&](const VnProcess& p){ return p.gameName == state.lastSearchTitle; });
			if (stillRunning) {
				sleepInterruptible(config::POLL_INTERVAL);
				continue;
			}
		}

		// ── Resolve: iterate candidates in priority order, stop at first VN ──
		// Cache hits are free; VNDB queries are issued for cache-miss candidates
		// one at a time.  Non-matches are added to the ignore list so subsequent
		// polls skip them automatically, letting the next candidate be tried.
		std::optional<VnInfo> vnInfo;
		std::string           matchedTitle;
		std::string           matchedSource;

		if (candidates.size() > 1)
			LOG_INFO("Checking " << candidates.size() << " candidates for a VN match");

		for (const auto& proc : candidates) {
			const std::string& title = proc.gameName;

			auto cached = cache.lookup(title);

			if (cached) {
				// ── Explicit SKIP ──
				if (cached->isSkip()) {
					LOG_DEBUG("SKIP cache entry — skipping candidate \""
							<< title << "\"");
					continue;
				}

				// ── Alias (redirect to a different search key) ──
				if (!cached->alias.empty()) {
					if (cached->hasMatch()) {
						vnInfo        = cached->toVnInfo();
						matchedTitle  = title;
						matchedSource = proc.source;
						LOG_INFO("Cache hit (alias→" << cached->alias << "): \"" << vnInfo->title << "\"");
						break;
					}
					// Alias target not yet resolved — query VNDB now
					std::string aliasKey = cached->alias;
					LOG_INFO("Alias \"" << title
							<< "\" → querying VNDB as \"" << aliasKey << "\"");
					vnInfo = vndb.search(aliasKey);
					if (vnInfo) {
						cache.storeAliasFilled(title, aliasKey, *vnInfo);
						matchedTitle  = title;
						matchedSource = proc.source;
						break;
					}
					LOG_INFO("No VNDB match for alias target \"" << aliasKey
							<< "\" — adding \"" << title << "\" to ignore list");
					ignoreList.add(title);
					continue;
				}

				// ── Normal cache hit ──
				if (cached->hasMatch()) {
					int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
							std::chrono::system_clock::now().time_since_epoch()).count();
					int64_t ttlSec = std::chrono::duration_cast<std::chrono::seconds>(
							config::VNDB_CACHE_TTL).count();
					bool stale = (now - cached->cached_at) > ttlSec;

					if (!stale) {
						vnInfo        = cached->toVnInfo();
						matchedTitle  = title;
						matchedSource = proc.source;
						LOG_INFO("Cache hit: \"" << vnInfo->title << "\"");
						break;
					}
					// Stale — refresh from VNDB
					int64_t ageMin = (now - cached->cached_at) / 60;
					LOG_INFO("Cache expired for \"" << title << "\"  age=" << ageMin << "min/" << (ttlSec / 60) << "min — re-querying VNDB");
					vnInfo = vndb.search(title);
					if (vnInfo) {
						cache.store(title, *vnInfo);
						matchedTitle  = title;
						matchedSource = proc.source;
						break;
					}
					// Refresh failed — try next candidate
					continue;
				}

				// Stale no-match row → migrate to ignore list, try next
				LOG_INFO("Stale no-match for \"" << title << "\" — migrating to ignore list");
				ignoreList.add(title);
				continue;
			}

			// ── Cache miss: query VNDB ──
			LOG_INFO("Cache miss — querying VNDB for candidate \""
					<< title << "\"");
			vnInfo = vndb.search(title);
			if (vnInfo) {
				cache.store(title, *vnInfo);
				matchedTitle  = title;
				matchedSource = proc.source;
				LOG_INFO("VNDB match for \"" << title << "\": \"" << vnInfo->title << "\"");
				break;
			}
			// No VNDB match — add to ignore list and try the next candidate
			LOG_INFO("No VNDB match for \"" << title
					<< "\" — adding to ignore list, trying next candidate");
			ignoreList.add(title);
		}

		// ── Same VN still active — nothing to update ──
		if (matchedTitle == state.lastSearchTitle && state.hasPresence) {
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}

		// ── Update presence ──
		if (rpc.isConnected()) {
			if (vnInfo) {
				LOG_INFO("Updating Discord presence: \""
						<< vnInfo->title << "\"  id=" << vnInfo->id);
				LOG_DEBUG("rating=" << vnInfo->rating
						<< "  released=" << vnInfo->released
						<< "  image_sexual=" << vnInfo->image_sexual
						<< "  image_violence=" << vnInfo->image_violence);
				rpc.setPresence(*vnInfo, matchedSource);
				state.hasPresence      = true;
				state.lastSearchTitle  = matchedTitle;
				state.lastDetectSource = matchedSource;
			} else {
				if (state.hasPresence) {
					LOG_INFO("No VN match among candidates — clearing presence");
					rpc.clearPresence();
					state.hasPresence     = false;
					state.lastSearchTitle.clear();
					state.lastDetectSource.clear();
				}
			}
		}

		sleepInterruptible(config::POLL_INTERVAL);
	}

	LOG_INFO("Shutting down");
	rpc.clearPresence();
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(17);
	while (std::chrono::steady_clock::now() < deadline) {
		rpc.runCallbacks();
		if (!rpc.hasPending()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	rpc.disconnect();
	return 0;
}
