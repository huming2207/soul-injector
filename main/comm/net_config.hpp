#pragma once

#include <esp_err.h>
#include <manage.pb.h>

/**
 * Network settings in NVS, namespace "si.cfg". The SIDP service reads them
 * once at boot; NET_CONFIG_SET writes them for the next boot.
 */
class net_config
{
public:
    /** Read the stored settings. Missing keys read as empty, and networking as off. */
    static esp_err_t read(si_manage_NetConfig &cfg);

    /**
     * Store @p cfg. The WiFi password and the device password are kept when
     * keep_wifi_password and keep_password are set. ESP_ERR_INVALID_ARG when
     * networking is enabled without an SSID, a ws:// server URL or a device password.
     */
    static esp_err_t write(const si_manage_NetConfig &cfg);

private:
    static const constexpr char NVS_NAMESPACE[] = "si.cfg";
    static const constexpr char KEY_ENABLED[] = "net_en";
    static const constexpr char KEY_SSID[] = "wifi_ssid";
    static const constexpr char KEY_PASSWORD[] = "wifi_pass";
    static const constexpr char KEY_SERVER_URL[] = "srv_url";
    static const constexpr char KEY_SALT[] = "pw_salt";
    static const constexpr char KEY_VERIFIER[] = "pw_verifier";
    static const constexpr char TAG[] = "net_cfg";
};
