#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from cookies.pplugin (group: callbacks)

namespace cookies {

  using _OnClientCookiesCached_Register = void (*)(ClientCookiesCachedCallback);
}
extern "C" PLUGIN_API cookies::_OnClientCookiesCached_Register __cookies_OnClientCookiesCached_Register;
namespace cookies {
  /**
   * @brief Registers a callback that is invoked on the game thread once a client's cookies have been loaded from the database (the equivalent of the original HookClientCookieLoaded). Duplicate registrations of the same function are ignored.
   * @param callback (function): Function to invoke.
   */
  inline void OnClientCookiesCached_Register(ClientCookiesCachedCallback callback) {
    return __cookies_OnClientCookiesCached_Register(callback);
  }

  using _OnClientCookiesCached_Unregister = void (*)(ClientCookiesCachedCallback);
}
extern "C" PLUGIN_API cookies::_OnClientCookiesCached_Unregister __cookies_OnClientCookiesCached_Unregister;
namespace cookies {
  /**
   * @brief Removes a callback previously passed to OnClientCookiesCached_Register. Safe to call from inside the callback itself.
   * @param callback (function): Function to remove.
   */
  inline void OnClientCookiesCached_Unregister(ClientCookiesCachedCallback callback) {
    return __cookies_OnClientCookiesCached_Unregister(callback);
  }

} // namespace cookies
