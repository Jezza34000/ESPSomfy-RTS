#include <Arduino.h>

#ifndef Network_h
#define Network_h

#define CONNECT_TIMEOUT 20000
class Network {
  protected:
    unsigned long lastEmit = 0;
    unsigned long lastMDNS = 0;
    int lastRSSI = 0;
    int lastChannel = 0;
    int linkSpeed = 0;
    bool _connecting = false;
  public:
    bool ethStarted = false;
    bool wifiFallback = false;
    bool softAPOpened = false;
    bool openingSoftAP = false;
    bool needsBroadcast = true;
    conn_types connType = conn_types::unset;
    conn_types connTarget = conn_types::unset;
    bool connected();
    bool connecting();
    String ssid;
    String mac;
    int channel;
    int strength;
    int disconnected = 0;
    int connectAttempts = 0;
    uint32_t connectStart = 0;
    uint32_t connectTime = 0;
    bool openSoftAP();
    bool connect();
    bool connectWiFi();
    bool connectWired();
    void setConnected(conn_types connType);
    bool getStrongestAP(const char *ssid, uint8_t *bssid, int32_t *channel);
    bool changeAP(const uint8_t *bssid, const int32_t channel);
    //int getStrengthByMac(const char *mac);
    //int getStrengthBySSID(const char *ssid);
    void updateHostname();
    bool setup();
    void loop();
    void end();
    void emitSockets();
    void emitSockets(uint8_t num);
    void emitHeap(uint8_t num = 255);
    uint32_t getChipId();
    static void networkEvent(WiFiEvent_t event);
};
#endif
