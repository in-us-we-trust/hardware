#include "Storage.h"
#include <Preferences.h>

namespace
{
    Preferences prefs;
}

namespace Storage
{
    String loadDeviceId()
    {
        prefs.begin("device", true);
        String id = prefs.getString("uuid", "");
        prefs.end();
        return id;
    }

    void saveDeviceId(const String &id)
    {
        prefs.begin("device", false);
        prefs.putString("uuid", id);
        prefs.end();
    }

    bool loadDeviceRegistered()
    {
        prefs.begin("device", true);
        bool registered = prefs.getBool("registered", false);
        prefs.end();
        return registered;
    }

    void saveDeviceRegistered(bool registered)
    {
        prefs.begin("device", false);
        prefs.putBool("registered", registered);
        prefs.end();
    }

    void saveWifiCredentials(const String &ssid, const String &password)
    {
        prefs.begin("wifi", false);
        prefs.putString("ssid", ssid);
        prefs.putString("password", password);
        prefs.end();
    }

    String loadWifiSSID()
    {
        prefs.begin("wifi", true);
        String ssid = prefs.getString("ssid", "");
        prefs.end();
        return ssid;
    }

    String loadWifiPassword()
    {
        prefs.begin("wifi", true);
        String password = prefs.getString("password", "");
        prefs.end();
        return password;
    }
}
