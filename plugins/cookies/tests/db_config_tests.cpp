// Tests for plugins/cookies/src/db_config.hpp - where the plugin takes its database connection
// from (PLUGIN_STANDARD.md section 4) and the transition-period fallback to config.jsonc.
//
// The pisexcompat path mapping is exercised for real: <GetConfigsDir()> is pointed at a throwaway
// directory, so LoadDatabaseConfig() has to find <configsDir>/cookies/databases.cfg and must not
// pick up a databases.cfg in the root of the config directory.
//
// Run by ../test.sh from build/tests.

#include <plg/plugin.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

#include "db_config.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN (they are filled in by the plugify module). The
// test serves the config/data directories from the throwaway directory of the run and leaves the
// rest null, like tests/pisexcompat_tests.cpp.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

std::string g_configsDir;
std::string g_dataDir;

plg::string TestConfigsDir() {
	return plg::string(g_configsDir.c_str());
}

plg::string TestDataDir() {
	return plg::string(g_dataDir.c_str());
}

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                        \
	do {                                                                                   \
		++g_checks;                                                                        \
		if (!(expr)) {                                                                     \
			++g_failures;                                                                  \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),     \
						g_section.data(), __FILE__, __LINE__, #expr);                      \
		}                                                                                  \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                     \
	do {                                                                                   \
		++g_checks;                                                                        \
		const std::string a_ = (actual);                                                   \
		const std::string e_ = (expected);                                                 \
		if (a_ != e_) {                                                                    \
			++g_failures;                                                                  \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,   \
						__LINE__, a_.c_str(), e_.c_str());                                 \
		}                                                                                  \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                     \
	do {                                                                                   \
		++g_checks;                                                                        \
		const long long a_ = static_cast<long long>(actual);                               \
		const long long e_ = static_cast<long long>(expected);                             \
		if (a_ != e_) {                                                                    \
			++g_failures;                                                                  \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,   \
						__LINE__, a_, e_);                                                 \
		}                                                                                  \
	} while (false)

#define CHECK_CONTAINS(haystack, needle)                                                   \
	do {                                                                                   \
		++g_checks;                                                                        \
		const std::string h_ = (haystack);                                                 \
		if (h_.find(needle) == std::string::npos) {                                        \
			++g_failures;                                                                  \
			std::printf("FAIL [%.*s] %s:%d: \"%s\" does not contain \"%s\"\n",             \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,   \
						__LINE__, h_.c_str(), std::string(needle).c_str());                \
		}                                                                                  \
	} while (false)

// --- helpers -----------------------------------------------------------------

constexpr std::string_view kLegacyConfig = R"({
	"driver": "mysql",
	"host": "legacy.example.com",
	"port": 3308,
	"user": "legacy_user",
	"password": "legacy_pass",
	"database": "legacy_db"
})";

std::string LegacyPath() {
	return (std::filesystem::path(g_configsDir) / "cookies" / "config.jsonc").string();
}

std::string DatabasesCfgPath() {
	return (std::filesystem::path(g_configsDir) / "cookies" / "databases.cfg").string();
}

void WriteDatabasesCfg(std::string_view text) {
	std::ofstream out(DatabasesCfgPath(), std::ios::binary | std::ios::trunc);
	out << text;
}

void RemoveFile(const std::string& path) {
	std::error_code ec;
	std::filesystem::remove(path, ec);
}

// --- tests -------------------------------------------------------------------

void TestDatabasesCfgWins() {
	Section("databases.cfg wins over config.jsonc");

	WriteDatabasesCfg(R"("Databases"
{
	"cookies"
	{
		"driver"	"mysql"
		"host"		"db.example.com"
		"port"		"3307"
		"user"		"ck_user"
		"pass"		"ck_pass"
		"database"	"ck_db"
	}
}
)");
	// A decoy in the root of the config directory must be ignored: the standard keeps the
	// connection in the plugin's own directory.
	std::ofstream(std::filesystem::path(g_configsDir) / "databases.cfg")
		<< R"("Databases" { "cookies" { "host" "root-decoy.example.com" } })";

	cookies::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(cookies::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK_EQ_STR(warning, "");
	CHECK(config.source == cookies::DatabaseSource::DatabasesCfg);
	CHECK(config.db.driver == plgdb::Driver::MySQL);
	CHECK_EQ_STR(config.db.host, "db.example.com");
	CHECK_EQ_INT(config.db.port, 3307);
	CHECK_EQ_STR(config.db.user, "ck_user");
	CHECK_EQ_STR(config.db.password, "ck_pass");
	CHECK_EQ_STR(config.db.database, "ck_db");
}

void TestMissingDatabasesCfgFallsBack() {
	Section("missing databases.cfg -> config.jsonc + one warning");

	RemoveFile(DatabasesCfgPath());
	RemoveFile((std::filesystem::path(g_configsDir) / "databases.cfg").string());

	cookies::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(cookies::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == cookies::DatabaseSource::LegacyConfig);
	CHECK(config.db.driver == plgdb::Driver::MySQL);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK_EQ_INT(config.db.port, 3308);
	CHECK_EQ_STR(config.db.user, "legacy_user");
	CHECK_EQ_STR(config.db.password, "legacy_pass");
	CHECK_EQ_STR(config.db.database, "legacy_db");
	CHECK_CONTAINS(warning, "databases.cfg");
	CHECK_CONTAINS(warning, "cookies"); // the section the connection has to move into
	CHECK_CONTAINS(warning, "move the connection");
}

void TestMissingSectionFallsBack() {
	Section("databases.cfg without the 'cookies' section -> fallback + one warning");

	WriteDatabasesCfg(R"("Databases"
{
	"admin_system"
	{
		"host"		"other.example.com"
	}
}
)");

	cookies::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(cookies::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == cookies::DatabaseSource::LegacyConfig);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK(config.db.driver == plgdb::Driver::MySQL);
	CHECK_CONTAINS(warning, "'cookies' section");
	CHECK_CONTAINS(warning, "databases.cfg");
}

void TestSqliteSection() {
	Section("sqlite connection: the path is resolved against the data directory");

	WriteDatabasesCfg(R"("Databases"
{
	"cookies"
	{
		"driver"	"sqlite"
		"sqlite_path"	"ck.sqlite3"
	}
}
)");

	cookies::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(cookies::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK_EQ_STR(warning, "");
	CHECK(config.source == cookies::DatabaseSource::DatabasesCfg);
	CHECK(config.db.driver == plgdb::Driver::SQLite);
	CHECK_EQ_STR(config.db.sqlitePath, (std::filesystem::path(g_dataDir) / "ck.sqlite3").string());

	// ":memory:" is kept as given.
	WriteDatabasesCfg(R"("Databases" { "cookies" { "driver" "sqlite" "sqlite_path" ":memory:" } })");
	CHECK(cookies::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(config.db.sqlitePath, ":memory:");
}

void TestLegacyDefaults() {
	Section("the shipped config.jsonc (no database keys) still means SQLite");

	RemoveFile(DatabasesCfgPath());

	// The file package.sh ships: a comment pointing at databases.cfg, no keys at all.
	constexpr std::string_view kShippedConfig = R"({
	// The database connection of this plugin is read from configs/cookies/databases.cfg
	// (section "cookies", see docs/PLUGIN_STANDARD.md section 4). This file keeps the
	// non-database options of the plugin; there are none yet.
})";

	cookies::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(cookies::LoadDatabaseConfig(LegacyPath(), kShippedConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == cookies::DatabaseSource::LegacyConfig);
	CHECK(config.db.driver == plgdb::Driver::SQLite);
	CHECK_EQ_STR(config.db.sqlitePath, (std::filesystem::path(g_dataDir) / "cookies.sqlite3").string());
	CHECK(!warning.empty());
}

void TestBrokenLegacyConfigFails() {
	Section("a broken config.jsonc is still a hard error");

	RemoveFile(DatabasesCfgPath());

	cookies::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(!cookies::LoadDatabaseConfig(LegacyPath(), "{ \"driver\": ", g_dataDir, config, warning, error));
	CHECK_CONTAINS(error, "config parse error");
	CHECK_CONTAINS(error, "config.jsonc");

	// An unknown driver in the legacy file is an error too, as before the standard.
	CHECK(!cookies::LoadDatabaseConfig(LegacyPath(), R"({ "driver": "postgres" })", g_dataDir, config, warning, error));
	CHECK_CONTAINS(error, "unknown 'driver'");
}

} // namespace

int main() {
	const std::filesystem::path tempRoot =
		std::filesystem::temp_directory_path() / ("cookies_dbconfig_tests_" + std::to_string(::getpid()));
	std::error_code ec;
	std::filesystem::remove_all(tempRoot, ec);
	std::filesystem::create_directories(tempRoot / "cookies", ec);
	std::filesystem::create_directories(tempRoot / "data", ec);
	if (!std::filesystem::is_directory(tempRoot / "cookies") || !std::filesystem::is_directory(tempRoot / "data")) {
		std::printf("FATAL: cannot create the test directory %s\n", tempRoot.c_str());
		return 1;
	}
	g_configsDir = tempRoot.string();
	g_dataDir = (tempRoot / "data").string();
	plg::GetConfigsDir = &TestConfigsDir;
	plg::GetDataDir = &TestDataDir;

	std::printf("cookies db_config tests (%s)\n", tempRoot.c_str());

	TestDatabasesCfgWins();
	TestMissingDatabasesCfgFallsBack();
	TestMissingSectionFallsBack();
	TestSqliteSection();
	TestLegacyDefaults();
	TestBrokenLegacyConfigFails();

	std::filesystem::remove_all(tempRoot, ec);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
