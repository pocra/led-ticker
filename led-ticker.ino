// **WICHTIG:** Bitte patche die Datei
// <Arduino>/libraries/ESPAsyncWebServer/src/WebAuthentication.cpp
// indem Du in Zeile 74–76
//   mbedtls_md5_starts_ret(&_ctx);
//   mbedtls_md5_update_ret(&_ctx, data, len);
//   mbedtls_md5_finish_ret(&_ctx, _buf);
// ersetzt durch:
//   mbedtls_md5_starts(&_ctx);
//   mbedtls_md5_update(&_ctx, data, len);
//   mbedtls_md5_finish(&_ctx, _buf);
// da die aktuelle ESPAsyncWebServer-Version die alten Symbole nicht mehr exportiert.

#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sntp.h>

// — Hardware & Display —
#define HARDWARE_TYPE   MD_MAX72XX::FC16_HW
#define DATA_PIN        23
#define CLK_PIN         18
#define CS_PIN          5
#define MAX_DEVICES     12

MD_Parola display(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// — WLAN & NTP —
const char* ssid               = "DEIN_SSID";
const char* pass               = "DEIN_PASS";
const long  gmtOffset_sec      = 3600;   // MEZ
const int   daylightOffset_sec = 3600;   // MESZ

// — Configuration —
struct Config {
  String text;
  int patternCols;
  std::vector<std::vector<uint8_t>> pattern2D;
  String igUserId;
  String appId;
  String appSecret;
  String igToken;
  unsigned long lastRefresh;
};
Config cfg;

// — Webserver & Timing —
AsyncWebServer server(80);
enum Slide { CLOCK, TEXT, INSTA, PATTERN };
Slide current       = CLOCK;
unsigned long lastSlide   = 0;
unsigned long lastInsta   = 0;
const unsigned long slideInterval   = 10000;
const unsigned long instaInterval   = 60000;
const unsigned long refreshInterval = 30UL*24UL*3600UL*1000UL;

// — Save/Load Config —
void saveConfig() {
  DynamicJsonDocument doc(2048);
  doc["text"] = cfg.text;
  doc["patternCols"] = cfg.patternCols;
  JsonArray patArr = doc.createNestedArray("pattern2D");
  for (auto &row : cfg.pattern2D) {
    JsonArray rowArr = patArr.createNestedArray();
    for (auto v : row) rowArr.add(v);
  }
  doc["igUserId"] = cfg.igUserId;
  doc["appId"] = cfg.appId;
  doc["appSecret"] = cfg.appSecret;
  doc["igToken"] = cfg.igToken;
  doc["lastRefresh"] = cfg.lastRefresh;
  File f = LittleFS.open("/config.json","w");
  serializeJson(doc,f);
  f.close();
}

void loadConfig() {
  if (!LittleFS.exists("/config.json")) return;
  File f = LittleFS.open("/config.json","r");
  DynamicJsonDocument doc(2048);
  deserializeJson(doc,f);
  f.close();
  cfg.text = doc["text"].as<String>();
  cfg.patternCols = doc["patternCols"].as<int>();
  cfg.pattern2D.clear();
  for (JsonVariant rv : doc["pattern2D"].as<JsonArray>()) {
    std::vector<uint8_t> row;
    for (JsonVariant cv : rv.as<JsonArray>()) row.push_back(cv.as<uint8_t>());
    cfg.pattern2D.push_back(row);
  }
  cfg.igUserId = doc["igUserId"].as<String>();
  cfg.appId = doc["appId"].as<String>();
  cfg.appSecret = doc["appSecret"].as<String>();
  cfg.igToken = doc["igToken"].as<String>();
  cfg.lastRefresh = doc["lastRefresh"].as<unsigned long>();
}

// — Instagram Token Refresh —
bool refreshInstaToken() {
  if (cfg.igToken.isEmpty()||cfg.appId.isEmpty()||cfg.appSecret.isEmpty()) return false;
  HTTPClient http;
  String url = String("https://graph.facebook.com/v17.0/oauth/access_token?") +
    "grant_type=fb_exchange_token&client_id="+cfg.appId+
    "&client_secret="+cfg.appSecret+
    "&fb_exchange_token="+cfg.igToken;
  http.begin(url);
  if (http.GET()!=200) { http.end(); return false; }
  DynamicJsonDocument d(512);
  deserializeJson(d,http.getString());
  http.end();
  String nt = d["access_token"].as<String>();
  if (nt.length()>0) {
    cfg.igToken=nt;
    cfg.lastRefresh=millis();
    saveConfig();
    return true;
  }
  return false;
}

// — Fetch Followers —
int fetchInstaFollowers() {
  if (cfg.igUserId.isEmpty()||cfg.igToken.isEmpty()) return -1;
  HTTPClient http;
  String url = String("https://graph.facebook.com/v17.0/")+cfg.igUserId+"?fields=followers_count&access_token="+cfg.igToken;
  http.begin(url);
  int code = http.GET(); int cnt=-1;
  if (code==200) {
    DynamicJsonDocument d(256);
    deserializeJson(d,http.getString());
    cnt=d["followers_count"].as<int>();
  }
  http.end();
  return cnt;
}

// — Display Handler —
void loadAndDisplay(Slide s) {
  display.displayClear(); char buf[32];
  switch(s) {
    case CLOCK: {
      struct tm t;
      if (getLocalTime(&t)) strftime(buf,sizeof(buf),"%H:%M",&t);
      else strcpy(buf,"--:--");
      display.displayText(buf,PA_CENTER,0,0,PA_SCROLL_LEFT,PA_SCROLL_LEFT);
      break;
    }
    case TEXT:
      display.displayText(cfg.text.c_str(),PA_LEFT,0,0,PA_SCROLL_LEFT,PA_SCROLL_LEFT);
      break;
    case INSTA: {
      int f=fetchInstaFollowers();
      snprintf(buf,sizeof(buf),"IG:%d",f>=0?f:0);
      display.displayText(buf,PA_LEFT,0,0,PA_SCROLL_LEFT,PA_SCROLL_LEFT);
      break;
    }
    case PATTERN: {
      for(int off=0;off<cfg.patternCols;off++){
        for(uint8_t r=0;r<8;r++){
          uint8_t col=0;
          for(uint8_t b=0;b<8;b++){
            int c=off+b;
            if(c<cfg.patternCols && cfg.pattern2D[r][c]) col|=1<<(7-b);
          }
          display.getGraphicObject()->setColumn(r,col);
        }
        display.displayAnimate(); delay(50);
      }
      break;
    }
  }
}

void setup(){
  Serial.begin(115200);
  LittleFS.begin();
  WiFi.begin(ssid,pass);
  while(WiFi.status()!=WL_CONNECTED) delay(200);
  configTime(gmtOffset_sec,daylightOffset_sec,"pool.ntp.org");
  sntp_set_sync_interval(3600UL*1000UL);

  server.serveStatic("/",LittleFS,"/").setDefaultFile("index.html");
  server.on("/api/getConfig",HTTP_GET,[](AsyncWebServerRequest*req){
    DynamicJsonDocument d(2048);
    d["text"]=cfg.text; d["patternCols"]=cfg.patternCols;
    JsonArray pa=d.createNestedArray("pattern2D");
    for(auto&r:cfg.pattern2D){auto a=pa.createNestedArray(); for(auto v:r) a.add(v);}    
    d["igUserId"]=cfg.igUserId; d["appId"]=cfg.appId;
    d["appSecret"]=cfg.appSecret; d["igToken"]=cfg.igToken;
    String out; serializeJson(d,out);
    req->send(200,"application/json",out);
  });
  server.on("/api/config",HTTP_POST,[](AsyncWebServerRequest*req){
    String body=req->getParam("plain",true)->value();
    DynamicJsonDocument d(2048); deserializeJson(d,body);
    cfg.text=d["text"].as<String>(); cfg.patternCols=d["patternCols"].as<int>();
    cfg.pattern2D.clear(); for(JsonVariant rv:d["pattern2D"].as<JsonArray>()){std::vector<uint8_t>row; for(JsonVariant cv:rv.as<JsonArray>())row.push_back(cv.as<uint8_t>()); cfg.pattern2D.push_back(row);}    
    cfg.igUserId=d["igUserId"].as<String>(); cfg.appId=d["appId"].as<String>();
    cfg.appSecret=d["appSecret"].as<String>(); cfg.igToken=d["igToken"].as<String>();
    saveConfig(); req->send(200,"text/plain","Gespeichert");
  });
  server.begin();

  loadConfig();
  if(millis()-cfg.lastRefresh>refreshInterval) refreshInstaToken();
  display.begin(); display.setIntensity(8); display.setTextAlignment(PA_LEFT);
}

void loop(){
  unsigned long now=millis();
  if(now-lastSlide>slideInterval){lastSlide=now; current=Slide((current+1)%4); loadAndDisplay(current);}  
  if(now-lastInsta>instaInterval) lastInsta=now;
  if(display.displayAnimate()) display.displayReset();
}