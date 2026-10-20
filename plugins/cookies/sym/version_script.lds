{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* exported methods */
        SetClientCookie;
        GetClientCookie;
        AreClientCookiesCached;
        OnClientCookiesCached_Register;
        OnClientCookiesCached_Unregister;

    local: *;
};
