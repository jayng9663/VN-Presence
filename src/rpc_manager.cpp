#include "logger.hpp"
#include "rpc_manager.hpp"
#include "config.hpp"
#include "lutris_db.hpp"
#include "steam_detector.hpp"

#include <discord-rpc.hpp>

#include <chrono>
#include <mutex>
#include <string>

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

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers (all must be called with s_mutex held)
// ─────────────────────────────────────────────────────────────────────────────
static int64_t wallSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
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
		.setActivityType(discord::ActivityType::Game)
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

// ─── PIMPL ───
struct RpcManager::Impl {
	std::string appId;
	bool        connected = false;
	explicit Impl(std::string id) : appId(std::move(id)) {}
};

RpcManager::RpcManager(const std::string& appId) : d(new Impl(appId)) {}
RpcManager::~RpcManager() { disconnect(); delete d; }

// ─── connect / disconnect ───
/** Open the Discord IPC socket and register event callbacks. **/
bool RpcManager::connect()
{
	if (d->connected) return true;

	discord::RPCManager::get()
		.setClientID(d->appId)
		.onReady([](discord::User const& user) {
				LOG_INFO("Connected to Discord as " << user.username);
				})
	.onDisconnected([](int code, std::string_view msg) {
			LOG_INFO("Disconnected from Discord (" << code << "): " << msg);
			})
	.onErrored([](int code, std::string_view msg) {
			LOG_ERR("Discord RPC error " << code << ": " << msg);
			});

	LOG_DEBUG("Calling RPCManager::initialize() for app=" << d->appId);
	discord::RPCManager::get().initialize();
	d->connected = true;
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
// runCallbacks — heartbeat + flush any pending update/clear
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Send a heartbeat and flush any pending deferred update or clear.
 * Call this every POLL_INTERVAL seconds from the main loop.
 **/
void RpcManager::runCallbacks()
{
	if (!d->connected) return;

	LOG_DEBUG("RPCManager::update() heartbeat");
	discord::RPCManager::get().update();

	std::lock_guard<std::mutex> lock(s_mutex);

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
void RpcManager::setPresence(const VnInfo& vn, const std::string& source)
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
		int appId = SteamDetector::getRunningAppId();
		if (appId > 0) {
			int64_t mins = SteamDetector::getPlaytimeMinutes(appId);
			if (mins > 0) {
				playtimeSeconds = mins * 60;
				LOG_DEBUG("Steam playtime: " << mins << "min  ("
						<< (mins / 60) << "h " << (mins % 60) << "m)");
			}
		}
	} else {
		// Lutris (or unknown source) — use Lutris DB
		auto pt = LutrisDB::getPlaytime(vn.title.empty() ? vn.alt_title : vn.title);
		if (!pt && !vn.alt_title.empty())
			pt = LutrisDB::getPlaytime(vn.alt_title);
		playtimeSeconds = pt;
		if (pt)
			LOG_DEBUG("Lutris playtime: " << LutrisDB::formatPlaytime(*pt));
	}

	// Resolve startTs:
	//   1. Same title still active in memory → keep existing timestamp
	//   2. Playtime available → now - playtime
	//   3. Fallback → now
	int64_t startTs;
	if (vn.id == s_lastTitle && s_startTimestamp != 0) {
		startTs = s_startTimestamp;
		LOG_DEBUG("Reusing existing start_ts=" << startTs);
	} else if (playtimeSeconds && *playtimeSeconds > 0) {
		startTs = wallSeconds() - *playtimeSeconds;
		LOG_DEBUG("Playtime start_ts=" << startTs);
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
	s_largeImageText = displayTitle;
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

// ─── clearPresence ───
/** Clear the Discord presence (deferred if within rate-limit window). **/
void RpcManager::clearPresence()
{
	if (!d->connected) return;

	std::lock_guard<std::mutex> lock(s_mutex);

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
