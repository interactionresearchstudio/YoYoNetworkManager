#include "YoYoWiFiManager.h"

YoYoWiFiManager::YoYoWiFiManager() {
}

void YoYoWiFiManager::init(YoYoWiFiManagerSettings *settings, callbackPtr getHandler, callbackPtr postHandler, bool startWebServerOnceConnected, int webServerPort, uint8_t wifiLEDPin) {
  this -> settings = settings;
  this -> yoYoCommandGetHandler = getHandler;
  this -> yoYoCommandPostHandler = postHandler;

  this -> startWebServerOnceConnected = startWebServerOnceConnected;
  this -> webServerPort = webServerPort;

  this -> wifiLEDPin = wifiLEDPin;
  pinMode(wifiLEDPin, OUTPUT);

  #if defined(ESP32)
    memset(&wifi_sta_list, 0, sizeof(wifi_sta_list));
  #endif
  memset(&adapter_sta_list, 0, sizeof(adapter_sta_list));

  SPIFFS_ENABLED = SPIFFS.begin();

  peerNetworkSSID[0] = NULL;
  peerNetworkPassword[0] = NULL;
}

boolean YoYoWiFiManager::begin(char const *apName, char const *apPassword, bool autoconnect) {
  addPeerNetwork((char *)apName, (char *)apPassword);
  wifiMulti.run();  //prioritise joining peer networks over known networks

  if(autoconnect && settings && settings -> hasNetworkCredentials()) {
    Serial.println("network credentials available");
    addKnownNetworks();
    setMode(YY_MODE_CLIENT);
  }
  else {
    setMode(YY_MODE_PEER_CLIENT); //attempt to join peer network;
  }

  return(true);
}

void YoYoWiFiManager::connect() {
  //Once in YY_MODE_CLIENT mode - update() will trigger wifiMulti.run()
  setMode(YY_MODE_CLIENT);
}

// Creates Access Point for other device to connect to
void YoYoWiFiManager::startPeerNetworkAsAP() {
  Serial.print("Wifi name:");
  Serial.println(peerNetworkSSID);

  WiFi.mode(WIFI_AP);
  delay(2000);

  WiFi.persistent(false);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(peerNetworkSSID, peerNetworkPassword);
  IPAddress myIP = WiFi.softAPIP();
  Serial.println(myIP);

  //Start captive portal
  dnsServer.start(DNS_PORT, "*", apIP);
}

void YoYoWiFiManager::startWebServer() {
  Serial.println("startWebServer\n");

  if(webserver == NULL) {
    webserver = new AsyncWebServer(webServerPort);
    webserver -> addHandler(this);
    webserver -> begin();
  }
}

void YoYoWiFiManager::stopWebServer() {
  if(webserver != NULL) {
    webserver -> end();
    delete(webserver);
    webserver = NULL;
  }
}

void YoYoWiFiManager::addPeerNetwork(char *ssid, char *password) {
  if(ssid != NULL) {
    strcpy(peerNetworkSSID, ssid);
    if(password != NULL) strcpy(peerNetworkPassword, password);

    addNetwork(peerNetworkSSID, peerNetworkPassword, false);
  }
}

void YoYoWiFiManager::addKnownNetworks() {
  if(settings) {
    int numberOfNetworkCredentials = settings->getNumberOfNetworkCredentials();
    char *ssid = new char[32];
    char *password = new char[64];
    for(int n = 0; n < numberOfNetworkCredentials; ++n) {
      settings -> getSSID(n, ssid);
      settings -> getPassword(n, password);
      addNetwork(ssid, password, false);
    }
    delete ssid, password;
  }
}

bool YoYoWiFiManager::addNetwork(char const *ssid, char const *password, bool save) {
  Serial.printf("YoYoWiFiManager::addNetwork %s  %s\n", ssid, password);

  bool success = false;

  if(strlen(ssid) > 0 && strlen(ssid) <= SSID_MAX_LENGTH) {
    char *matchingSSID = new char[SSID_MAX_LENGTH];

    if(findNetwork(ssid, matchingSSID, false, true, 2)) {
      ssid = matchingSSID;
    }

    if(wifiMulti.addAP(ssid, password)) {
      if(save && settings) settings -> addNetwork(ssid, password);
      success = true;
    }
    delete matchingSSID;
  }

  return(success);
}

bool YoYoWiFiManager::findNetwork(char const *ssid, char *matchingSSID, bool autocomplete, bool autocorrect, int autocorrectError) {
  bool result = false;

  int numberOfNetworks = scanNetworks();
  for(int n = 0; n < numberOfNetworks && !result; ++n) {
    bool match = WiFi.SSID(n).equals(ssid) || 
                  (autocomplete && WiFi.SSID(n).startsWith(ssid)) || 
                  (autocorrect && Levenshtein::levenshteinIgnoreCase(ssid, WiFi.SSID(n).c_str()) < autocorrectError);

    if(match) {
      result = true;
      strcpy(matchingSSID, WiFi.SSID(n).c_str());
    }
  }

  return(result);
}

yy_status_t YoYoWiFiManager::getStatus() {
  uint8_t wlStatus = (currentMode == YY_MODE_PEER_SERVER) ?  WiFi.status() : wifiMulti.run();
  yy_status_t yyStatus = currentStatus;

  if(wlStatus == WL_CONNECTED) {
    switch(currentMode) {
      case YY_MODE_CLIENT:      yyStatus = YY_CONNECTED; break;
      case YY_MODE_PEER_CLIENT: yyStatus = YY_CONNECTED_PEER_CLIENT; break;
    }
  }
  else if(currentMode == YY_MODE_PEER_SERVER && hasClients()) {
    yyStatus = YY_CONNECTED_PEER_SERVER;
  }
  else if(currentMode == WL_IDLE_STATUS) {
    //Suspress WL_IDLE_STATUS
  }
  else {
    yyStatus = (yy_status_t) wlStatus;  //Otherwise yy_status_t and wl_status_t are value compatible
  }

  return(yyStatus);
}

uint8_t YoYoWiFiManager::update() {
  yy_status_t yyStatus = getStatus();

  //Only when the status changes:
  if(currentStatus != yyStatus) {
    switch(yyStatus) {
      //implicitly in YY_MODE_CLIENT
      case YY_CONNECTED:
        if(WiFi.SSID().equals(peerNetworkSSID)) {
          setMode(YY_MODE_PEER_CLIENT);
        }
        else {
          blinkWiFiLED(3);
          Serial.printf("Connected to: %s\n", WiFi.SSID().c_str());
          Serial.println(WiFi.localIP());
        }
      break;
      //implicitly in YY_MODE_PEER_CLIENT
      case YY_CONNECTED_PEER_CLIENT:
        Serial.printf("Connected to Peer Network: %s\n", WiFi.SSID().c_str());
        Serial.println(WiFi.localIP());
      break;
      //implicitly in YY_MODE_PEER_SERVER
      case YY_CONNECTED_PEER_SERVER:
      break;
      case YY_CONNECTION_LOST:
        setMode(YY_MODE_CLIENT);
      break;
      case YY_DISCONNECTED:
      break;
    }
    currentStatus = yyStatus;
    printModeAndStatus();
  }

  //Everytime for each status:
  switch(currentStatus) {
    //implicitly in YY_MODE_CLIENT
    case YY_CONNECTED:
      updateClientTimeOut();
    break;
    //implicitly in YY_MODE_PEER_CLIENT
    case YY_CONNECTED_PEER_CLIENT:
      updateClientTimeOut();
    break;
    //implicitly in YY_MODE_PEER_SERVER
    case YY_CONNECTED_PEER_SERVER:
      if(hasClients()) updateServerTimeOut();
    break;
    case YY_NO_SSID_AVAIL:
      if(currentMode != YY_MODE_PEER_SERVER && clientHasTimedOut())
        setMode(YY_MODE_PEER_SERVER);
    break;
    case YY_CONNECTION_LOST:
      if(currentMode != YY_MODE_PEER_SERVER && clientHasTimedOut())
        setMode(YY_MODE_PEER_SERVER);

      if(currentMode != YY_MODE_CLIENT && serverHasTimedOut() && settings && settings -> hasNetworkCredentials())
        setMode(YY_MODE_CLIENT);
    break;
    case YY_DISCONNECTED:
      if(currentMode != YY_MODE_PEER_SERVER && clientHasTimedOut())
        setMode(YY_MODE_PEER_SERVER);

      if(currentMode != YY_MODE_CLIENT && serverHasTimedOut() && settings && settings -> hasNetworkCredentials())
        setMode(YY_MODE_CLIENT);
    break;
  }

  //Everytime for each mode:
  switch(currentMode) {
    case YY_MODE_NONE:
      digitalWrite(wifiLEDPin, LOW);
      break;
    case YY_MODE_CLIENT:
      digitalWrite(wifiLEDPin, LOW);
      break;
    case YY_MODE_PEER_CLIENT:
      digitalWrite(wifiLEDPin, HIGH);
      break;
    case YY_MODE_PEER_SERVER:
      digitalWrite(wifiLEDPin, HIGH);
      dnsServer.processNextRequest();
      break;
  }

  return(currentStatus);
}

bool YoYoWiFiManager::setMode(yy_mode_t mode) {
  bool result = true;

  if(mode != currentMode) {
    switch(currentMode) {
      case YY_MODE_NONE:
        break;
      case YY_MODE_CLIENT:
        break;
      case YY_MODE_PEER_CLIENT: 
        break;
      case YY_MODE_PEER_SERVER:
        WiFi.softAPdisconnect(true);
        dnsServer.stop();
        break;
    }

    switch(mode) {
      case YY_MODE_NONE:
        break;
      case YY_MODE_CLIENT:
        updateClientTimeOut();
        if(startWebServerOnceConnected) startWebServer();
        else stopWebServer(); //make sure it's stopped
        break;
      case YY_MODE_PEER_CLIENT: 
        updateClientTimeOut();
        startWebServer();
        break;
      case YY_MODE_PEER_SERVER:
        updateServerTimeOut();
        startPeerNetworkAsAP();
        startWebServer();
        break;
    }
    
    currentMode = mode;
    printModeAndStatus();
  }

  return(result);
}

void YoYoWiFiManager::printModeAndStatus() {
  switch(currentMode) {
    case YY_MODE_NONE:
      Serial.print("YY_MODE_NONE");
      break;
    case YY_MODE_CLIENT:
      Serial.print("YY_MODE_CLIENT");
      break;
    case YY_MODE_PEER_CLIENT: 
      Serial.print("YY_MODE_PEER_CLIENT");
      break;
    case YY_MODE_PEER_SERVER:
      Serial.print("YY_MODE_PEER_SERVER");
      break;
  }

  switch (currentStatus) {
    case YY_CONNECTED:
      Serial.println("\tYY_CONNECTED");
      break;
    case YY_IDLE_STATUS:
      Serial.println("\tYY_IDLE_STATUS");
      break;
    case YY_NO_SSID_AVAIL:
      Serial.println("\tYY_NO_SSID_AVAIL");
      break;
    case YY_SCAN_COMPLETED:
      Serial.println("\tYY_SCAN_COMPLETED");
      break;
    case YY_CONNECT_FAILED:
      Serial.println("\tYY_CONNECT_FAILED");
      break;
    case YY_CONNECTION_LOST:
      Serial.println("\tYY_CONNECTION_LOST");
      break;
    case YY_DISCONNECTED:
      Serial.println("\tYY_DISCONNECTED");
      break;
    case YY_CONNECTED_PEER_CLIENT:
      Serial.println("\tYY_CONNECTED_PEER_CLIENT");
      break;
    case YY_CONNECTED_PEER_SERVER:
      Serial.println("\tYY_CONNECTED_PEER_SERVER");
      break;
  }
}

bool YoYoWiFiManager::clientHasTimedOut() {
  return(clientTimeOutAtMs > 0 && millis() > clientTimeOutAtMs);
}

void YoYoWiFiManager::updateClientTimeOut() {
  clientTimeOutAtMs = millis() + WIFICLIENTTIMEOUT;
}

bool YoYoWiFiManager::serverHasTimedOut() {
  return(serverTimeOutAtMs > 0 && millis() > serverTimeOutAtMs);
}

void YoYoWiFiManager::updateServerTimeOut() {
  serverTimeOutAtMs = millis() + WIFISERVERTIMEOUT;
}


void YoYoWiFiManager::blinkWiFiLED(int count) {
  for (byte i = 0; i < count; i++) {
    digitalWrite(wifiLEDPin, 1);
    delay(100);
    digitalWrite(wifiLEDPin, 0);
    delay(400);
  }
  delay(600);
}

//AsyncWebHandler
//===============

bool YoYoWiFiManager::canHandle(AsyncWebServerRequest *request) {
  //we can handle anything!
  return true;
}

void YoYoWiFiManager::handleRequest(AsyncWebServerRequest *request) {
  Serial.print("handleRequest: ");
  Serial.println(request->url());

  if (request->method() == HTTP_GET) {
    if(request->url().startsWith("/yoyo")) {
      onYoYoCommandGET(request);
    }
    else if (SPIFFS_ENABLED && SPIFFS.exists(request->url())) {
      sendFile(request, request->url());
    }
    else if (currentMode == YY_MODE_PEER_SERVER) {
      handleCaptivePortalRequest(request);
    }
    else if(request->url().equals("/")) {
      sendIndexFile(request);
    }
    else if(request->url().endsWith("/")) {
      sendFile(request, request->url() + "index.html");
    }
    else {
      request->send(404);
    }
  }
  else if (request->method() == HTTP_POST) {
    request->send(400); //POSTs are expected to have a body and then be processes by handleBody()
  }
  else {
    request->send(400);
  }
}

void YoYoWiFiManager::handleCaptivePortalRequest(AsyncWebServerRequest *request) {
    if (request->url().endsWith(".html") || 
              request->url().endsWith("/") ||
              request->url().endsWith("generate_204") ||
              request->url().endsWith("redirect"))  {
      sendIndexFile(request);
    }
    else if (request->url().endsWith("connecttest.txt") || 
              request->url().endsWith("ncsi.txt")) {
      request->send(200, "text/plain", "Microsoft NCSI");
    }
    else if (strstr(request->url().c_str(), "generate_204_") != NULL) {
      Serial.println("you must be huawei!");
      sendIndexFile(request);
    }
    else {
      request->send(304);
    }
}

void YoYoWiFiManager::handleBody(AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
  Serial.print("handleBody: ");
  Serial.println(request->url());

  if (request->method() == HTTP_GET) {
    request->send(400); //GETs are expected to have no body and then be processes by handleRequest()
  }
  else if (request->method() == HTTP_POST) {
    if(request->url().startsWith("/yoyo")) {
      String json = "";
      for (int i = 0; i < len; i++)  json += char(data[i]);

      StaticJsonDocument<1024> jsonDoc;
      if (!deserializeJson(jsonDoc, json)) {
        onYoYoCommandPOST(request, jsonDoc.as<JsonVariant>());
      }
    }
    else {
      request->send(404);
    }
  }
  else {
    request->send(400);
  }
}

void YoYoWiFiManager::sendFile(AsyncWebServerRequest * request, String path) {
  Serial.println("handleFileRead: " + path);

  if (SPIFFS_ENABLED && SPIFFS.exists(path)) {
    request->send(SPIFFS, path, getContentType(path));
  }
  else {
    request->send(404);
  }
}

void YoYoWiFiManager::sendIndexFile(AsyncWebServerRequest * request) {
  String path = "/index.html";
  if (SPIFFS_ENABLED && SPIFFS.exists(path)) {
    sendFile(request, path);
  }
  else {
    request->send(200, "text/html", DEFAULT_INDEX_HTML);
  }
}

String YoYoWiFiManager::getContentType(String filename) {
  if (filename.endsWith(".htm")) return "text/html";
  else if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".gif")) return "image/gif";
  else if (filename.endsWith(".jpg")) return "image/jpeg";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".xml")) return "text/xml";
  else if (filename.endsWith(".pdf")) return "application/x-pdf";
  else if (filename.endsWith(".zip")) return "application/x-zip";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  else if (filename.endsWith(".json")) return "application/json";
  return "text/plain";
}

void YoYoWiFiManager::onYoYoCommandGET(AsyncWebServerRequest *request) {
  bool success = false;

  if (request->url().equals("/yoyo/networks"))         getNetworks(request);
  else if (request->url().equals("/yoyo/clients"))     getClients(request);
  else if (request->url().equals("/yoyo/peers"))       getPeers(request);
  else if (request->url().equals("/yoyo/credentials")) getCredentials(request);
  else {
    AsyncResponseStream *response = request->beginResponseStream("application/json");

    StaticJsonDocument<1024> settingsJsonDoc;
    if(yoYoCommandGetHandler) {
      success = yoYoCommandGetHandler(request->url(), settingsJsonDoc.as<JsonVariant>());
    }

    if(success) {
      if(!settingsJsonDoc.isNull()) {
        String jsonString;
        serializeJson(settingsJsonDoc, jsonString);
        response->print(jsonString);
      }
      else response->print("{}");

      request->send(response);
    }
    else {
      request->send(400);
    }
  }
}

void YoYoWiFiManager::onYoYoCommandPOST(AsyncWebServerRequest *request, JsonVariant json) {
  if (request->url().equals("/yoyo/credentials")) {
    if(setCredentials(request, json)) {
      broadcastToPeersPOST(request, json);
      delay(random(MAX_SYNC_DELAY));  //stop peers that are restarting together becoming synchronised
      connect();
    }
  }
  else {
    bool success = false;

    if(yoYoCommandPostHandler) {
      success = yoYoCommandPostHandler(request->url(), json);
    }
    request->send(success ? 200 : 404);
  }
}

void YoYoWiFiManager::broadcastToPeersPOST(AsyncWebServerRequest *request, JsonVariant json) {
  if(currentMode == YY_MODE_PEER_SERVER) {
    int peerCount = countPeers();

    char *ipAddress = new char[17];
    for (int i = 0; i < peerCount; i++) {
      getPeerN(i, ipAddress, NULL);
      makePOST(ipAddress, request->url().c_str(), json);
    }
    delete ipAddress;
  }
}

void YoYoWiFiManager::makePOST(const char *server, const char *path, JsonVariant json) {
  String jsonAsString;
  serializeJson(json, jsonAsString);

  String urlAsString = "http://" + String(server) + String(path);
  Serial.printf("%s > %s\n", urlAsString.c_str(), jsonAsString.c_str());

  HTTPClient http;

  http.begin(urlAsString);
  http.POST(jsonAsString);

  // Read response
  Serial.print(http.getString());

  http.end();
}

void YoYoWiFiManager::getCredentials(AsyncWebServerRequest *request) {
  //TODO: get all the credentials and turn them into json - but not passwords
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->print("{}");
  //serializeJson(settings["credentials"], Serial);

  request->send(response);
}

bool YoYoWiFiManager::setCredentials(AsyncWebServerRequest *request, JsonVariant json) {
  bool success = setCredentials(json);
  request->send(success ? 200 : 400);

  return(success);
}

bool YoYoWiFiManager::setCredentials(JsonVariant json) {
  bool success = false;

  const char* ssid = json["ssid"];
  const char* password = json["password"];

  if(ssid && password) {
    addNetwork(ssid, password, true);
    success = true;
  }

  return(success);
}

void YoYoWiFiManager::getPeers(AsyncWebServerRequest * request) {
  AsyncResponseStream *response = request->beginResponseStream("application/json");

  response->print(getPeersAsJsonString());
  request->send(response);
}

String YoYoWiFiManager::getPeersAsJsonString() {
  String jsonString;

  StaticJsonDocument<1000> jsonDoc;
  getPeersAsJson(jsonDoc);
  serializeJson(jsonDoc[0], jsonString);

  return (jsonString);
}

void YoYoWiFiManager::getPeersAsJson(JsonDocument& jsonDoc) {
  JsonArray peers = jsonDoc.createNestedArray();
 
  char *ipAddress = new char[17];
  char *macAddress = new char[18];

  if(currentMode == YY_MODE_PEER_SERVER) {
    int peerCount = countPeers();

    for (int i = 0; i < peerCount; i++) {
      getPeerN(i, ipAddress, macAddress);

      JsonObject peer  = peers.createNestedObject();
      peer["IP"] = ipAddress;
      peer["MAC"] = macAddress;   
    }
  }
  else if(currentMode == YY_MODE_PEER_CLIENT) {
    //TODO - request from server or just the server or empty?
  }
  else if(currentMode == YY_MODE_CLIENT) {
    //TODO - empty?
  }

  delete ipAddress;
  delete macAddress;
}

void YoYoWiFiManager::getClients(AsyncWebServerRequest * request) {
  AsyncResponseStream *response = request->beginResponseStream("application/json");

  response->print(getClientsAsJsonString());
  request->send(response);
}

String YoYoWiFiManager::getClientsAsJsonString() {
  String jsonString;

  StaticJsonDocument<1000> jsonDoc;
  getClientsAsJson(jsonDoc);
  serializeJson(jsonDoc[0], jsonString);

  return (jsonString);
}

void YoYoWiFiManager::getClientsAsJson(JsonDocument& jsonDoc) {
  JsonArray clients = jsonDoc.createNestedArray();
 
  char *ipAddress = new char[17];
  char *macAddress = new char[18];

  if(currentMode == YY_MODE_PEER_SERVER) {
    tcpip_adapter_sta_info_t station;

    int clientCount = updateClientList();
    for(int n = 0; n < clientCount; ++n) {
      strcpy(ipAddress, ip4addr_ntoa(&(adapter_sta_list.sta[n].ip)));
      mac_addr_to_c_str(adapter_sta_list.sta[n].mac, macAddress);

      JsonObject client  = clients.createNestedObject();
      client["IP"] = ipAddress;
      client["MAC"] = macAddress; 
    }
  }
  else if(currentMode == YY_MODE_PEER_CLIENT) {
    //TODO - reques from server?
  }
  else if(currentMode == YY_MODE_CLIENT) {
    //TODO - empty?
  }

  delete ipAddress;
  delete macAddress;
}

int YoYoWiFiManager::updateClientList() {
  int count = 0;

  if(currentMode == YY_MODE_PEER_SERVER) {
    #if defined(ESP8266)
      struct station_info *stat_info;

      count = min(wifi_softap_get_station_num(), (uint8) ESP_WIFI_MAX_CONN_NUM);
      stat_info = wifi_softap_get_station_info();

      adapter_sta_list.num = count;

      int n=0;
      tcpip_adapter_sta_info_t station;
      while (count > 0 && stat_info != NULL) {
        memcpy(adapter_sta_list.sta[n].mac, stat_info->bssid, sizeof(stat_info->bssid[0])*6);
        adapter_sta_list.sta[n].ip = stat_info->ip;

        stat_info = STAILQ_NEXT(stat_info, next);
        n++;
      }
      wifi_softap_free_station_info();

    #elif defined(ESP32)
      esp_wifi_ap_get_sta_list(&wifi_sta_list);
      tcpip_adapter_get_sta_list(&wifi_sta_list, &adapter_sta_list);
      count = adapter_sta_list.num;
    #endif
  }
  else if(currentMode == YY_MODE_PEER_CLIENT) {
    //TODO
  }
  else if(currentMode == YY_MODE_CLIENT) {
    //TODO
  }

  return(count);
}

bool YoYoWiFiManager::hasPeers() {
  return(countPeers() > 0);
}

int YoYoWiFiManager::countPeers() {
  int count = 0;

  switch(currentMode) {
    case YY_MODE_NONE:
      break;
    case YY_MODE_CLIENT:
      break;
    case YY_MODE_PEER_CLIENT:
      if(currentStatus == YY_CONNECTED_PEER_CLIENT) count = 1;  //is connected to the server
      break;
    case YY_MODE_PEER_SERVER:
      tcpip_adapter_sta_info_t station;
      char *thisMacAddress = new char[18];
      int clientCount = countClients();
      for(int n = 0; n < clientCount; ++n) {
        station = adapter_sta_list.sta[n];
        mac_addr_to_c_str(station.mac, thisMacAddress);
        if(isEspressif(thisMacAddress)) count++;
      }
      delete thisMacAddress;
      break;
  }

  return(count);
}

bool YoYoWiFiManager::hasClients() {
  return(countClients() > 0);
}

int YoYoWiFiManager::countClients() {
  int count = 0;

  if(currentMode == YY_MODE_PEER_SERVER) {
    count = updateClientList();
  }

  return(count);
}

bool YoYoWiFiManager::getPeerN(int n, char *ipAddress, char *macAddress) {
  bool success = false;

  switch(currentMode) {
    case YY_MODE_NONE:
      break;
    case YY_MODE_CLIENT:
      break;
    case YY_MODE_PEER_CLIENT:
      //TODO: implement
      break;
    case YY_MODE_PEER_SERVER:
        tcpip_adapter_sta_info_t station;
        char *thisMacAddress = new char[18];
        int clientCount = countClients();
        int peerCount = 0;
        for(int i = 0; i < clientCount && !success; ++i) {
          station = adapter_sta_list.sta[i];
          mac_addr_to_c_str(station.mac, thisMacAddress);
          if(isEspressif(thisMacAddress)) {
            success = (peerCount == n);
            if(success) {
              if(ipAddress != NULL)   strcpy(ipAddress, ip4addr_ntoa(&(station.ip)));
              if(macAddress != NULL)  mac_addr_to_c_str(station.mac, macAddress);
            }
            peerCount++;
          }
        }
        delete thisMacAddress;
      break;
  }

  return(success);
}

void YoYoWiFiManager::getNetworks(AsyncWebServerRequest * request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");

    response->print(getNetworksAsJsonString());
    request->send(response);
}

String YoYoWiFiManager::getNetworksAsJsonString() {
  String jsonString;

  StaticJsonDocument<1000> jsonDoc;
  getNetworksAsJson(jsonDoc);
  serializeJson(jsonDoc[0], jsonString);

  return (jsonString);
}

void YoYoWiFiManager::getNetworksAsJson(JsonDocument& jsonDoc) {
  JsonArray networks = jsonDoc.createNestedArray();

  int n = scanNetworks();
  n = (n > MAX_NETWORKS_TO_SCAN) ? MAX_NETWORKS_TO_SCAN : n;

  //Array is ordered by signal strength - strongest first
  for (int i = 0; i < n; ++i) {
    String networkSSID = WiFi.SSID(i);
    if (networkSSID.length() <= SSID_MAX_LENGTH) {
      JsonObject network  = networks.createNestedObject();
      network["SSID"] = WiFi.SSID(i);
      network["BSSID"] = WiFi.BSSIDstr(i);
      network["RSSI"] = WiFi.RSSI(i);
    }
  }
}

int YoYoWiFiManager::scanNetworks() {
  int count = 0;

  if(lastScanNetworksAtMs == 0 || millis() > (lastScanNetworksAtMs + SCAN_NETWORKS_MIN_INT)) {
    lastScanNetworksAtMs = millis();

    #if defined(ESP8266)
      //ESP8266 scanNetworks() can only operate as async because of ESPAsyncWebServer > https://github.com/me-no-dev/ESPAsyncWebServer#scanning-for-available-wifi-networks
      //the consequence is that calls to function will fail to return any results if they initiate a scan
      count = WiFi.scanNetworks(true, false);

    #elif defined(ESP32)
      count = WiFi.scanNetworks(false, false);

    #endif
  }
  else {
    count = WiFi.scanComplete();
  }

  return(count);
}

bool YoYoWiFiManager::isEspressif(char *macAddress) {
  bool result = false;

  int oui = getOUI(macAddress);
  int count = sizeof(ESPRESSIF_OUI) > 0 ? sizeof(ESPRESSIF_OUI)/sizeof(ESPRESSIF_OUI[0]) : 0;

  for(int n=0; n < count && !result; ++n) {
    result = (oui == ESPRESSIF_OUI[n]);
  }

  return(result);
}

bool YoYoWiFiManager::mac_addr_to_c_str(uint8_t *mac, char *str) {
  bool success = true;

  sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X\0", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  return(success);
}

int YoYoWiFiManager::getOUI(char *mac) {
  int oui = 0;

  //basic format test ##.##.##.##.##.##
  if(strlen(mac) == 17) {
    int a, b, c, d, e, f;
    sscanf(mac, "%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f);
    
    oui = (a << 16) & 0xff0000 | (b << 8) & 0x00ff00 | c & 0x0000ff;
  }

  return(oui);
}