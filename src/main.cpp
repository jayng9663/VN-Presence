#include "config.hpp"
#include "logger.hpp"
#include "process_scanner.hpp"
#include "vndb_client.hpp"
#include "vn_cache.hpp"
#include "ignore_list.hpp"
#include "rpc_manager.hpp"

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
	std::string lastSearchTitle;
	std::string candidateTitle;
	std::string detectionSource;  ///< "lutris" | "steam-appid" | ""
	int         stablePollCount = 0;
	bool        hasPresence     = false;
};

/** Print the startup banner and warn if APP_ID is unset. **/
static void printBanner()
{
	std::cout << R"(
  ╭──────────────────────────────────────╮
  │        vn-discord-rpc  v1.0.0        │
  │  Visual Novel Discord Rich Presence  │
  ╰──────────────────────────────────────╯
)" << std::flush;
if (std::string(config::DISCORD_APP_ID) == "YOUR_DISCORD_APP_ID_HERE")
	LOG_WARN("DISCORD_APP_ID not set in src/config.hpp! "
	"Get one at: https://discord.com/developers/applications");
}

/**
 * Detect the currently running VN via Lutris wrapper or Steam AppID.
 * @param source  Filled with "lutris" or "steam-appid" on success.
 * @return Game name string to search on VNDB, or "" if nothing found.
 **/
static std::string detectCurrentVn(std::string& source)
{
	auto proc = ProcessScanner::findFirst();
	static std::string lastGame;

	if (proc) {
		if (proc->gameName != lastGame) {
			LOG_INFO("Detected [" << proc->source << "] \""
			<< proc->gameName << "\"  pid=" << proc->pid);
			LOG_DEBUG("exe=" << proc->exePath);
			lastGame = proc->gameName;
		}
		source = proc->source;
		return proc->gameName;
	}

	LOG_DEBUG("No VN process found");
	source.clear();
	return {};
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
	LOG_INFO("Cache file    : " << VnCache::defaultPath().string());
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

		std::string searchTitle = detectCurrentVn(state.detectionSource);

		// ── Nothing running ──
		if (searchTitle.empty()) {
			if (state.hasPresence) {
				rpc.clearPresence();
				state.hasPresence     = false;
				state.lastSearchTitle.clear();
				state.candidateTitle.clear();
				state.stablePollCount = 0;
				LOG_INFO("VN no longer running — presence cleared");
			}
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}

		// ── Debounce ──
		if (searchTitle != state.candidateTitle) {
			state.candidateTitle  = searchTitle;
			state.stablePollCount = 1;
			LOG_DEBUG("Debounce candidate (1/" << config::STABLE_TITLE_POLLS
			<< "): \"" << searchTitle << "\"");
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}
		if (++state.stablePollCount < config::STABLE_TITLE_POLLS) {
			LOG_DEBUG("Debounce " << state.stablePollCount
			<< "/" << config::STABLE_TITLE_POLLS);
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}

		// ── Same as currently shown ──
		if (searchTitle == state.lastSearchTitle) {
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}

		// ── Ignore list check ──
		if (ignoreList.matches(searchTitle)) {
			LOG_INFO("Ignored: \"" << searchTitle << "\"");
			if (state.hasPresence) { rpc.clearPresence(); state.hasPresence = false; }
			state.lastSearchTitle = searchTitle;
			sleepInterruptible(config::POLL_INTERVAL);
			continue;
		}

		// ── Resolve via cache ──
		LOG_INFO("Resolving: \"" << searchTitle << "\"");
		std::optional<VnInfo> vnInfo;
		auto cached = cache.lookup(searchTitle);

		if (cached) {
			if (cached->isSkip()) {
				LOG_INFO("SKIP entry for \"" << searchTitle << "\"");
				if (state.hasPresence) { rpc.clearPresence(); state.hasPresence = false; }
				state.lastSearchTitle = searchTitle;
				sleepInterruptible(config::POLL_INTERVAL);
				continue;

			} else if (!cached->alias.empty()) {
				if (cached->hasMatch()) {
					vnInfo = cached->toVnInfo();
					LOG_INFO("Cache hit (alias→" << cached->alias
					<< "): \"" << vnInfo->title << "\"");
				} else {
					std::string aliasKey = cached->alias;
					LOG_INFO("Alias \"" << searchTitle
					<< "\" → querying VNDB as \"" << aliasKey << "\"");
					vnInfo = vndb.search(aliasKey);
					if (vnInfo) cache.storeAliasFilled(searchTitle, aliasKey, *vnInfo);
					else {
						LOG_INFO("No VNDB match for alias target \"" << aliasKey
						<< "\" — adding \"" << searchTitle << "\" to ignore list");
						ignoreList.add(searchTitle);
					}
				}

			} else if (cached->hasMatch()) {
				vnInfo = cached->toVnInfo();
				LOG_INFO("Cache hit: \"" << vnInfo->title << "\"");

			} else {
				LOG_INFO("Stale no-match entry for \"" << searchTitle
				<< "\" — migrating to ignore list");
				ignoreList.add(searchTitle);
			}

		} else {
			LOG_INFO("Cache miss — querying VNDB for \"" << searchTitle << "\"");
			vnInfo = vndb.search(searchTitle);
			if (vnInfo) cache.store(searchTitle, *vnInfo);
			else {
				LOG_INFO("No VNDB match for \"" << searchTitle
				<< "\" — adding to ignore list");
				ignoreList.add(searchTitle);
			}
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
				rpc.setPresence(*vnInfo, state.detectionSource);
				state.hasPresence     = true;
				state.lastSearchTitle = searchTitle;
			} else {
				LOG_INFO("No VNDB match — clearing presence");
				rpc.clearPresence();
				state.hasPresence     = false;
				state.lastSearchTitle.clear();
			}
		}

		sleepInterruptible(config::POLL_INTERVAL);
	}

	LOG_INFO("Shutting down");
	rpc.clearPresence();
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(17);
	while (std::chrono::steady_clock::now() < deadline) {
		rpc.runCallbacks();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	rpc.disconnect();
	return 0;
}
