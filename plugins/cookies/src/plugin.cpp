// Plugify glue for the "cookies" plugin: config loading, s2sdk listener registration,
// exported methods (SetClientCookie / GetClientCookie / AreClientCookiesCached and the
// OnClientCookiesCached_Register/Unregister pair), and the Poll() pump.
//
// Nothing here blocks on the database on the game thread; the only synchronous statements
// are the schema creation at plugin start.

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <plgdb/db.hpp>

#include "cookie_store.hpp"
#include "db_config.hpp"

namespace {

using OnClientCookiesCachedCallback = void (*)(int32_t);

// Written on first start (the package ships the same file): the database connection moved to
// databases.cfg next to it (PLUGIN_STANDARD.md section 4), so only non-database options - none
// yet - belong here.
constexpr std::string_view kDefaultConfig = R"({
	// The database connection of this plugin is read from configs/cookies/databases.cfg
	// (section "cookies", see docs/PLUGIN_STANDARD.md section 4). This file keeps the
	// non-database options of the plugin; there are none yet.
})";

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarning(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

std::string JoinPath(std::string_view dir, std::string_view leaf) {
	if (dir.empty()) {
		return std::string(leaf);
	}
	std::string path(dir);
	if (path.back() != '/' && path.back() != '\\') {
		path.push_back('/');
	}
	path.append(leaf);
	return path;
}

} // namespace

namespace {

class CookiesPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// s2sdk listener bodies (game thread, invoked by the engine through s2sdk).
	void HandleClientAuthenticated(int32_t playerSlot, uint64_t steamid64);
	void HandleClientDisconnect(int32_t playerSlot);
	// Map lifecycle (state only - see m_mapReady).
	void HandleMapStart();
	void HandleMapEnd();

	// Exported API.
	void SetCookie(int32_t playerSlot, std::string_view name, std::string_view value);
	std::string GetCookie(int32_t playerSlot, std::string_view name) const;
	bool AreCached(int32_t playerSlot) const;
	void RegisterCachedCallback(OnClientCookiesCachedCallback callback);
	void UnregisterCachedCallback(OnClientCookiesCachedCallback callback);

private:
	bool LoadConfig(std::string& error);
	bool CreateDatabase(std::string& error);
	bool RegisterListeners(std::string& error);
	void UnregisterListeners();
	// Once per second: binds authorized human clients that have no cookies yet (late-load and
	// missed OnClientAuthenticated events) and releases slots whose client is gone.
	void ReconcileClients();
	// Calls every registered OnClientCookiesCached callback with a snapshot of the list,
	// so callbacks may (un)register themselves while the dispatch is running.
	void DispatchCached(int32_t playerSlot);

	plgdb::Config m_dbConfig;
	std::unique_ptr<plgdb::Database> m_db;
	std::unique_ptr<cookies::CookieStore> m_store;
	bool m_listenersRegistered = false;
	// s2sdk client queries crash without a live map: before the first map and between OnMapEnd
	// and the next OnMapStart (the client list is torn down; the host keeps pumping
	// OnPluginUpdate through a changelevel). The reconcile therefore runs only while m_mapReady,
	// the same state machine as pisexcompat/perm_connector/hudmenu.
	// It also covers missed s2sdk events: OnClientAuthenticated is driven solely by Steam's
	// ValidateAuthTicketResponse callback and does not reliably fire for real players.
	bool m_reconcileEnabled = false;
	bool m_mapReady = false;
	bool m_mapStartSeen = false; // an OnMapStart arrived since the last OnMapEnd
	bool m_mapEndSeen = false;	 // an OnMapEnd arrived since the last OnMapStart
	std::chrono::milliseconds m_sinceReconcile{0};
	std::vector<OnClientCookiesCachedCallback> m_cachedCallbacks;
};

CookiesPlugin g_cookiesPlugin;

void OnClientAuthenticatedListener(int32_t playerSlot, uint64_t steamid64) {
	try {
		g_cookiesPlugin.HandleClientAuthenticated(playerSlot, steamid64);
	} catch (const std::exception& ex) {
		LogError(std::string("cookies: exception in OnClientAuthenticated: ") + ex.what());
	} catch (...) {
		LogError("cookies: unknown exception in OnClientAuthenticated");
	}
}

// Map lifecycle: only local state is touched (a client query here would crash).
void OnMapStartListener() {
	try {
		g_cookiesPlugin.HandleMapStart();
	} catch (...) {
	}
}

void OnMapEndListener() {
	try {
		g_cookiesPlugin.HandleMapEnd();
	} catch (...) {
	}
}

void OnClientDisconnectListener(int32_t playerSlot, s2sdk::NetworkDisconnectionReason reason) {
	(void)reason;
	try {
		g_cookiesPlugin.HandleClientDisconnect(playerSlot);
	} catch (const std::exception& ex) {
		LogError(std::string("cookies: exception in OnClientDisconnect: ") + ex.what());
	} catch (...) {
		LogError("cookies: unknown exception in OnClientDisconnect");
	}
}

bool CookiesPlugin::LoadConfig(std::string& error) {
	if (!plg::GetConfigsDir) {
		error = "cookies: plugify config directory is not available";
		return false;
	}

	const std::string configDir = JoinPath(std::string(plg::GetConfigsDir()), "cookies");
	const std::string configPath = JoinPath(configDir, "config.jsonc");

	std::error_code ec;
	if (!std::filesystem::exists(configPath, ec)) {
		std::filesystem::create_directories(configDir, ec);
		if (ec) {
			error = "cookies: cannot create config directory '" + configDir + "': " + ec.message();
			return false;
		}
		std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
		if (!out) {
			error = "cookies: cannot create default config '" + configPath + "'";
			return false;
		}
		out << kDefaultConfig;
		out.close();
		if (!out) {
			error = "cookies: failed to write default config '" + configPath + "'";
			return false;
		}
		LogInfo("cookies: wrote default config " + configPath);
	}

	std::ifstream in(configPath, std::ios::binary);
	if (!in) {
		error = "cookies: cannot open config '" + configPath + "'";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	// The connection comes from databases.cfg (PLUGIN_STANDARD.md section 4). A server whose
	// config.jsonc still holds the pre-standard keys keeps working; that path produces one
	// migration warning, which is logged here.
	const std::string dataDir = plg::GetDataDir ? std::string(plg::GetDataDir()) : std::string();
	cookies::DatabaseConfig database;
	std::string warning;
	if (!cookies::LoadDatabaseConfig(configPath, text, dataDir, database, warning, error)) {
		error = "cookies: " + error;
		return false;
	}
	if (!warning.empty()) {
		LogWarning("cookies: " + warning);
	}
	m_dbConfig = std::move(database.db);
	return true;
}

bool CookiesPlugin::CreateDatabase(std::string& error) {
	m_db = plgdb::Database::Create(m_dbConfig);
	if (!m_db) {
		error = "cookies: failed to create the database handle (invalid config)";
		return false;
	}

	m_store = std::make_unique<cookies::CookieStore>(*m_db);
	m_store->onError = [](std::string_view message) { LogError(message); };
	m_store->onCached = [](int slot) { g_cookiesPlugin.DispatchCached(slot); };

	try {
		m_store->EnsureSchema();
	} catch (const std::exception& ex) {
		error = ex.what();
		return false;
	}
	return true;
}

bool CookiesPlugin::RegisterListeners(std::string& error) {
	// The plugify module fills the __s2sdk_* slots from the s2sdk plugin declared as our
	// dependency; without them the plugin cannot observe clients at all.
	if (!__s2sdk_OnClientAuthenticated_Register || !__s2sdk_OnClientDisconnect_Register) {
		error = "cookies: s2sdk bindings are not available (is the s2sdk plugin loaded?)";
		return false;
	}
	s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedListener);
	s2sdk::OnClientDisconnect_Register(&OnClientDisconnectListener);
	if (__s2sdk_OnMapStart_Register && __s2sdk_OnMapEnd_Register) {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
	}
	m_listenersRegistered = true;
	return true;
}

void CookiesPlugin::HandleMapStart() {
	m_mapReady = true;
	m_mapStartSeen = true;
	m_mapEndSeen = false;
}

void CookiesPlugin::HandleMapEnd() {
	m_mapReady = false;
	m_mapStartSeen = false;
	m_mapEndSeen = true;
}

void CookiesPlugin::UnregisterListeners() {
	if (!m_listenersRegistered) {
		return;
	}
	m_listenersRegistered = false;
	if (__s2sdk_OnClientAuthenticated_Unregister) {
		s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedListener);
	}
	if (__s2sdk_OnClientDisconnect_Unregister) {
		s2sdk::OnClientDisconnect_Unregister(&OnClientDisconnectListener);
	}
	if (__s2sdk_OnMapStart_Unregister) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

void CookiesPlugin::ReconcileClients() {
	for (int32_t slot = 0; slot < cookies::CookieStore::kMaxSlots; ++slot) {
		try {
			const uint64_t bound = m_store->SteamId(slot);
			if (bound != 0 && !s2sdk::IsClientConnected(slot)) {
				m_store->OnClientDisconnect(slot); // missed disconnect event
				continue;
			}
			if (!s2sdk::IsClientAuthorized(slot) || s2sdk::IsFakeClient(slot)) {
				continue;
			}
			const uint64_t steamid64 = s2sdk::GetClientSteamID64(slot);
			if (steamid64 != 0 && steamid64 != bound) {
				m_store->OnClientAuthenticated(slot, steamid64);
			}
		} catch (...) {
			LogError("cookies: failed to reconcile slot " + std::to_string(slot));
		}
	}
}

void CookiesPlugin::DispatchCached(int32_t playerSlot) {
	const std::vector<OnClientCookiesCachedCallback> snapshot(m_cachedCallbacks);
	for (const OnClientCookiesCachedCallback callback : snapshot) {
		if (!callback) {
			continue;
		}
		try {
			callback(playerSlot);
		} catch (...) {
			// Foreign callback: never let it take the server down or stop the dispatch.
			LogError("cookies: an OnClientCookiesCached callback threw an exception");
		}
	}
}

plg::PluginResult CookiesPlugin::OnPluginStart() {
	try {
		// Fail soft: a Failed result from OnPluginStart stops every Plugify plugin on the server,
		// so a config/database/binding problem leaves this plugin inert
		// (m_store stays null - every export checks it) and logs why.
		std::string error;
		if (!LoadConfig(error) || !CreateDatabase(error) || !RegisterListeners(error)) {
			LogError(error + " - cookies stays inactive");
			m_store.reset();
			return {};
		}

		m_reconcileEnabled = true;
		m_sinceReconcile = std::chrono::seconds(1); // first reconcile on the first update
		LogInfo("cookies: started (driver: " +
			std::string(m_dbConfig.driver == plgdb::Driver::MySQL ? "mysql" : "sqlite") + ")");
		return {};
	} catch (const std::exception& ex) {
		LogError(std::string("cookies: OnPluginStart failed: ") + ex.what() + " - cookies stays inactive");
		m_store.reset();
		return {};
	} catch (...) {
		LogError("cookies: OnPluginStart failed with an unknown exception - cookies stays inactive");
		m_store.reset();
		return {};
	}
}

plg::PluginResult CookiesPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	// The first update proves a live map (a plugin loaded mid-map never saw an OnMapStart); after
	// an OnMapEnd only a new OnMapStart re-opens the gate.
	if (!m_mapReady && (m_mapStartSeen || !m_mapEndSeen)) {
		m_mapReady = true;
	}
	if (m_reconcileEnabled && m_store && m_mapReady) {
		m_sinceReconcile += dt;
		if (m_sinceReconcile >= std::chrono::seconds(1)) {
			m_sinceReconcile = std::chrono::milliseconds(0);
			ReconcileClients();
		}
	}
	if (m_db) {
		try {
			m_db->Poll();
		} catch (const std::exception& ex) {
			LogError(std::string("cookies: database poll failed: ") + ex.what());
		} catch (...) {
			LogError("cookies: database poll failed with an unknown exception");
		}
	}
	return {};
}

plg::PluginResult CookiesPlugin::OnPluginEnd() {
	try {
		UnregisterListeners();
		m_cachedCallbacks.clear();
		m_store.reset();
		m_db.reset(); // flushes pending writes on the worker and joins it
	} catch (const std::exception& ex) {
		LogError(std::string("cookies: OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("cookies: OnPluginEnd failed with an unknown exception");
	}
	return {};
}

void CookiesPlugin::HandleClientAuthenticated(int32_t playerSlot, uint64_t steamid64) {
	if (!m_store) {
		return;
	}
	if (s2sdk::IsFakeClient(playerSlot)) {
		return; // bots have no persistent identity
	}
	m_store->OnClientAuthenticated(playerSlot, steamid64);
}

void CookiesPlugin::HandleClientDisconnect(int32_t playerSlot) {
	if (m_store) {
		m_store->OnClientDisconnect(playerSlot);
	}
}

void CookiesPlugin::SetCookie(int32_t playerSlot, std::string_view name, std::string_view value) {
	if (!m_store) {
		return;
	}
	// A missed auth or disconnect event can leave the slot bound to its previous owner; the write
	// below would then land on that player's row and be dropped when the slot rebinds. While a map
	// is up, settle the binding first, the same way the reconcile pass does (client queries crash
	// without a live map).
	const uint64_t bound = m_store->SteamId(playerSlot);
	if (m_mapReady) {
		if (bound != 0) {
			if (!s2sdk::IsClientAuthorized(playerSlot)) {
				m_store->OnClientDisconnect(playerSlot); // the bound client is gone (or is a bot)
			} else {
				const uint64_t steamid64 = s2sdk::GetClientSteamID64(playerSlot);
				if (steamid64 != 0 && steamid64 != bound) {
					m_store->OnClientAuthenticated(playerSlot, steamid64);
				}
			}
		} else {
			// The engine knows the SteamID of a client that connected but is not authorized yet
			// (and of one that is disconnecting); its auth event has not bound the slot. Bind it
			// here so the value is not queued for whoever lands on the slot later.
			const uint64_t steamid64 = s2sdk::GetClientSteamID64(playerSlot);
			if (steamid64 != 0 && !s2sdk::IsFakeClient(playerSlot)) {
				m_store->OnClientAuthenticated(playerSlot, steamid64);
			}
		}
	}
	m_store->Set(playerSlot, std::string(name), std::string(value));
}

std::string CookiesPlugin::GetCookie(int32_t playerSlot, std::string_view name) const {
	if (!m_store) {
		return {};
	}
	return m_store->Get(playerSlot, name);
}

bool CookiesPlugin::AreCached(int32_t playerSlot) const {
	return m_store && m_store->IsCached(playerSlot);
}

void CookiesPlugin::RegisterCachedCallback(OnClientCookiesCachedCallback callback) {
	if (!callback) {
		return;
	}
	for (const OnClientCookiesCachedCallback existing : m_cachedCallbacks) {
		if (existing == callback) {
			return; // duplicates ignored
		}
	}
	m_cachedCallbacks.push_back(callback);
}

void CookiesPlugin::UnregisterCachedCallback(OnClientCookiesCachedCallback callback) {
	std::erase(m_cachedCallbacks, callback);
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_cookiesPlugin)

PLUGIFY_WARN_PUSH()

#if defined(__clang__)
PLUGIFY_WARN_IGNORE("-Wreturn-type-c-linkage")
#elif defined(_MSC_VER)
PLUGIFY_WARN_IGNORE(4190)
#endif

extern "C" PLUGIN_API void SetClientCookie(const int32_t playerSlot, const plg::string& name,
										   const plg::string& value) {
	try {
		g_cookiesPlugin.SetCookie(playerSlot, std::string_view(name), std::string_view(value));
	} catch (...) {
		LogError("cookies: SetClientCookie failed with an exception");
	}
}

extern "C" PLUGIN_API plg::string GetClientCookie(const int32_t playerSlot, const plg::string& name) {
	try {
		return plg::string(g_cookiesPlugin.GetCookie(playerSlot, std::string_view(name)));
	} catch (...) {
		LogError("cookies: GetClientCookie failed with an exception");
		return {};
	}
}

extern "C" PLUGIN_API bool AreClientCookiesCached(const int32_t playerSlot) {
	try {
		return g_cookiesPlugin.AreCached(playerSlot);
	} catch (...) {
		LogError("cookies: AreClientCookiesCached failed with an exception");
		return false;
	}
}

extern "C" PLUGIN_API void OnClientCookiesCached_Register(OnClientCookiesCachedCallback callback) {
	try {
		g_cookiesPlugin.RegisterCachedCallback(callback);
	} catch (...) {
		LogError("cookies: OnClientCookiesCached_Register failed with an exception");
	}
}

extern "C" PLUGIN_API void OnClientCookiesCached_Unregister(OnClientCookiesCachedCallback callback) {
	try {
		g_cookiesPlugin.UnregisterCachedCallback(callback);
	} catch (...) {
		LogError("cookies: OnClientCookiesCached_Unregister failed with an exception");
	}
}

PLUGIFY_WARN_POP()
