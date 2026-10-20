#pragma once
// Engine-independent core of the "cookies" plugin (no s2sdk / plugify / plgdb internals).
//
// Port of the Pisex Metamod plugin ICookiesApi (SetCookie / GetCookie / HookClientCookieLoaded)
// on top of plgdb. The table schema is byte-for-byte compatible with the original Pisex cookies
// plugin so a Metamod server and a Plugify server can share one database.
//
// Threading: every method (including the onCached / onError callbacks) runs on the single
// thread that also calls plgdb::Database::Poll() - the game thread. There is no locking;
// async results are applied inside Poll() callbacks and are validated against a per-slot
// generation counter, so a late SELECT for a client that already left is discarded.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <plgdb/db.hpp>

namespace cookies {

class CookieStore {
public:
	static constexpr int kMaxSlots = 64;                 // engine player slots: 0..63
	static constexpr std::size_t kMaxNameBytes = 255;    // schema: cookie_name VARCHAR(255)
	static constexpr std::size_t kMaxValueBytes = 65535; // schema: cookie_data TEXT (MySQL cap)

	explicit CookieStore(plgdb::Database& db);
	~CookieStore();

	CookieStore(const CookieStore&) = delete;
	CookieStore& operator=(const CookieStore&) = delete;

	// Creates the shared `client_cookies` table if it does not exist. Blocking; startup only.
	// Throws std::runtime_error if the statement fails.
	void EnsureSchema();

	// Marks the slot as owned by an authenticated client and starts the async cookie load.
	// Invalid slots and steamid64 == 0 (bots / unauthenticated clients) are ignored.
	void OnClientAuthenticated(int slot, uint64_t steamid64);

	// Drops everything cached for the slot; a SELECT still in flight for the old client is
	// discarded (generation check). Values that were set while the load was pending are
	// flushed to the DB first, while the steamid is still known.
	void OnClientDisconnect(int slot);

	// True once the DB load for the slot's current client has been applied.
	bool IsCached(int slot) const;

	// SteamID64 of the client the slot is bound to, 0 if none (or invalid slot).
	uint64_t SteamId(int slot) const;

	// Cached value, or "" when missing / not loaded / invalid slot (matches the original plugin).
	std::string Get(int slot, std::string_view name) const;

	// Updates the cache immediately; the DB write is asynchronous (bound parameters, no SQL
	// interpolation - the Pisex version interpolated '%s' unescaped, an injection bug).
	// May be called before the cookies of the slot's client are loaded: the value is kept in
	// the cache, marked dirty and flushed once the load lands (or when the client disconnects);
	// the rows arriving from the pending SELECT never overwrite it. A slot without an
	// authenticated client is rejected with a message on onError - the value cannot be
	// attributed to a player, and keeping it would give it to whoever takes the slot next.
	// Over-long names (> 255 bytes) and values (> 65535 bytes) are rejected with a message on
	// onError.
	void Set(int slot, std::string name, std::string value);

	// Invoked from Poll() once the cookies of a slot became available - Pisex HookClientCookieLoaded.
	std::function<void(int slot)> onCached;
	// Rejections and SQL errors are reported here; wire it to a logger. May be empty.
	std::function<void(std::string_view message)> onError;

private:
	struct Slot {
		uint64_t steamid = 0;    // 0 == no authenticated client on this slot
		uint64_t generation = 0; // bumped on auth/disconnect; ties async results to one client
		bool loaded = false;     // DB load applied for the current generation
		std::map<std::string, std::string, std::less<>> cookies;
		std::map<std::string, std::string, std::less<>> dirty; // set before load, not yet persisted
	};

	static bool ValidSlot(int slot);
	void Load(int slot);
	void Upsert(int slot, std::string_view name, std::string_view value);
	void ReportError(std::string_view message) const;

	plgdb::Database& m_db;
	std::array<Slot, kMaxSlots> m_slots{};
};

} // namespace cookies
