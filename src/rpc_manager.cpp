#include "logger.hpp"
#include "rpc_manager.hpp"
#include "config.hpp"
#include "lutris_db.hpp"
#include "steam_detector.hpp"

#include <discord-rpc.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Discord silently drops updates faster than ~15s apart.
// 16s gives a 1s margin over Discord's enforced limit.
static constexpr int64_t RATE_LIMIT_SECONDS = 16;

// ─────────────────────────────────────────────────────────────────────────────
// All RPC state — protected by a single mutex (mirrors the uploaded rpc.cpp)
// ─────────────────────────────────────────────────────────────────────────────
static std::mutex       s_mutex;
static std::string      s_details;
static std::string      s_state;
static std::string      s_largeImageKey;
static std::string      s_largeImageText;
static std::string      s_smallImageKey;
static std::string      s_smallImageText;
static std::string      s_buttonLabel;
static std::string      s_buttonUrl;
static int64_t          s_startTimestamp  = 0;
static bool             s_showImage       = false;
static bool             s_pendingUpdate   = false;
static bool             s_pendingClear    = false;
static int64_t          s_lastPushTime    = 0;
static std::string      s_lastTitle;        // track title changes to reset startTs

// Track whether anything is currently shown in Discord
static bool             s_hasPresence     = false;

// Set by the onReady callback (library IO thread) — Discord (re)connected,
// so the current presence has to be sent again.
static std::atomic<bool> s_resend{false};

// Connection state reported by the library callbacks (IO thread)
static std::atomic<bool> s_ready{false};   ///< READY received on the current connection
static std::atomic<bool> s_lost{false};    ///< connection closed since the last check

// How long to wait for Discord's READY before restarting the library.  Must stay
// well above the library's reconnect backoff (up to 60 s before it connects, and
// again before it reads READY), or we'd restart it before READY can arrive.
static constexpr auto READY_TIMEOUT = std::chrono::minutes(3);

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers (all must be called with s_mutex held)
// ─────────────────────────────────────────────────────────────────────────────
static int64_t wallSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * Convert a /proc/<pid>/stat starttime (clock ticks since boot) to a Unix
 * wall-clock timestamp.  Reads btime once from /proc/stat and caches it.
 * Returns 0 on any read failure.
 **/
static int64_t clockTicksToWallSeconds(int64_t ticks)
{
	if (ticks <= 0) return 0;

	static int64_t s_btime = 0;
	if (s_btime == 0) {
		std::ifstream f("/proc/stat");
		std::string line;
		while (std::getline(f, line)) {
			if (line.rfind("btime ", 0) == 0) {
				try { s_btime = std::stoll(line.substr(6)); } catch (...) {}
				break;
			}
		}
	}

	long clkTck = sysconf(_SC_CLK_TCK);
	if (s_btime <= 0 || clkTck <= 0) return 0;
	return s_btime + ticks / static_cast<int64_t>(clkTck);
}

static int64_t rateLimitRemaining() {
	int64_t elapsed = wallSeconds() - s_lastPushTime;
	int64_t rem     = RATE_LIMIT_SECONDS - elapsed;
	return rem > 0 ? rem : 0;
}

// Apply the current state to Discord. Call with mutex held.
/** Send the current presence state to Discord. Must be called with s_mutex held. **/
static void applyLocked() {
	auto& presence = discord::RPCManager::get()
		.getPresence()
		.clear()
		.setActivityType(static_cast<discord::ActivityType>(config::DISCORD_ACTIVITY_TYPE))
		.setStatusDisplayType(static_cast<discord::StatusDisplayType>(config::DISCORD_STATUS_DISPLAY))
		.setDetails(s_details)
		.setState(s_state)
		.setStartTimestamp(s_startTimestamp);

	if (!s_buttonLabel.empty() && !s_buttonUrl.empty())
		presence.setButton1(s_buttonLabel, s_buttonUrl);

	if (s_showImage) {
		presence
			.setLargeImageKey(s_largeImageKey)
			.setLargeImageText(s_largeImageText)
			.setSmallImageKey(s_smallImageKey)
			.setSmallImageText(s_smallImageText);
	}

	presence.refresh();
	s_lastPushTime  = wallSeconds();
	s_pendingUpdate = false;
	s_hasPresence   = true;

	LOG_DEBUG("Presence applied:"
			<< "  details=\""     << s_details << "\""
			<< "  state=\""       << s_state   << "\""
			<< "  image="         << (s_showImage ? s_largeImageKey : "(none)")
			<< "  start_ts="      << s_startTimestamp);
}

// Try to push; defer if within rate-limit window. Call with mutex held.
/**
 * Attempt to send immediately; defer if within the 16 s rate-limit window.
 * Must be called with s_mutex held.
 * @return true if sent immediately, false if deferred.
 **/
static bool pushOrDefer() {
	int64_t rem = rateLimitRemaining();
	if (rem > 0) {
		LOG_DEBUG("Presence update deferred (rate limit, " << rem << "s remaining)");
		s_pendingUpdate = true;
		return false;
	}
	applyLocked();
	return true;
}

/**
 * Return true when a Discord IPC socket accepts connections.  Tries the same
 * paths as discord-presence (third_party/.../platform/unix.hpp).
 *
 * Why: the library can't recover from a failed attempt — sun_path isn't
 * NUL-terminated between tries and the failed socket leaks — so once it has
 * tried while Discord was down, it never connects.  We only start it when
 * Discord is listening, and restart it after a lost connection.
 **/
static bool discordListening()
{
	std::string dir;
	if (const char* xdg = std::getenv("XDG_RUNTIME_DIR")) {
		dir = xdg;
	} else {
		std::string runUser = "/run/user/" + std::to_string(::getuid());
		if (::access(runUser.c_str(), F_OK) == 0) {
			dir = runUser;
		} else {
			const char* tmp = std::getenv("TMPDIR");
			if (!tmp) tmp = std::getenv("TMP");
			if (!tmp) tmp = std::getenv("TEMP");
			dir = tmp ? tmp : "/tmp";
		}
	}

	for (const char* sub : { "", "/snap.discord", "/app/com.discordapp.Discord", "/app/com.discordapp.DiscordCanary" }) {
		for (int i = 0; i < 10; ++i) {
			std::string path = dir + sub + "/discord-ipc-" + std::to_string(i);
			sockaddr_un addr{};
			addr.sun_family = AF_UNIX;
			if (path.size() >= sizeof(addr.sun_path)) continue;
			std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

			int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
			if (fd == -1) return false;
			bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
			::close(fd);
			if (ok) return true;
		}
	}
	return false;
}

// ─── PIMPL ───
struct RpcManager::Impl {
	std::string appId;
	bool        connected = false;   ///< library initialised (Discord was listening)
	bool        waitingLogged = false;
	std::chrono::steady_clock::time_point startedAt{};
	explicit Impl(std::string id) : appId(std::move(id)) {}
};

RpcManager::RpcManager(const std::string& appId) : d(new Impl(appId)) {}
RpcManager::~RpcManager() { disconnect(); delete d; }

// ─── connect / disconnect ───
/**
 * Register event callbacks and start the library — but only once Discord is
 * listening (see discordListening()).  Returns false while Discord is down.
 **/
bool RpcManager::connect()
{
	if (d->connected) return true;

	if (!discordListening()) {
		if (!d->waitingLogged) LOG_INFO("Waiting for Discord to start…");
		d->waitingLogged = true;
		return false;
	}
	d->waitingLogged = false;
	s_ready = false;
	s_lost  = false;

	discord::RPCManager::get()
		.setClientID(d->appId)
		.onReady([](discord::User const& user) {
				LOG_INFO("Connected to Discord as " << user.username);
				s_ready  = true;
				s_resend = true;
				})
	.onDisconnected([](int code, std::string_view msg) {
			LOG_INFO("Disconnected from Discord (" << code << "): " << msg);
			s_lost = true;
			})
	.onErrored([](int code, std::string_view msg) {
			LOG_ERR("Discord RPC error " << code << ": " << msg);
			});

	LOG_DEBUG("Calling RPCManager::initialize() for app=" << d->appId);
	discord::RPCManager::get().initialize();
	d->connected = true;
	d->startedAt = std::chrono::steady_clock::now();
	LOG_INFO("RPCManager initialized (app=" << d->appId << ")");
	return true;
}

void RpcManager::disconnect()
{
	if (!d->connected) return;
	LOG_DEBUG("RPCManager::shutdown()");
	discord::RPCManager::get().shutdown();
	d->connected = false;
	LOG_INFO("Discord RPC disconnected");
}

bool RpcManager::isConnected() const { return d->connected; }

// ─────────────────────────────────────────────────────────────────────────────
// runCallbacks — flush any pending update/clear
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Flush any pending deferred update or clear once the rate limit allows.
 * Call this every POLL_INTERVAL seconds from the main loop.
 **/
void RpcManager::runCallbacks()
{
	if (!d->connected) return;

	// Connection lost, or no READY in time — shut the library down so the
	// next connect() starts from a fresh socket (it can't reconnect itself).
	bool noReady = !s_ready && std::chrono::steady_clock::now() - d->startedAt > READY_TIMEOUT;
	if (s_lost.exchange(false) || noReady) {
		LOG_INFO((noReady ? "No answer from Discord — reconnecting" : "Discord connection lost — waiting for it to come back"));
		discord::RPCManager::get().shutdown();
		d->connected = false;
		return;
	}

	// Don't call discord::RPCManager::update() here — the library's own IO
	// thread already does (every 500 ms), and update() isn't thread-safe:
	// calling it from two threads can open two IPC sockets at startup and
	// leave the handshake stuck, so presence never shows up in Discord.

	std::lock_guard<std::mutex> lock(s_mutex);

	// Discord restarted — it forgot our activity, so queue it again
	if (s_resend.exchange(false) && s_hasPresence && !s_pendingClear) {
		LOG_INFO("Discord reconnected — re-sending presence");
		s_pendingUpdate = true;
	}

	// Flush pending clear
	if (s_pendingClear && rateLimitRemaining() == 0) {
		LOG_INFO("Flushing deferred clearPresence");
		discord::RPCManager::get().clearPresence();
		s_lastPushTime  = wallSeconds();
		s_pendingClear  = false;
		s_pendingUpdate = false;
		s_hasPresence   = false;
		return;
	}

	// Flush pending update
	if (s_pendingUpdate && rateLimitRemaining() == 0) {
		LOG_INFO("Flushing deferred presence update: \"" << s_details << "\"");
		applyLocked();
		return;
	}

	if (s_pendingUpdate || s_pendingClear) {
		LOG_DEBUG("Pending flush in " << rateLimitRemaining() << "s");
	}
}

// ─── setPresence ───
/**
 * Build and send (or queue) a rich presence update from VNDB data.
 * Uses alt_title as display title, suppresses explicit cover images,
 * and sets start_ts from Lutris or Steam playtime.
 **/
void RpcManager::setPresence(const VnInfo& vn, const std::string& source, const std::string& detectedName, int64_t processStartTicks, int steamAppId)
{
	if (!d->connected) return;

	// Prefer original script title (Japanese/Chinese/Korean); fall back to romanized
	const std::string& displayTitle = vn.alt_title.empty() ? vn.title : vn.alt_title;

	std::string state = std::string(config::RPC_STATE_READING);
	if (!vn.released.empty())
		state += "  \u2022  " + vn.released.substr(0, 4);

	bool showImage = !vn.image_url.empty() && !vn.isImageExplicit();

	// ── Resolve playtime → start timestamp ──
	// Lutris source: query Lutris SQLite DB by title
	// Steam source:  read localconfig.vdf by AppID (minutes → seconds)
	std::optional<int64_t> playtimeSeconds;

	if (source == "steam-appid") {
		if (steamAppId > 0) {
			int64_t mins = SteamDetector::getPlaytimeMinutes(steamAppId);
			if (mins > 0) {
				playtimeSeconds = mins * 60;
				LOG_DEBUG("Steam playtime: " << mins << "min  ("
						<< (mins / 60) << "h " << (mins % 60) << "m)");
			}
		}
	} else {
		// Lutris (or unknown source) — use Lutris DB
		std::optional<int64_t> pt;
		if (!vn.alt_title.empty())
			pt = LutrisDB::getPlaytime(vn.alt_title);
		if (!pt)
			pt = LutrisDB::getPlaytime(vn.title);
		if (!pt && !detectedName.empty())
			pt = LutrisDB::getPlaytime(detectedName);
		playtimeSeconds = pt;
		if (pt)
			LOG_DEBUG("Lutris playtime: " << LutrisDB::formatPlaytime(*pt));
	}

	// Resolve startTs:
	//   1. Same title still active in memory → keep existing timestamp
	//   2. processStartWall available + playtime → processStartWall - playtime
	//      (elapsed = session_time + prior_playtime)
	//   3. processStartWall only → use as-is (session time only)
	//   4. playtime only (no proc info) → now - playtime
	//   5. Fallback → now
	int64_t processStartWall = clockTicksToWallSeconds(processStartTicks);
	int64_t startTs;
	if (vn.id == s_lastTitle && s_startTimestamp != 0) {
		startTs = s_startTimestamp;
		LOG_DEBUG("Reusing existing start_ts=" << startTs);
	} else if (processStartWall > 0 && playtimeSeconds && *playtimeSeconds > 0) {
		startTs = processStartWall - *playtimeSeconds;
		LOG_DEBUG("Combined start_ts=" << startTs
				<< "  (proc_start=" << processStartWall
				<< "  playtime=" << *playtimeSeconds << "s)");
	} else if (processStartWall > 0) {
		startTs = processStartWall;
		LOG_DEBUG("Process start_ts=" << startTs);
	} else if (playtimeSeconds && *playtimeSeconds > 0) {
		startTs = wallSeconds() - *playtimeSeconds;
		LOG_DEBUG("Playtime-only start_ts=" << startTs);
	} else {
		startTs = wallSeconds();
		LOG_DEBUG("New start_ts=" << startTs);
	}

	std::lock_guard<std::mutex> lock(s_mutex);

	// Skip entirely if nothing changed
	bool same = s_hasPresence
		&& s_details       == displayTitle
		&& s_state         == state
		&& s_largeImageKey == (showImage ? vn.image_url : "")
		&& s_buttonUrl     == vn.vndb_url
		&& !s_pendingUpdate;
	if (same) {
		LOG_DEBUG("Presence unchanged — skipping update");
		return;
	}

	s_lastTitle      = vn.id;  // use vndb id as key, not display title
	s_details        = displayTitle;
	s_state          = state;
	s_largeImageKey  = showImage ? vn.image_url : "";
	// VNDB rating is 10–100; show it out of 10 like the website does
	s_largeImageText = (config::RPC_SHOW_RATING && vn.rating > 0)
		? std::format("{} — ★ {:.2f}", displayTitle, vn.rating / 10.0)
		: displayTitle;
	s_smallImageKey  = std::string(config::RPC_SMALL_IMG_KEY);
	s_smallImageText = std::string(config::RPC_SMALL_IMG_TEXT);
	s_buttonLabel    = "View on VNDB";
	s_buttonUrl      = vn.vndb_url;
	s_startTimestamp = startTs;
	s_showImage      = showImage;
	s_pendingClear   = false;

	LOG_DEBUG("setPresence:"
			<< "  display=\"" << displayTitle << "\""
			<< "  alt=\""     << vn.alt_title  << "\""
			<< "  state=\""   << s_state        << "\""
			<< "  image="      << (showImage ? vn.image_url : "(suppressed)")
			<< "  sexual="     << vn.image_sexual
			<< "  violence="   << vn.image_violence);

	if (pushOrDefer())
		LOG_INFO("Presence sent: \"" << s_details << "\"");
	else
		LOG_INFO("Presence queued: \"" << s_details << "\"");
}

// ─── setGenericPresence ───
void RpcManager::setGenericPresence(const std::string& windowTitle)
{
	if (!d->connected) return;

	std::string details = windowTitle.empty()
		? std::string(config::RPC_DEFAULT_DETAILS)
		: windowTitle;

	std::lock_guard<std::mutex> lock(s_mutex);

	if (s_hasPresence && s_details == details && !s_pendingUpdate) {
		LOG_DEBUG("Generic presence unchanged — skipping");
		return;
	}

	s_details        = details;
	s_state          = std::string(config::RPC_STATE_READING);
	s_largeImageKey  = "vn_default";
	s_largeImageText = "Visual Novel";
	s_smallImageKey  = std::string(config::RPC_SMALL_IMG_KEY);
	s_smallImageText = std::string(config::RPC_SMALL_IMG_TEXT);
	s_buttonLabel    = "";
	s_buttonUrl      = "";
	s_startTimestamp = wallSeconds();
	s_showImage      = true;
	s_pendingClear   = false;

	pushOrDefer();
}

// ─── hasPending ───
bool RpcManager::hasPending() const {
	std::lock_guard<std::mutex> lock(s_mutex);
	return s_pendingUpdate || s_pendingClear;
}

// ─── clearPresence ───
/** Clear the Discord presence (deferred if within rate-limit window). **/
void RpcManager::clearPresence()
{
	std::lock_guard<std::mutex> lock(s_mutex);

	// Not connected — nothing to send, but forget the presence so it isn't
	// re-sent when Discord comes back
	if (!d->connected) {
		s_hasPresence   = false;
		s_pendingUpdate = false;
		s_pendingClear  = false;
		s_lastTitle.clear();
		return;
	}

	if (!s_hasPresence && !s_pendingUpdate && !s_pendingClear) {
		LOG_DEBUG("clearPresence: already clear");
		return;
	}

	s_pendingUpdate = false;  // discard any queued update
	s_lastTitle.clear();

	if (rateLimitRemaining() == 0) {
		LOG_DEBUG("Sending clearPresence to Discord");
		discord::RPCManager::get().clearPresence();
		s_lastPushTime = wallSeconds();
		s_hasPresence  = false;
		s_pendingClear = false;
	} else {
		LOG_INFO("clearPresence deferred (" << rateLimitRemaining() << "s remaining)");
		s_pendingClear = true;
		s_hasPresence  = false;
	}
}
