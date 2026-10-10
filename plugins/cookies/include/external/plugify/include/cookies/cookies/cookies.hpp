#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from cookies.pplugin (group: cookies)

namespace cookies {

  using _SetClientCookie = void (*)(int32_t, const plg::string&, const plg::string&);
}
extern "C" PLUGIN_API cookies::_SetClientCookie __cookies_SetClientCookie;
namespace cookies {
  /**
   * @brief Stores a cookie for a player. The value is cached immediately and written to the database asynchronously. May be called before the player's cookies have been loaded - the value is then queued and flushed as soon as the client is known, and it is never overwritten by the pending load. Names longer than 255 bytes and values longer than 65535 bytes are rejected (logged).
   * @param playerSlot (int32): Player slot (0..63). Values outside the range are ignored.
   * @param name (string): Cookie name (1..255 bytes).
   * @param value (string): Cookie value (0..65535 bytes).
   */
  inline void SetClientCookie(int32_t playerSlot, const plg::string& name, const plg::string& value) {
    return __cookies_SetClientCookie(playerSlot, name, value);
  }

  using _GetClientCookie = plg::string (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API cookies::_GetClientCookie __cookies_GetClientCookie;
namespace cookies {
  /**
   * @brief Returns the cached value of a cookie. Returns an empty string when the cookie does not exist, the cookies of the player have not been loaded yet, or the slot is invalid - matching the behaviour of the original plugin.
   * @param playerSlot (int32): Player slot (0..63).
   * @param name (string): Cookie name.
   * @return string: Cookie value or an empty string.
   */
  inline plg::string GetClientCookie(int32_t playerSlot, const plg::string& name) {
    return __cookies_GetClientCookie(playerSlot, name);
  }

  using _AreClientCookiesCached = bool (*)(int32_t);
}
extern "C" PLUGIN_API cookies::_AreClientCookiesCached __cookies_AreClientCookiesCached;
namespace cookies {
  /**
   * @brief Returns true once the cookies of the player have been loaded from the database (or the load failed - the callback is still fired).
   * @param playerSlot (int32): Player slot (0..63).
   * @return bool: Whether the client's cookies are cached.
   */
  inline bool AreClientCookiesCached(int32_t playerSlot) {
    return __cookies_AreClientCookiesCached(playerSlot);
  }

} // namespace cookies
