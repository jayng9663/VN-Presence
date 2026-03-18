#pragma once
#include "vndb_client.hpp"
#include <string>

/**
 * Manages the Discord Rich Presence connection and presence state.
 *
 * Wraps the EclipseMenu/discord-presence library (RPCManager singleton) with:
 *   - Automatic rate-limit enforcement (16 s between SET_ACTIVITY calls)
 *   - Deferred flush: updates sent within the rate-limit window are queued and
 *     flushed by runCallbacks() once the window clears
 *   - Change detection: identical presence payloads are dropped silently
 *   - Persistent start timestamps: written to
 *     ~/.config/vn-discord-rpc/timestamps/ so reconnects don't reset the
 *     elapsed-time counter
 *   - Lutris playtime integration: start_ts = now - total_playtime
 *
 * All state is protected by a single mutex so runCallbacks() may be called
 * from any thread without data races.
 *
 * Usage:
 * @code
 *   RpcManager rpc{"your_app_id"};
 *   rpc.connect();
 *   // in poll loop:
 *   rpc.runCallbacks();
 *   rpc.setPresence(vnInfo);
 * @endcode
 **/
class RpcManager {
public:
	/** Construct with a Discord application ID. **/
	explicit RpcManager(const std::string& appId);
	~RpcManager();

	/**
	 * Open the Discord IPC socket and register event callbacks.
	 * @return true on success, false if Discord is not running.
	 **/
	bool connect();

	/** Close the Discord IPC socket and flush any pending clear. **/
	void disconnect();

	/** Return true when the IPC socket is open. **/
	[[nodiscard]] bool isConnected() const;

	/**
	 * Send a rich presence update for a matched VN.
	 * - Uses alt_title (original script) as the display title when available.
	 * - Suppresses the cover image when image_sexual >= 2 or image_violence >= 2.
	 * - source="lutris"      → playtime from Lutris DB
	 * - source="steam-appid" → playtime from Steam localconfig.vdf
	 * - Defers to runCallbacks() if called within the 16 s rate-limit window.
	 **/
	void setPresence(const VnInfo& vn, const std::string& source = "");

	/**
	 * Send a generic "Playing a Visual Novel" presence when no VNDB match
	 * was found.
	 **/
	void setGenericPresence(const std::string& windowTitle);

	/**
	 * Clear the Discord presence.
	 * Deferred if called within the rate-limit window.
	 **/
	void clearPresence();

	/**
	 * Send a heartbeat to Discord and flush any pending deferred update or
	 * clear.  Must be called periodically (≈ every POLL_INTERVAL seconds).
	 **/
	void runCallbacks();

	/**
	 * Return true when a deferred update or clear is still waiting to be
	 * flushed.  Used by the shutdown loop to know when it is safe to call
	 * disconnect().
	 **/
	[[nodiscard]] bool hasPending() const;

private:
	struct Impl;
	Impl* d;
};
