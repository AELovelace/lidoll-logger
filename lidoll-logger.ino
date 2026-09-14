#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "CrowPanel14.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h" // A credential-free build shows setup instructions instead of contacting the server.
#endif

struct Selection { uint32_t serial; bool individual; uint8_t days; uint32_t index; };
struct Snapshot {
  uint32_t serial,index,total; bool ok,allAllowed,clear; int httpCode;
  char label[96],from[11],to[11],captured[32],message[96];
  double liquids; uint32_t observations,wettings,changes,bedwetting,potty,stars;
  uint32_t daily[31]; uint8_t dayCount;
};

CrowPanel14 screen;
QueueHandle_t requests=nullptr,results=nullptr;
Selection selection={0,false,7,0};Snapshot visible={};
uint32_t lastSuccess=0,lastPoll=0,lastPaint=0,participantCount=0;bool pending=false,touchDown=false,configured=false,allAllowed=false;
const uint16_t BG=0x18C5,CARD=0x2968,INK=0xFFFF,MUTED=0xB5B8,PINK=0xFBB7,TEAL=0x6E79;

bool controllerCommand(uint8_t command) { // The V1.4 STC controller owns backlight and touch reset at I2C 0x30.
  Wire.beginTransmission(0x30);Wire.write(command);return Wire.endTransmission()==0;
}
bool startPanel() { // Bound startup retries so missing hardware cannot trap the device in an endless I2C scan.
  Wire.begin(15,16);Wire.setTimeOut(100);delay(50);bool ready=false;
  for(int attempt=0;attempt<12&&!ready;attempt++){
    controllerCommand(250); // Enable touch, matching Elecrow's Advance startup sequence.
    Wire.beginTransmission(0x5D);ready=Wire.endTransmission()==0;
    if(!ready){pinMode(1,OUTPUT);digitalWrite(1,LOW);delay(120);pinMode(1,INPUT);delay(100);}
  }
  const bool backlight=controllerCommand(0); // 0 = brightest, 245 = off; this differs from the Basic-series controller.
  Wire.end(); // Release Wire before LovyanGFX takes ownership of the same I2C peripheral.
  if(!psramFound()){Serial.println("OPI PSRAM required. Check board settings.");return false;}
  if(!screen.init())return false;
  screen.setRotation(0);screen.setTextWrap(false);screen.fillScreen(BG);
  if(!ready||!backlight)Serial.println("Board controller or touch did not acknowledge; check Advance V1.4 hardware.");
  return true;
}

bool readJson(const String &path,JsonDocument &document,Snapshot &out) { // Only the network task performs bounded, read-only HTTP requests.
  const String base=STATS_API_BASE;const bool tls=base.startsWith("https://");
  if((!tls&&!base.startsWith("http://"))||!base.endsWith("/")){strlcpy(out.message,"API URL must be http(s)://.../statistics/v1/",sizeof(out.message));return false;}
  if(tls&&strlen(TLS_ROOT_CA)==0){strlcpy(out.message,"HTTPS needs TLS_ROOT_CA in config.h",sizeof(out.message));return false;}
  WiFiClient plain;WiFiClientSecure secure;HTTPClient http;
  if(tls)secure.setCACert(TLS_ROOT_CA);
  http.setConnectTimeout(5000);http.setTimeout(7000);http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const bool begun=tls?http.begin(secure,base+path):http.begin(plain,base+path);
  if(!begun){strlcpy(out.message,"Cannot open configured API URL",sizeof(out.message));return false;}
  http.addHeader("Authorization",String("Bearer ")+STATS_TOKEN);http.addHeader("Accept","application/json");
  out.httpCode=http.GET();bool ok=false;
  if(out.httpCode==200){
    const int size=http.getSize(); // The API supplies Content-Length; bound allocation before reading JSON.
    if(size>0&&size<=32768){const String body=http.getString();ok=body.length()==size&&!deserializeJson(document,body);}
    if(!ok)strlcpy(out.message,"Invalid or oversized API response",sizeof(out.message));
  }else{
    out.clear=out.httpCode==401||out.httpCode==403; // Drop private cached values as soon as revocation is observed.
    if(out.clear)strlcpy(out.message,"Access denied: replace or renew the device token",sizeof(out.message));
    else snprintf(out.message,sizeof(out.message),"Server unavailable (HTTP %d); retrying",out.httpCode);
  }
  http.end();return ok;
}

void networkTask(void *) { // Keep Wi-Fi reconnects, TLS and proxy delays off the display/touch loop.
  WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  configTime(0,0,"pool.ntp.org","time.nist.gov"); // TLS uses UTC; dashboard dates come from the LA calendar on the server.
  uint32_t reconnect=millis();Selection wanted;
  for(;;){
    if(xQueueReceive(requests,&wanted,pdMS_TO_TICKS(250))!=pdTRUE)continue;
    Snapshot out={};out.serial=wanted.serial;out.index=wanted.index;
    if(WiFi.status()!=WL_CONNECTED){
      if(millis()-reconnect>=15000){WiFi.reconnect();reconnect=millis();}
      strlcpy(out.message,"Connecting to Wi-Fi; automatic retry",sizeof(out.message));xQueueOverwrite(results,&out);continue;
    }
    JsonDocument directory;
    if(!readJson("participants?limit=1&offset="+String(wanted.individual?wanted.index:0),directory,out)){xQueueOverwrite(results,&out);continue;}
    if(!directory["total"].is<uint32_t>()||!directory["participants"].is<JsonArray>()||!directory["scope"].is<const char*>()){
      strlcpy(out.message,"Unexpected participant response",sizeof(out.message));xQueueOverwrite(results,&out);continue;
    }
    out.total=directory["total"].as<uint32_t>();out.allAllowed=directory["scope"]=="all";
    bool individual=wanted.individual||!out.allAllowed;
    String query="summary?days="+String(wanted.days);
    if(individual){
      if(directory["participants"].size()==0){out.clear=true;strlcpy(out.message,"Participant list changed; choose Previous",sizeof(out.message));xQueueOverwrite(results,&out);continue;}
      const char *id=directory["participants"][0]["id"]|"";
      if(strlen(id)!=36){strlcpy(out.message,"Invalid participant ID",sizeof(out.message));xQueueOverwrite(results,&out);continue;}
      query+="&scope=participant&participantId=";query+=id;
    }else query+="&scope=all";
    JsonDocument data;
    if(!readJson(query,data,out)){xQueueOverwrite(results,&out);continue;}
    if(data["schemaVersion"]!=1||!data["totals"].is<JsonObject>()||!data["days"].is<JsonArray>()||data["days"].size()!=wanted.days){
      strlcpy(out.message,"Unexpected statistics response",sizeof(out.message));xQueueOverwrite(results,&out);continue;
    }
    strlcpy(out.label,individual?(data["participant"]["label"]|"Participant"):"Everyone",sizeof(out.label));
    strlcpy(out.from,data["from"]|"",sizeof(out.from));strlcpy(out.to,data["to"]|"",sizeof(out.to));strlcpy(out.captured,data["capturedAt"]|"",sizeof(out.captured));
    JsonObject totals=data["totals"];out.liquids=totals["liquidsMl"]|0.0;out.observations=totals["observations"]|0U;
    out.wettings=totals["wettings"]|0U;out.changes=totals["diaperChanges"]|0U;out.stars=totals["chartStars"]|0U;
    out.bedwetting=totals["categories"]["bedwetting"]|0U;out.potty=totals["categories"]["used-the-potty"]|0U;
    out.dayCount=data["days"].size();for(int i=0;i<out.dayCount;i++)out.daily[i]=data["days"][i]["wettings"]|0U;
    out.ok=true;xQueueOverwrite(results,&out);
  }
}

void textAt(const String &text,int x,int y,int size=2,uint16_t color=INK) { // Render a single dashboard line with the built-in font.
  screen.setTextColor(color);screen.setTextSize(size);screen.setCursor(x,y);screen.print(text);
}
void button(int x,int width,const char *label,bool active) { // Large targets let fingers operate the display without a stylus.
  screen.fillRoundRect(x,405,width,44,8,active?PINK:CARD);textAt(label,x+12,419,2,active?BG:INK);
}
void card(int x,int y,const char *label,const String &value) { // Group a metric label and value in a consistent card.
  screen.fillRoundRect(x,y,246,82,10,CARD);textAt(label,x+14,y+10,2,MUTED);textAt(value,x+14,y+36,3,INK);
}
void paint() { // The main task alone owns graphics, avoiding races with network requests.
  screen.fillScreen(BG);textAt("LITTLE LOG",20,16,2,PINK);
  if(!configured){textAt("CrowPanel Advance 7 / V1.4",20,70,3);textAt("Copy config.example.h to config.h",20,135);textAt("Set Wi-Fi, API URL and admin statistics token.",20,175);textAt("Then compile and upload again.",20,215);return;}
  String label=visible.ok?String(visible.label):(selection.individual?"Individual statistics":"Everyone");
  if(label.length()>30)label=label.substring(0,29)+"~";textAt(label,20,46,3);
  if(visible.ok)textAt(String(visible.from)+" to "+visible.to+"  |  saved local dates",20,84,2,MUTED);
  else textAt(pending?"Loading statistics...":String(visible.message),20,84,2,MUTED);
  const String dash="--";
  card(20,116,"Liquids (ml)",visible.ok?String(visible.liquids,0):dash);
  card(277,116,"Wetting records*",visible.ok?String(visible.wettings):dash);
  card(534,116,"Diaper changes",visible.ok?String(visible.changes):dash);
  card(20,210,"Bedwetting",visible.ok?String(visible.bedwetting):dash);
  card(277,210,"Used the potty",visible.ok?String(visible.potty):dash);
  card(534,210,"Chart stars",visible.ok?String(visible.stars):dash);
  textAt("*Includes potty use. Bars: wetting records by day.",20,305,2,MUTED);
  if(visible.ok){
    uint32_t peak=1;for(int i=0;i<visible.dayCount;i++)peak=max(peak,visible.daily[i]);
    int width=740/visible.dayCount;
    for(int i=0;i<visible.dayCount;i++){int height=uint64_t(visible.daily[i])*48/peak;screen.fillRect(25+i*width,378-height,max(2,width-5),max(1,height),TEAL);}
    textAt(String(visible.observations)+" liquid records",20,384,1,MUTED);
  }
  button(20,112,"Today",selection.days==1);button(140,112,"7 days",selection.days==7);button(260,112,"31 days",selection.days==31);
  button(380,144,selection.individual?"Individual":"Everyone",false);button(532,72,"Prev",false);button(612,72,"Next",false);button(692,88,"Sync",false);
  String status=pending?"Updating...":String(visible.message);
  if(!status.length()&&visible.ok)status="Updated "+String((millis()-lastSuccess)/1000)+"s ago | LA calendar | "+String(participantCount)+" participant(s)";
  if(visible.ok&&strlen(visible.message))status="STALE: "+status;
  if(status.length()>90)status=status.substring(0,89);textAt(status,20,459,1,visible.message[0]?PINK:MUTED);
}
void refresh(bool changed=false) { // Discard old selections immediately; serial numbers reject their delayed HTTP responses.
  selection.serial++;pending=true;lastPoll=millis();if(changed)visible={};xQueueOverwrite(requests,&selection);paint();
}
void setup() { // Initialize the panel and request a first read without waiting for a network connection.
  Serial.begin(115200);
  if(!startPanel()){Serial.println("Panel initialization failed. Check OPI PSRAM and board version.");for(;;)delay(1000);}
  configured=strlen(WIFI_SSID)>0&&strlen(STATS_TOKEN)>0;
  requests=xQueueCreate(1,sizeof(Selection));results=xQueueCreate(1,sizeof(Snapshot));
  if(!requests||!results){configured=false;paint();textAt("Queue allocation failed; restart device.",20,275);return;}
  paint();if(!configured)return;
  if(xTaskCreatePinnedToCore(networkTask,"statistics",16384,nullptr,1,nullptr,0)!=pdPASS){configured=false;textAt("Network task allocation failed",20,275);return;}
  refresh();
}
void loop() { // Handle touch continuously while the worker reconnects or waits on the server.
  if(!configured){delay(50);return;}
  Snapshot incoming;
  if(xQueueReceive(results,&incoming,0)==pdTRUE&&incoming.serial==selection.serial){
    pending=false;
    if(incoming.ok){visible=incoming;lastSuccess=millis();participantCount=incoming.total;allAllowed=incoming.allAllowed;if(!allAllowed)selection.individual=true;}
    else{if(incoming.clear)visible={};strlcpy(visible.message,incoming.message,sizeof(visible.message));}
    paint();
  }
  uint16_t x=0,y=0;bool touched=screen.getTouch(&x,&y);
  if(touched&&!touchDown&&y>=405&&y<=449){
    bool changed=true;
    if(x>=20&&x<132)selection.days=1;else if(x>=140&&x<252)selection.days=7;else if(x>=260&&x<372)selection.days=31;
    else if(x>=380&&x<524){if(allAllowed)selection.individual=!selection.individual;}
    else if(x>=532&&x<604){selection.individual=true;selection.index=selection.index?selection.index-1:(participantCount?participantCount-1:0);}
    else if(x>=612&&x<684){selection.individual=true;selection.index=participantCount?(selection.index+1)%participantCount:0;}
    else if(x>=692&&x<780)changed=false;else{touchDown=touched;delay(8);return;}
    refresh(changed);
  }
  touchDown=touched;
  const uint32_t interval=visible.ok?max(uint32_t(10000),POLL_INTERVAL_MS):10000;
  if(!pending&&millis()-lastPoll>=interval)refresh();
  if(millis()-lastPaint>=10000){lastPaint=millis();paint();}
  delay(8);
}
