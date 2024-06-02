#include <ETH.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include "ConfigSettings.h"
#include "Network.h"
#include "Web.h"
#include "Sockets.h"
#include "Utils.h"
#include "SSDP.h"
#include "MQTT.h"

extern ConfigSettings settings;
extern Web webServer;
extern SocketEmitter sockEmit;
extern MQTTClass mqtt;
extern rebootDelay_t rebootDelay;
extern Network net;
extern SomfyShadeController somfy;

static bool _apScanning = false;
static uint32_t _lastMaxHeap = 0;
static uint32_t _lastHeap = 0;
int connectRetries = 0;
void Network::end() {
  SSDP.end();
  mqtt.end();
  sockEmit.end();
  delay(100);
}
bool Network::setup() {
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.persistent(false);
  WiFi.onEvent(this->networkEvent);
  if(WiFi.status() == WL_CONNECTED) WiFi.disconnect(true, true);
  if(settings.connType == conn_types::wifi || settings.connType == conn_types::unset) {
    WiFi.persistent(false);
    if(settings.hostname[0] != '\0') WiFi.setHostname(settings.hostname);
    Serial.print("WiFi Mode: ");
    Serial.println(WiFi.getMode());
    WiFi.mode(WIFI_STA);
    //settings.WIFI.printNetworks();
  }
  sockEmit.begin();
  return true;
}
void Network::loop() {
  this->connect();
  if(!this->connected() || this->connecting()) return;  
  if(millis() - this->lastEmit > 1500) {
    this->lastEmit = millis();
    if(this->connected()) {
      this->emitSockets();
      this->lastEmit = millis();
    }
  }
  sockEmit.loop();
  if(this->connected() && millis() - this->lastMDNS > 60000) {
    // Every 60 seconds we are going to look at wifi connectivity
    // to get around the roaming issues with ESP32.  We will try to do this in an async manner.  If
    // there is a channel that is better we will stop the wifi radio and reconnect
    if(this->connType == conn_types::wifi && settings.WIFI.roaming && !this->softAPOpened) {
      // If we are not already scanning then we need to start a passive scan
      // and only respond if there is a better connection. 
      // 1. If there is currently a waiting scan don't do anything
      if(!_apScanning && WiFi.scanNetworks(true, false, true, 300, 0, settings.WIFI.ssid) == -1) {
        _apScanning = true;
      }
    }
    this->lastMDNS = millis();
  }
  if(_apScanning) {
    if(!settings.WIFI.roaming || this->connType != conn_types::wifi || this->softAPOpened) _apScanning = false;
    else {
      uint16_t n = WiFi.scanComplete();
      if( n > 0) {
        uint8_t bssid[6];
        int32_t channel = 0;
        if(this->getStrongestAP(settings.WIFI.ssid, bssid, &channel)) {
          if(memcmp(bssid, WiFi.BSSID(), sizeof(bssid)) != 0) {
            Serial.printf("Found stronger AP %d %02X:%02X:%02X:%02X:%02X:%02X\n", channel, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
            this->changeAP(bssid, channel);
          }
        }
        _apScanning = false;
      }
    }
  }
  if(settings.ssdpBroadcast) {
    if(!SSDP.isStarted) SSDP.begin();
    if(SSDP.isStarted) SSDP.loop();
  }
  else if(!settings.ssdpBroadcast && SSDP.isStarted) SSDP.end();
  mqtt.loop();
}
bool Network::changeAP(const uint8_t *bssid, const int32_t channel) {
  esp_task_wdt_reset(); // Make sure we do not reboot here.
  if(SSDP.isStarted) SSDP.end();
  mqtt.disconnect();
  sockEmit.end();
  WiFi.disconnect(false, true);
  WiFi.begin(settings.WIFI.ssid, settings.WIFI.passphrase, channel, bssid);
  this->connectStart = millis();
  return false;
}
void Network::emitSockets() {
  this->emitHeap();
  if(this->needsBroadcast || 
    (this->connType == conn_types::wifi && (abs(abs(WiFi.RSSI()) - abs(this->lastRSSI)) > 1 || WiFi.channel() != this->lastChannel))) {
    this->emitSockets(255);
    sockEmit.loop();
    this->needsBroadcast = false;
  }
}
void Network::emitSockets(uint8_t num) {
  //char buf[128];
  if(this->connType == conn_types::ethernet) {
      JsonSockEvent *json = sockEmit.beginEmit("ethernet");
      json->beginObject();
      json->addElem("connected", this->connected());
      json->addElem("speed", ETH.linkSpeed());
      json->addElem("fullduplex", ETH.fullDuplex());
      json->endObject();
      sockEmit.endEmit(num);
      /*
      snprintf(buf, sizeof(buf), "{\"connected\":%s,\"speed\":%d,\"fullduplex\":%s}", this->connected() ? "true" : "false", ETH.linkSpeed(), ETH.fullDuplex() ? "true" : "false");
      if(num == 255) 
        sockEmit.sendToClients("ethernet", buf);
      else
        sockEmit.sendToClient(num, "ethernet", buf);
      */
  }
  else {
      if(WiFi.status() == WL_CONNECTED) {
        JsonSockEvent *json = sockEmit.beginEmit("wifiStrength");
        json->beginObject();
        json->addElem("ssid", WiFi.SSID().c_str());
        json->addElem("strength", (int32_t)WiFi.RSSI());
        json->addElem("channel", (int32_t)this->channel);
        json->endObject();
        sockEmit.endEmit(num);
        /*
        snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"strength\":%d,\"channel\":%d}", WiFi.SSID().c_str(), WiFi.RSSI(), this->channel);
        if(num == 255)
          sockEmit.sendToClients("wifiStrength", buf);
        else
          sockEmit.sendToClient(num, "wifiStrength", buf);
        */
        this->lastRSSI = WiFi.RSSI();
        this->lastChannel = WiFi.channel();
      }
      else {
        JsonSockEvent *json = sockEmit.beginEmit("wifiStrength");
        json->beginObject();
        json->addElem("ssid", "");
        json->addElem("strength", (int8_t)-100);
        json->addElem("channel", (int8_t)-1);
        json->endObject();
        sockEmit.endEmit(num);
        
        json = sockEmit.beginEmit("ethernet");
        json->beginObject();
        json->addElem("connected", false);
        json->addElem("speed", (uint8_t)0);
        json->addElem("fullduplex", false);
        json->endObject();
        sockEmit.endEmit(num);
        /*

        if(num == 255) {
          sockEmit.sendToClients("wifiStrength", "{\"ssid\":\"\", \"strength\":-100,\"channel\":-1}");
          sockEmit.sendToClients("ethernet", "{\"connected\":false,\"speed\":0,\"fullduplex\":false}");
        }
        else {
          sockEmit.sendToClient(num, "wifiStrength", "{\"ssid\":\"\", \"strength\":-100,\"channel\":-1}");
          sockEmit.sendToClient(num, "ethernet", "{\"connected\":false,\"speed\":0,\"fullduplex\":false}");
        }
        */
        this->lastRSSI = -100;
        this->lastChannel = -1;
      }
  }
  this->emitHeap(num);
}
void Network::setConnected(conn_types connType) {
  this->connType = connType;
  this->connectTime = millis();
  connectRetries = 0;
  if(this->connType == conn_types::wifi) {
    if(this->softAPOpened && WiFi.softAPgetStationNum() == 0) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
    }
    this->_connecting = false;
    this->ssid = WiFi.SSID();
    this->mac = WiFi.BSSIDstr();
    this->strength = WiFi.RSSI();
    this->channel = WiFi.channel();
    this->connectAttempts++;
  }
  else if(this->connType == conn_types::ethernet) {
    if(this->softAPOpened) {
      Serial.println("Disonnecting from SoftAP");
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    this->connectAttempts++;
    this->_connecting = false;
    this->wifiFallback = false;
  }
  // NET: Begin this in the startup.
  //sockEmit.begin();
  if(this->connectAttempts == 1) {
    Serial.println();
    if(this->connType == conn_types::wifi) {
      Serial.print("Successfully Connected to WiFi!!!!");
      Serial.print(WiFi.localIP());
      Serial.print(" (");
      Serial.print(this->strength);
      Serial.println("dbm)");
      if(settings.IP.dhcp) {
        settings.IP.ip = WiFi.localIP();
        settings.IP.subnet = WiFi.subnetMask();
        settings.IP.gateway = WiFi.gatewayIP();
        settings.IP.dns1 = WiFi.dnsIP(0);
        settings.IP.dns2 = WiFi.dnsIP(1);
      }
    }
    else {
      Serial.print("Successfully Connected to Ethernet!!! ");
      Serial.print(ETH.localIP());
      if(ETH.fullDuplex()) {
        Serial.print(" FULL DUPLEX");
      }
      Serial.print(" ");
      Serial.print(ETH.linkSpeed());
      Serial.println("Mbps");
      if(settings.IP.dhcp) {
        settings.IP.ip = ETH.localIP();
        settings.IP.subnet = ETH.subnetMask();
        settings.IP.gateway = ETH.gatewayIP();
        settings.IP.dns1 = ETH.dnsIP(0);
        settings.IP.dns2 = ETH.dnsIP(1);
      }
      JsonSockEvent *json = sockEmit.beginEmit("ethernet");
      json->beginObject();
      json->addElem("connected", this->connected());
      json->addElem("speed", ETH.linkSpeed());
      json->addElem("fullduplex", ETH.fullDuplex());
      json->endObject();
      sockEmit.endEmit();
    }
  }
  else {
    Serial.println();
    Serial.print("Reconnected after ");
    Serial.print(1.0 * (millis() - this->connectStart)/1000);
      Serial.print("sec IP: ");
    if(this->connType == conn_types::wifi) {
      Serial.print(WiFi.localIP());
      Serial.print(" ");
      Serial.print(this->mac);
      Serial.print(" CH:");
      Serial.print(this->channel);
      Serial.print(" (");
      Serial.print(this->strength);
      Serial.print(" dBm)");
    }
    else {
      Serial.print(ETH.localIP());
      if(ETH.fullDuplex()) {
        Serial.print(" FULL DUPLEX");
      }
      Serial.print(" ");
      Serial.print(ETH.linkSpeed());
      Serial.print("Mbps");
    }
    Serial.print(" Disconnected ");
    Serial.print(this->connectAttempts - 1);
    Serial.println(" times");
  }
  SSDP.setHTTPPort(80);
  SSDP.setSchemaURL(0, "upnp.xml");
  SSDP.setChipId(0, this->getChipId());
  SSDP.setDeviceType(0, "urn:schemas-rstrouse-org:device:ESPSomfyRTS:1");
  SSDP.setName(0, settings.hostname);
  
  //SSDP.setSerialNumber(0, "C2496952-5610-47E6-A968-2FC19737A0DB");
  //SSDP.setUUID(0, settings.uuid);
  SSDP.setModelName(0, "ESPSomfy RTS");
  if(strlen(settings.chipModel) == 0) SSDP.setModelNumber(0, "ESP32");
  else {
    char sModel[20] = "";
    snprintf(sModel, sizeof(sModel), "ESP32-%s", settings.chipModel);
    SSDP.setModelNumber(0, sModel);
  }
  SSDP.setModelURL(0, "https://github.com/rstrouse/ESPSomfy-RTS");
  SSDP.setManufacturer(0, "rstrouse");
  SSDP.setManufacturerURL(0, "https://github.com/rstrouse");
  SSDP.setURL(0, "/");
  SSDP.setActive(0, true);
  if(MDNS.begin(settings.hostname)) {
    Serial.printf("MDNS Responder Started: serverId=%s\n", settings.serverId);
    MDNS.addService("http", "tcp", 80);
    //MDNS.addServiceTxt("http", "tcp", "board", "ESP32");
    //MDNS.addServiceTxt("http", "tcp", "model", "ESPSomfyRTS");
    
    MDNS.addService("espsomfy_rts", "tcp", 8080);
    MDNS.addServiceTxt("espsomfy_rts", "tcp", "serverId", String(settings.serverId));
    MDNS.addServiceTxt("espsomfy_rts", "tcp", "model", "ESPSomfyRTS");
    MDNS.addServiceTxt("espsomfy_rts", "tcp", "version", String(settings.fwVersion.name));
  }
  if(settings.ssdpBroadcast) {
    SSDP.begin();
  }
  else if(SSDP.isStarted) SSDP.end();
  this->emitSockets();
  settings.printAvailHeap();
  this->needsBroadcast = true;
}
bool Network::connectWired() {
  if(ETH.linkUp()) {
    // If the ethernet link is re-established then we need to shut down wifi.
    if(WiFi.status() == WL_CONNECTED) {
      sockEmit.end();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    if(this->connType != conn_types::ethernet) this->setConnected(conn_types::ethernet);
    this->wifiFallback = false;
    return true;
  }
  else if(this->ethStarted) {
    if(settings.connType == conn_types::ethernetpref && settings.WIFI.ssid[0] != '\0')
      return this->connectWiFi();
  }
  if(this->connectAttempts > 0) {
    Serial.printf("Ethernet Connection Lost... %d Reconnecting ", this->connectAttempts);
    Serial.println(this->mac);
  }
  else
    Serial.println("Connecting to Wired Ethernet");
  this->_connecting = true;
  this->connTarget = conn_types::ethernet;
  this->connType = conn_types::unset;
  if(!this->ethStarted) {
      this->ethStarted = true;
      WiFi.mode(WIFI_OFF);
      if(settings.hostname[0] != '\0') 
        ETH.setHostname(settings.hostname);
      else
        ETH.setHostname("ESPSomfy-RTS");
     
      Serial.print("Set hostname to:");
      Serial.println(ETH.getHostname());
      if(!ETH.begin(settings.Ethernet.phyAddress, settings.Ethernet.PWRPin, settings.Ethernet.MDCPin, settings.Ethernet.MDIOPin, settings.Ethernet.phyType, settings.Ethernet.CLKMode)) { 
          Serial.println("Ethernet Begin failed");
          this->ethStarted = false;
          if(settings.connType == conn_types::ethernetpref) {
            this->wifiFallback = true;
            return connectWiFi();
          }
          return false;
      }
      else {
        if(!settings.IP.dhcp) {
          if(!ETH.config(settings.IP.ip, settings.IP.gateway, settings.IP.subnet, settings.IP.dns1, settings.IP.dns2)) {
              Serial.println("Unable to configure static IP address....");
              ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
          }
        }
        else
            ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
      }
  }
  this->connectStart = millis();
  return true;
}
void Network::updateHostname() {
  if(settings.hostname[0] != '\0' && this->connected()) {
    if(this->connType == conn_types::ethernet &&
      strcmp(settings.hostname, ETH.getHostname()) != 0) {
      Serial.printf("Updating host name to %s...\n", settings.hostname);
      ETH.setHostname(settings.hostname);
      MDNS.setInstanceName(settings.hostname);        
      SSDP.setName(0, settings.hostname);
     }
     else if(strcmp(settings.hostname, WiFi.getHostname()) != 0) {
      Serial.printf("Updating host name to %s...\n", settings.hostname);
      WiFi.setHostname(settings.hostname);
      MDNS.setInstanceName(settings.hostname);        
      SSDP.setName(0, settings.hostname);
     }
  }
}
bool Network::connectWiFi() {
  if(this->softAPOpened && WiFi.softAPgetStationNum() > 0) {
    WiFi.disconnect(false);
    this->_connecting = false;
    this->connType = conn_types::unset;
    return true;
  }

  if(settings.WIFI.ssid[0] != '\0') {
    if(WiFi.status() == WL_CONNECTED && WiFi.SSID().compareTo(settings.WIFI.ssid) == 0) {
      // If we are connected to the target SSID then just return.
      this->disconnected = 0;
      return true;
    }
    if(this->_connecting) return true;
    this->_connecting = true;
    this->connTarget = conn_types::wifi;
    this->connType = conn_types::unset;
    if(this->connectAttempts > 0) {
      Serial.print("Connection Lost...");
      Serial.print(this->mac);
      Serial.print(" CH:");
      Serial.print(this->channel);
      Serial.print(" (");
      Serial.print(this->strength);
      Serial.println("dbm)  ");
    }
    else Serial.println("Connecting to AP");
    // If the soft AP is currently opened then we do not want to kill it.
    WiFi.setSleep(false);
    WiFi.disconnect(false);
    //WiFi.mode(WIFI_MODE_NULL);
    if(!settings.IP.dhcp) {
      if(!WiFi.config(settings.IP.ip, settings.IP.gateway, settings.IP.subnet, settings.IP.dns1, settings.IP.dns2))
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    else
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
    delay(100);
    // There is also another method simply called hostname() but this is legacy for esp8266.
    if(settings.hostname[0] != '\0') WiFi.setHostname(settings.hostname);
    Serial.print("Set hostname to:");
    Serial.println(WiFi.getHostname());
    //WiFi.mode(WIFI_STA);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    uint8_t bssid[6];
    int32_t channel = 0;
    if(this->getStrongestAP(settings.WIFI.ssid, bssid, &channel)) {
      Serial.printf("Found strongest AP %d %02X:%02X:%02X:%02X:%02X:%02X\n", channel, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
      WiFi.begin(settings.WIFI.ssid, settings.WIFI.passphrase, channel, bssid);
    }
    else
      WiFi.begin(settings.WIFI.ssid, settings.WIFI.passphrase);
  }
  this->connectStart = millis();
  return true;
}
bool Network::connect() {
  if(this->connecting()) {
    // We are currently connecting and this flag is triggered while there is an attempt
    // to connect to the network.  If the connection type is set then we need to
    // finish the connection.  If it is not then we need to fall back to AP or in
    // the case where the target was originally ethernet then we need to open the softAP.
    if(this->connType == conn_types::unset) {
      // If we reached our timeout for the connection then we need to open the soft ap.
      if(millis() > this->connectStart + CONNECT_TIMEOUT) {
        esp_task_wdt_reset();
        if(this->connTarget == conn_types::ethernet && settings.connType == conn_types::ethernetpref && settings.WIFI.ssid[0] != '\0')
          this->connectWiFi();
        else if(this->softAPOpened) {
          if(settings.connType == conn_types::ethernet || settings.connType == conn_types::ethernetpref)
            this->connectWired();
          else if(settings.connType == conn_types::wifi && strlen(settings.WIFI.ssid) > 0)
            this->connectWiFi();
        }
        else {
          //Serial.println("Fell into timeout");
          this->openSoftAP();
        }
      }
    }
    else
      this->setConnected(this->connTarget);
  }
  else if(settings.connType == conn_types::ethernet || settings.connType == conn_types::ethernetpref)
    this->connectWired();
  else if(settings.connType == conn_types::wifi && strlen(settings.WIFI.ssid) > 0)
    this->connectWiFi();
  else
    this->openSoftAP();
  if(this->softAPOpened && this->connected() && WiFi.softAPgetStationNum() == 0) {
      Serial.println("Closing uneeded SoftAP");
      WiFi.softAPdisconnect(true);
      if(this->connType == conn_types::wifi) WiFi.mode(WIFI_STA);
  }
  return true;
}
uint32_t Network::getChipId() {
  uint32_t chipId = 0;
  uint64_t mac = ESP.getEfuseMac();
  for(int i=0; i<17; i=i+8) {
    chipId |= ((mac >> (40 - i)) & 0xff) << i;
  }
  return chipId;
}

bool Network::getStrongestAP(const char *ssid, uint8_t *bssid, int32_t *channel) {
  // The new AP must be at least 10dbm greater.
  int32_t strength = this->connected() ? WiFi.RSSI() + 10 : -127;
  int32_t chan = -1;
  memset(bssid, 0x00, 6);
  esp_task_wdt_delete(NULL);
  uint8_t n = this->connected() ? WiFi.scanComplete() : WiFi.scanNetworks(false, false, false, 300, 0, ssid);
  esp_task_wdt_add(NULL);
  for(uint8_t i = 0; i < n; i++) {
    if(WiFi.SSID(i).compareTo(ssid) == 0) {
      if(WiFi.RSSI(i) > strength) { 
        strength = WiFi.RSSI(i); 
        memcpy(bssid, WiFi.BSSID(i), 6); 
        *channel = chan = WiFi.channel(i);
      }
    }
  }  
  WiFi.scanDelete();
  return chan > 0;
}
bool Network::openSoftAP() {
  if(this->softAPOpened || this->openingSoftAP) return true;
  WiFi.disconnect(false);
  this->openingSoftAP = true;
  Serial.println();
  Serial.println("Turning the HotSpot On");
  esp_task_wdt_reset(); // Make sure we do not reboot here.
  WiFi.softAP("ESPSomfy RTS", "");
  Serial.println("Initializing AP for credentials modification");
  delay(200);
  return true;
}
bool Network::connected() {
  if(this->connecting()) return false;
  else if(this->connType == conn_types::unset) return false;
  else if(this->connType == conn_types::wifi) return WiFi.status() == WL_CONNECTED;
  else if(this->connType == conn_types::ethernet) return ETH.linkUp();
  else return this->connType != conn_types::unset;
  return false;
}
bool Network::connecting() {
  return this->_connecting;
}
void Network::networkEvent(WiFiEvent_t event) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_READY:               Serial.println("(evt) WiFi interface ready"); break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE:           Serial.println("(evt) Completed scan for access points"); break;
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("WiFi station mode started");
      if(settings.hostname[0] != '\0') WiFi.setHostname(settings.hostname);
      break;
    case ARDUINO_EVENT_WIFI_STA_STOP:            Serial.println("(evt) WiFi clients stopped"); break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:       Serial.println("(evt) Connected to access point"); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:    Serial.println("(evt) Disconnected from WiFi access point"); break;
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE: Serial.println("(evt) Authentication mode of access point has changed"); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("Got WiFi IP: ");
      Serial.println(WiFi.localIP());
      net.connType = conn_types::wifi;
      net.connectTime = millis();
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:        Serial.println("Lost IP address and IP address is reset to 0"); break;    
    case ARDUINO_EVENT_ETH_GOT_IP:
      // If the Wifi is connected then drop that connection
      if(WiFi.status() == WL_CONNECTED) WiFi.disconnect(true);
      Serial.print("Got Ethernet IP ");
      Serial.println(ETH.localIP());
      net.connectTime = millis();
      net.connType = conn_types::ethernet;
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.print("(evt) Ethernet Connected ");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("(evt) Ethernet Disconnected");
      net.connType = conn_types::unset;
      break;
    case ARDUINO_EVENT_ETH_START:               
      Serial.println("(evt) Ethernet Started"); 
      net.ethStarted = true;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("(evt) Ethernet Stopped");
      net.connType = conn_types::unset;
      net.ethStarted = false;
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.print("(evt) WiFi SoftAP Started IP:");
      Serial.println(WiFi.softAPIP());
      net.openingSoftAP = false;
      net.softAPOpened = true;
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      Serial.println("(evt) WiFi SoftAP Stopped");
      //if(net.softAPOpened) net.openingSoftAP = false;
      net.softAPOpened = false;
      break;      
    default:
      if(event > ARDUINO_EVENT_ETH_START)
        Serial.printf("(evt) Unknown Ethernet Event %d\n", event);
      break;
  }
}
void Network::emitHeap(uint8_t num) {
  if(num != 255 || this->needsBroadcast || (ESP.getMaxAllocHeap() != _lastMaxHeap || ESP.getFreeHeap() != _lastHeap)) {
    _lastMaxHeap = ESP.getMaxAllocHeap();
    _lastHeap = ESP.getFreeHeap();
    JsonSockEvent *json = sockEmit.beginEmit("memStatus");
    json->beginObject();
    json->addElem("max", _lastMaxHeap);
    json->addElem("free", _lastHeap);
    json->addElem("min", ESP.getMinFreeHeap());
    json->addElem("total", ESP.getHeapSize());
    json->endObject();
    sockEmit.endEmit(num);
  }
}
