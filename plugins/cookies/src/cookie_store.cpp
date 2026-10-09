#include "cookie_store.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace cookies {
namespace {

// Schema copied verbatim from ref/pisex-cookies/cookies.cpp (LoadConfig) so that the Metamod
// and the Plugify plugin keep using the same table and the same unique key on (steamid, name).
constexpr std::string_view kCreateTableMySQL =
	"CREATE TABLE IF NOT EXISTS `client_cookies` ("
	"`id` INT PRIMARY KEY AUTO_INCREMENT, "
	"`steamid` BIGINT NOT NULL, "
	"`cookie_name` VARCHAR(255) NOT NULL, "
	"`cookie_data` TEXT NOT NULL, "
	"UNIQUE KEY `steamid_cookie_name` (`steamid`, `cookie_name`)"
	");";

constexpr std::string_view kCreateTableSQLite =
	"CREATE TABLE IF NOT EXISTS `client_cookies` ("
	"`id` INTEGER PRIMARY KEY AUTOINCREMENT, "
	"`steamid` INTEGER NOT NULL, "
	"`cookie_name` TEXT NOT NULL, "
	"`cookie_data` TEXT NOT NULL, "
	"UNIQUE(`steamid`, `cookie_name`)"
	");";

constexpr std::string_view kSelectCookies =
	"SELECT cookie_name, cookie_data FROM client_cookies WHERE steamid = ?";

constexpr std::string_view kUpsertMySQL =
	"INSERT INTO client_cookies (steamid, cookie_name, cookie_data) VALUES (?, ?, ?) "
	"ON DUPLICATE KEY UPDATE cookie_data = VALUES(cookie_data)";

constexpr std::string_view kUpsertSQLite =
	"INSERT INTO client_cookies (steamid, cookie_name, cookie_data) VALUES (?, ?, ?) "
	"ON CONFLICT(steamid, cookie_name) DO UPDATE SET cookie_data = excluded.cookie_data";

std::string SlotMessage(std::string_view what, int slot) {
	std::string message("cookies: ");
	message.append(what);
	message.append(" slot ");
	message.append(std::to_string(slot));
	return message;
}

} // namespace

CookieStore::CookieStore(plgdb::Database& db) : m_db(db) {}

CookieStore::~CookieStore() = default;

bool CookieStore::ValidSlot(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

void CookieStore::ReportError(std::string_view message) const {
	if (onError) {
		onError(message);
	}
}

void CookieStore::EnsureSchema() {
	const std::string_view sql =
		m_db.GetDriver() == plgdb::Driver::MySQL ? kCreateTableMySQL : kCreateTableSQLite;
	const plgdb::Result result = m_db.QuerySync(std::string(sql), {});
	if (!result.Ok()) {
		throw std::runtime_error("cookies: creating client_cookies table failed: " + result.Error());
	}
}

void CookieStore::OnClientAuthenticated(int slot, uint64_t steamid64) {
	if (!ValidSlot(slot)) {
		ReportError(SlotMessage("OnClientAuthenticated: invalid", slot));
		return;
	}
	if (steamid64 == 0) {
		return; // bots / clients without an auth id are never cached
	}

	Slot& s = m_slots[slot];
	if (s.steamid == steamid64) {
		return; // already loading or loaded: the s2sdk event and the poll may both report it
	}
	if (s.steamid != 0) {
		// A previous client on this slot never disconnected (or disconnect was missed):
		// its cache must not leak into the new client.
		s.cookies.clear();
		s.dirty.clear();
	}
	++s.generation;
	s.steamid = steamid64;
	s.loaded = false;
	// Values set while the slot had no steamid (e.g. from a connect callback) stay in the
	// cache: they were marked dirty and belong to this client.

	Load(slot);
}

void CookieStore::OnClientDisconnect(int slot) {
	if (!ValidSlot(slot)) {
		return;
	}

	Slot& s = m_slots[slot];
	++s.generation; // any in-flight SELECT for this client is now stale
	if (s.steamid != 0 && !s.dirty.empty()) {
		// Persist values that were cached while the load was still pending - this is the
		// last moment the steamid is known.
		auto dirty = std::move(s.dirty);
		s.dirty.clear();
		for (const auto& [name, value] : dirty) {
			Upsert(slot, name, value);
		}
	}
	s.steamid = 0;
	s.loaded = false;
	s.cookies.clear();
	s.dirty.clear();
}

uint64_t CookieStore::SteamId(int slot) const {
	return ValidSlot(slot) ? m_slots[slot].steamid : 0;
}

bool CookieStore::IsCached(int slot) const {
	return ValidSlot(slot) && m_slots[slot].loaded;
}

std::string CookieStore::Get(int slot, std::string_view name) const {
	if (!ValidSlot(slot)) {
		return {};
	}
	const Slot& s = m_slots[slot];
	const auto it = s.cookies.find(name);
	return it == s.cookies.end() ? std::string{} : it->second;
}

void CookieStore::Set(int slot, std::string name, std::string value) {
	if (!ValidSlot(slot)) {
		ReportError(SlotMessage("Set: invalid", slot));
		return;
	}
	if (name.empty()) {
		ReportError(SlotMessage("Set: empty cookie name rejected,", slot));
		return;
	}
	if (name.size() > kMaxNameBytes) {
		ReportError(SlotMessage("Set: cookie name longer than 255 bytes rejected,", slot));
		return;
	}
	if (value.size() > kMaxValueBytes) {
		ReportError(SlotMessage("Set: cookie value longer than 65535 bytes rejected,", slot));
		return;
	}

	Slot& s = m_slots[slot];
	const auto it = s.cookies.insert_or_assign(std::move(name), std::move(value)).first;
	const std::string& storedName = it->first;
	const std::string& storedValue = it->second;

	if (!s.loaded) {
		// A SELECT may still be in flight; keep the new value on top of whatever it returns.
		s.dirty.insert_or_assign(storedName, storedValue);
	}
	if (s.steamid != 0) {
		Upsert(slot, storedName, storedValue);
	}
	// Otherwise the value stays cached + dirty and is written as soon as the steamid is known.
}

void CookieStore::Load(int slot) {
	Slot& s = m_slots[slot];
	const uint64_t generation = s.generation;
	const uint64_t steamid = s.steamid;

	m_db.Query(std::string(kSelectCookies), {static_cast<int64_t>(steamid)},
		[this, slot, generation](const plgdb::Result& result) {
			try {
				Slot& current = m_slots[slot];
				if (current.generation != generation) {
					return; // the client left (or the slot got reused): drop the stale result
				}

				if (result.Ok()) {
					const std::optional<std::size_t> nameCol = result.ColumnIndex("cookie_name");
					const std::optional<std::size_t> dataCol = result.ColumnIndex("cookie_data");
					if (nameCol && dataCol) {
						for (std::size_t row = 0; row < result.RowCount(); ++row) {
							std::string name = result.GetString(row, *nameCol);
							std::string value = result.GetString(row, *dataCol);
							// Values written while this SELECT was in flight win over the DB.
							if (current.dirty.contains(name)) {
								continue;
							}
							current.cookies.insert_or_assign(std::move(name), std::move(value));
						}
					} else {
						ReportError(SlotMessage("load: unexpected result columns,", slot));
					}
				} else {
					// Report, but still mark the slot cached (the original plugin also
					// signalled loaded after a failed query); waiting plugins must not hang.
					ReportError(SlotMessage("load failed,", slot) + ": " + result.Error());
				}

				if (!current.dirty.empty()) {
					auto dirty = std::move(current.dirty);
					current.dirty.clear();
					for (const auto& [name, value] : dirty) {
						Upsert(slot, name, value);
					}
				}
				current.loaded = true;
				if (onCached) {
					onCached(slot);
				}
			} catch (...) {
				// Never let an exception escape into plgdb::Database::Poll().
				ReportError(SlotMessage("load: unexpected exception for", slot));
			}
		});
}

void CookieStore::Upsert(int slot, std::string_view name, std::string_view value) {
	const Slot& s = m_slots[slot];
	if (s.steamid == 0) {
		return;
	}

	const std::string_view sql =
		m_db.GetDriver() == plgdb::Driver::MySQL ? kUpsertMySQL : kUpsertSQLite;
	m_db.Query(std::string(sql),
		{static_cast<int64_t>(s.steamid), std::string(name), std::string(value)},
		[this, slot](const plgdb::Result& result) {
			try {
				if (!result.Ok()) {
					ReportError(SlotMessage("write failed,", slot) + ": " + result.Error());
				}
			} catch (...) {
				// No exception may escape into plgdb::Database::Poll().
			}
		});
}

} // namespace cookies
