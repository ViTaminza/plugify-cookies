#include "cookies.hpp"

cookies::_SetClientCookie __cookies_SetClientCookie = nullptr;

cookies::_GetClientCookie __cookies_GetClientCookie = nullptr;

cookies::_AreClientCookiesCached __cookies_AreClientCookiesCached = nullptr;

cookies::_OnClientCookiesCached_Register __cookies_OnClientCookiesCached_Register = nullptr;

cookies::_OnClientCookiesCached_Unregister __cookies_OnClientCookiesCached_Unregister = nullptr;

