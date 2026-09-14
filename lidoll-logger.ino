#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "CrowPanel14.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h" // A credential-free build shows setup instructions instead of contacting the server.
#endif

class BundledRootClient : public WiFiClientSecure {
public:
  void useBundledRootCAs() { // Arduino-ESP32 exposes custom bundles, while ESP-IDF also ships a built-in public root store.
    attach_ssl_certificate_bundle(sslclient.get(),true);
    _use_ca_bundle=true;
  }
};

static constexpr uint8_t PEOPLE_PER_PAGE=5,WIFI_RESULTS=10;
enum UiMode : uint8_t { UI_DASHBOARD,UI_PEOPLE,UI_WIFI,UI_WIFI_PASSWORD };
enum WifiCommandType : uint8_t { WIFI_SCAN,WIFI_CONNECT };
struct Selection { uint32_t serial; bool individual,directoryOnly; uint8_t days; uint32_t index,peopleOffset; };
struct Snapshot {
  uint32_t serial,index,total; bool ok,allAllowed,clear,directoryOnly; int httpCode;
  char label[96],from[11],to[11],captured[32],message[96];
  double liquids; uint32_t observations,wettings,changes,bedwetting,potty,stars;
  uint32_t daily[31]; uint8_t dayCount;
  uint32_t peopleOffset;uint8_t peopleCount;char people[PEOPLE_PER_PAGE][64];
};
struct WifiNetwork { char ssid[33];int32_t rssi;bool open; };
struct WifiCommand { WifiCommandType type;char ssid[33],password[65]; };
struct WifiReply { WifiCommandType type;bool ok;uint8_t count;char message[96];WifiNetwork networks[WIFI_RESULTS]; };
struct StoredWifi { uint32_t magic;char ssid[33],password[65]; };

CrowPanel14 screen;
QueueHandle_t requests=nullptr,results=nullptr,wifiCommands=nullptr,wifiReplies=nullptr;
Selection selection={0,false,false,7,0,0};Snapshot visible={};WifiReply wifiView={};
uint32_t lastSuccess=0,lastPoll=0,lastPaint=0,participantCount=0;bool pending=false,touchDown=false,configured=false,allAllowed=false;
UiMode uiMode=UI_DASHBOARD;uint8_t paintedMode=255,wifiPage=0,keyboardMode=0;bool wifiBusy=false;char chosenSsid[33]={},wifiPassword[65]={},wifiMessage[96]={};
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
  WiFiClient plain;BundledRootClient secure;HTTPClient http;
  if(tls){if(strlen(TLS_ROOT_CA))secure.setCACert(TLS_ROOT_CA);else secure.useBundledRootCAs();}
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
  StoredWifi storedWifi={};Preferences savedWifi;savedWifi.begin("lidoll-wifi",true);
  const bool hasSavedWifi=savedWifi.getBytesLength("credentials")==sizeof(storedWifi)&&savedWifi.getBytes("credentials",&storedWifi,sizeof(storedWifi))==sizeof(storedWifi)&&storedWifi.magic==0x4C445746&&storedWifi.ssid[32]==0&&storedWifi.password[64]==0;
  savedWifi.end();String activeSsid=hasSavedWifi?storedWifi.ssid:WIFI_SSID,activePassword=hasSavedWifi?storedWifi.password:WIFI_PASSWORD;
  WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);if(activeSsid.length())WiFi.begin(activeSsid.c_str(),activePassword.c_str());
  configTime(0,0,"pool.ntp.org","time.nist.gov"); // TLS uses UTC; dashboard dates come from the LA calendar on the server.
  uint32_t reconnect=millis();Selection wanted;
  for(;;){
    WifiCommand wifiCommand;
    if(xQueueReceive(wifiCommands,&wifiCommand,0)==pdTRUE){
      WifiReply reply={};reply.type=wifiCommand.type;
      if(wifiCommand.type==WIFI_SCAN){
        const int found=WiFi.scanNetworks();
        if(found<0)strlcpy(reply.message,"Wi-Fi scan failed; tap Scan to retry",sizeof(reply.message));
        else{
          for(int i=0;i<found&&reply.count<WIFI_RESULTS;i++){
            String ssid=WiFi.SSID(i);if(!ssid.length())continue;
            bool duplicate=false;for(int j=0;j<reply.count;j++)if(ssid==reply.networks[j].ssid){duplicate=true;break;}
            if(duplicate)continue;
            WifiNetwork &network=reply.networks[reply.count++];strlcpy(network.ssid,ssid.c_str(),sizeof(network.ssid));
            network.rssi=WiFi.RSSI(i);network.open=WiFi.encryptionType(i)==WIFI_AUTH_OPEN;
          }
          snprintf(reply.message,sizeof(reply.message),"Found %u network(s)",reply.count);reply.ok=true;
        }
        WiFi.scanDelete();
      }else{
        const String oldSsid=activeSsid,oldPassword=activePassword;
        WiFi.disconnect();WiFi.begin(wifiCommand.ssid,wifiCommand.password);
        const uint32_t started=millis();while(WiFi.status()!=WL_CONNECTED&&millis()-started<15000)vTaskDelay(pdMS_TO_TICKS(100));
        if(WiFi.status()==WL_CONNECTED){
          activeSsid=wifiCommand.ssid;activePassword=wifiCommand.password;
          StoredWifi newWifi={0x4C445746};strlcpy(newWifi.ssid,wifiCommand.ssid,sizeof(newWifi.ssid));strlcpy(newWifi.password,wifiCommand.password,sizeof(newWifi.password));
          Preferences writeWifi;const bool opened=writeWifi.begin("lidoll-wifi",false);
          const bool stored=opened&&writeWifi.putBytes("credentials",&newWifi,sizeof(newWifi))==sizeof(newWifi);
          if(opened)writeWifi.end();reply.ok=true;
          snprintf(reply.message,sizeof(reply.message),stored?"Connected to %s":"Connected to %s; save failed",wifiCommand.ssid);
        }else{
          snprintf(reply.message,sizeof(reply.message),"Could not connect to %s",wifiCommand.ssid);
          if(oldSsid.length())WiFi.begin(oldSsid.c_str(),oldPassword.c_str());
        }
      }
      xQueueOverwrite(wifiReplies,&reply);continue;
    }
    if(xQueueReceive(requests,&wanted,pdMS_TO_TICKS(250))!=pdTRUE)continue;
    Snapshot out={};out.serial=wanted.serial;out.index=wanted.index;out.directoryOnly=wanted.directoryOnly;
    if(WiFi.status()!=WL_CONNECTED){
      if(millis()-reconnect>=15000){WiFi.reconnect();reconnect=millis();}
      strlcpy(out.message,"Connecting to Wi-Fi; automatic retry",sizeof(out.message));xQueueOverwrite(results,&out);continue;
    }
    JsonDocument directory;
    if(!readJson("participants?limit="+String(PEOPLE_PER_PAGE)+"&offset="+String(wanted.peopleOffset),directory,out)){xQueueOverwrite(results,&out);continue;}
    if(!directory["total"].is<uint32_t>()||!directory["participants"].is<JsonArray>()||!directory["scope"].is<const char*>()){
      strlcpy(out.message,"Unexpected participant response",sizeof(out.message));xQueueOverwrite(results,&out);continue;
    }
    out.total=directory["total"].as<uint32_t>();out.allAllowed=directory["scope"]=="all";
    out.peopleOffset=wanted.peopleOffset;out.peopleCount=min(uint8_t(PEOPLE_PER_PAGE),uint8_t(directory["participants"].size()));
    for(int i=0;i<out.peopleCount;i++)strlcpy(out.people[i],directory["participants"][i]["label"]|"Participant",sizeof(out.people[i]));
    if(wanted.directoryOnly){out.ok=true;xQueueOverwrite(results,&out);continue;}
    bool individual=wanted.individual||!out.allAllowed;
    String query="summary?days="+String(wanted.days);
    if(individual){
      const uint32_t row=wanted.index>=wanted.peopleOffset?wanted.index-wanted.peopleOffset:PEOPLE_PER_PAGE;
      if(row>=directory["participants"].size()){out.clear=true;strlcpy(out.message,"Participant list changed; choose a person",sizeof(out.message));xQueueOverwrite(results,&out);continue;}
      const char *id=directory["participants"][row]["id"]|"";
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
void buttonAt(int x,int y,int width,int height,const char *label,bool active=false) {
  screen.fillRoundRect(x,y,width,height,8,active?PINK:CARD);textAt(label,x+12,y+(height-16)/2,2,active?BG:INK);
}
void button(int x,int width,const char *label,bool active) { // Large targets let fingers operate the display without a stylus.
  buttonAt(x,405,width,44,label,active);
}
void card(int x,int y,const char *label,const String &value) { // Group a metric label and value in a consistent card.
  screen.fillRoundRect(x,y,246,82,10,CARD);textAt(label,x+14,y+10,2,MUTED);textAt(value,x+14,y+36,3,INK);
}
void paintPeople() {
  textAt("Choose statistics",20,18,3,PINK);
  buttonAt(40,60,720,44,"Everyone",!selection.individual&&allAllowed);
  if(!allAllowed)textAt("Not available for this token",420,75,1,MUTED);
  const bool pageReady=visible.peopleOffset==selection.peopleOffset;
  for(int row=0;row<PEOPLE_PER_PAGE;row++){
    const int y=112+row*52;String label=pageReady&&row<visible.peopleCount?String(visible.people[row]):"";
    if(label.length()>44)label=label.substring(0,43)+"~";
    if(label.length())buttonAt(40,y,720,44,label.c_str(),selection.individual&&selection.index==selection.peopleOffset+row);
    else screen.fillRoundRect(40,y,720,44,8,CARD);
  }
  screen.fillRect(0,365,800,43,BG);
  String pageStatus=!pageReady?(pending?"Loading people...":String(visible.message)):"People "+String(participantCount?selection.peopleOffset+1:0)+"-"+String(min(participantCount,selection.peopleOffset+uint32_t(visible.peopleCount)))+" of "+String(participantCount);
  textAt(pageStatus,40,377,2,MUTED);
  buttonAt(40,420,120,44,"Back");buttonAt(276,420,120,44,"Previous");buttonAt(404,420,120,44,"Next");buttonAt(640,420,120,44,"Refresh");
}
void paintWifi() {
  textAt("Connect Wi-Fi",20,18,3,PINK);
  const uint8_t first=wifiPage*5;
  for(int row=0;row<5;row++){
    const uint8_t index=first+row;const int y=68+row*57;
    if(index<wifiView.count){
      String label=wifiView.networks[index].ssid;if(label.length()>38)label=label.substring(0,37)+"~";
      label+="  ";label+=wifiView.networks[index].open?"open":"locked";label+="  ";label+=String(wifiView.networks[index].rssi);label+=" dBm";
      buttonAt(40,y,720,48,label.c_str());
    }else screen.fillRoundRect(40,y,720,48,8,CARD);
  }
  screen.fillRect(0,355,800,53,BG);textAt(wifiBusy?"Scanning...":String(wifiMessage),40,365,2,wifiBusy?PINK:MUTED);
  buttonAt(40,420,120,44,"Back");buttonAt(276,420,120,44,"Previous");buttonAt(404,420,120,44,"Next");buttonAt(640,420,120,44,"Scan");
}
const char *keyboardRow(uint8_t row) {
  static const char *layouts[3][4]={
    {"1234567890","qwertyuiop","asdfghjkl@","zxcvbnm.-_"},
    {"1234567890","QWERTYUIOP","ASDFGHJKL@","ZXCVBNM.-_"},
    {"!#$%&'()*+",",/:;<=>?[]","^`{|}~\\\"@-","_.$%&*()+="}
  };
  return layouts[keyboardMode][row];
}
void paintWifiPassword() {
  String title="Password for "+String(chosenSsid);if(title.length()>45)title=title.substring(0,44)+"~";textAt(title,20,14,2,PINK);
  screen.fillRect(0,48,800,45,BG);
  String hidden;const size_t passwordLength=strlen(wifiPassword);for(size_t i=0;i<min(size_t(45),passwordLength);i++)hidden+="*";if(passwordLength>45)hidden+="...";if(!hidden.length())hidden="Enter 8-63 characters";
  textAt(hidden,50,58,2,strlen(wifiPassword)?INK:MUTED);
  for(int row=0;row<4;row++){
    const char *keys=keyboardRow(row);
    for(int col=0;keys[col]&&col<10;col++){char label[2]={keys[col],0};buttonAt(50+col*70,105+row*60,62,50,label);}
  }
  screen.fillRect(0,345,800,45,BG);textAt(wifiBusy?"Connecting...":String(wifiMessage),20,356,1,wifiBusy?PINK:MUTED);
  buttonAt(20,398,110,54,"Cancel");buttonAt(140,398,100,54,keyboardMode==0?"abc":keyboardMode==1?"ABC":"#+=");
  buttonAt(250,398,150,54,"Backspace");buttonAt(410,398,120,54,"Space");buttonAt(620,398,160,54,"Connect",true);
}
void paintStatus() {
  screen.fillRect(0,450,800,30,BG);String status=pending?"Updating...":String(visible.message);
  if(!status.length()&&visible.ok)status="Updated "+String((millis()-lastSuccess)/1000)+"s ago | LA calendar | "+String(participantCount)+" participant(s)";
  if(visible.ok&&strlen(visible.message))status="STALE: "+status;
  if(status.length()>90)status=status.substring(0,89);textAt(status,20,459,1,visible.message[0]?PINK:MUTED);
}
void paint() { // The main task alone owns graphics, avoiding races with network requests.
  const bool full=paintedMode!=uint8_t(uiMode);if(full){screen.fillScreen(BG);paintedMode=uint8_t(uiMode);}
  if(!configured){textAt("LITTLE LOG",20,16,2,PINK);textAt("CrowPanel Advance 7 / V1.4",20,70,3);textAt("Copy config.example.h to config.h",20,135);textAt("Set the API URL and admin statistics token.",20,175);textAt("Wi-Fi can then be chosen on this screen.",20,215);return;}
  if(uiMode==UI_PEOPLE){paintPeople();return;}if(uiMode==UI_WIFI){paintWifi();return;}if(uiMode==UI_WIFI_PASSWORD){paintWifiPassword();return;}
  if(!full){screen.fillRect(0,40,800,70,BG);screen.fillRect(0,300,800,105,BG);screen.fillRect(0,450,800,30,BG);}
  textAt("LITTLE LOG",20,16,2,PINK);
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
  button(380,220,"People",selection.individual);button(608,80,"Wi-Fi",false);button(696,84,"Sync",false);
  paintStatus();
}
void refresh(bool changed=false) { // Discard old selections immediately; serial numbers reject their delayed HTTP responses.
  selection.serial++;pending=true;lastPoll=millis();if(changed)visible={};xQueueOverwrite(requests,&selection);
  if(!changed&&uiMode==UI_DASHBOARD&&paintedMode==uint8_t(UI_DASHBOARD))paintStatus();else paint();
}
void refreshPeople() {
  Selection request=selection;request.serial=++selection.serial;request.directoryOnly=true;pending=true;xQueueOverwrite(requests,&request);paint();
}
void scanWifi() {
  if(wifiBusy)return;WifiCommand command={};command.type=WIFI_SCAN;wifiBusy=true;wifiPage=0;strlcpy(wifiMessage,"Starting Wi-Fi scan...",sizeof(wifiMessage));
  xQueueOverwrite(wifiCommands,&command);paint();
}
void connectWifi(const char *ssid,const char *password) {
  if(wifiBusy)return;WifiCommand command={};command.type=WIFI_CONNECT;strlcpy(command.ssid,ssid,sizeof(command.ssid));strlcpy(command.password,password,sizeof(command.password));
  wifiBusy=true;strlcpy(wifiMessage,"Connecting...",sizeof(wifiMessage));xQueueOverwrite(wifiCommands,&command);paint();
}
void handlePeopleTouch(uint16_t x,uint16_t y) {
  if(y>=60&&y<104&&allAllowed){selection.individual=false;uiMode=UI_DASHBOARD;refresh(true);return;}
  if(y>=112&&y<372&&x>=40&&x<760){
    const uint8_t row=(y-112)/52;
    if((y-112)%52<44&&visible.peopleOffset==selection.peopleOffset&&row<visible.peopleCount){
      selection.individual=true;selection.index=selection.peopleOffset+row;uiMode=UI_DASHBOARD;refresh(true);
    }
    return;
  }
  if(y<420||y>=464)return;
  if(x>=40&&x<160){if(selection.individual)selection.peopleOffset=(selection.index/PEOPLE_PER_PAGE)*PEOPLE_PER_PAGE;uiMode=UI_DASHBOARD;paint();}
  else if(x>=276&&x<396&&selection.peopleOffset>=PEOPLE_PER_PAGE){selection.peopleOffset-=PEOPLE_PER_PAGE;refreshPeople();}
  else if(x>=404&&x<524&&selection.peopleOffset+PEOPLE_PER_PAGE<participantCount){selection.peopleOffset+=PEOPLE_PER_PAGE;refreshPeople();}
  else if(x>=640&&x<760)refreshPeople();
}
void handleWifiTouch(uint16_t x,uint16_t y) {
  if(!wifiBusy&&y>=68&&y<353&&x>=40&&x<760){
    const uint8_t row=(y-68)/57,index=wifiPage*5+row;
    if((y-68)%57<48&&index<wifiView.count){
      strlcpy(chosenSsid,wifiView.networks[index].ssid,sizeof(chosenSsid));wifiPassword[0]=0;wifiMessage[0]=0;
      if(wifiView.networks[index].open)connectWifi(chosenSsid,"");else{keyboardMode=0;uiMode=UI_WIFI_PASSWORD;paint();}
    }
    return;
  }
  if(y<420||y>=464)return;
  if(x>=40&&x<160){uiMode=UI_DASHBOARD;paint();}
  else if(!wifiBusy&&x>=276&&x<396&&wifiPage>0){wifiPage--;paint();}
  else if(!wifiBusy&&x>=404&&x<524&&(wifiPage+1)*5<wifiView.count){wifiPage++;paint();}
  else if(x>=640&&x<760)scanWifi();
}
void handleWifiPasswordTouch(uint16_t x,uint16_t y) {
  if(wifiBusy)return;
  if(y>=105&&y<335&&x>=50&&x<750){
    const uint8_t row=(y-105)/60,col=(x-50)/70;
    if((y-105)%60<50&&col<10){
      const char *keys=keyboardRow(row);const size_t length=strlen(wifiPassword);
      if(keys[col]&&length<sizeof(wifiPassword)-1){wifiPassword[length]=keys[col];wifiPassword[length+1]=0;paint();}
    }
    return;
  }
  if(y<398||y>=452)return;
  if(x>=20&&x<130){uiMode=UI_WIFI;wifiMessage[0]=0;paint();}
  else if(x>=140&&x<240){keyboardMode=(keyboardMode+1)%3;paint();}
  else if(x>=250&&x<400){const size_t length=strlen(wifiPassword);if(length)wifiPassword[length-1]=0;paint();}
  else if(x>=410&&x<530){const size_t length=strlen(wifiPassword);if(length<sizeof(wifiPassword)-1){wifiPassword[length]=' ';wifiPassword[length+1]=0;paint();}}
  else if(x>=620&&x<780){
    const size_t length=strlen(wifiPassword);bool valid=length>=8&&length<=63;
    if(length==64){valid=true;for(size_t i=0;i<length;i++)if(!((wifiPassword[i]>='0'&&wifiPassword[i]<='9')||(wifiPassword[i]>='a'&&wifiPassword[i]<='f')||(wifiPassword[i]>='A'&&wifiPassword[i]<='F'))){valid=false;break;}}
    if(!valid){strlcpy(wifiMessage,"Use 8-63 characters or a 64-character hex key",sizeof(wifiMessage));paint();}
    else connectWifi(chosenSsid,wifiPassword);
  }
}
void setup() { // Initialize the panel and request a first read without waiting for a network connection.
  Serial.begin(115200);
  if(!startPanel()){Serial.println("Panel initialization failed. Check OPI PSRAM and board version.");for(;;)delay(1000);}
  configured=strlen(STATS_API_BASE)>0&&strlen(STATS_TOKEN)>0;
  requests=xQueueCreate(1,sizeof(Selection));results=xQueueCreate(1,sizeof(Snapshot));wifiCommands=xQueueCreate(1,sizeof(WifiCommand));wifiReplies=xQueueCreate(1,sizeof(WifiReply));
  if(!requests||!results||!wifiCommands||!wifiReplies){configured=false;paint();textAt("Queue allocation failed; restart device.",20,275);return;}
  paint();if(!configured)return;
  if(xTaskCreatePinnedToCore(networkTask,"statistics",16384,nullptr,1,nullptr,0)!=pdPASS){configured=false;textAt("Network task allocation failed",20,275);return;}
  refresh();
}
void loop() { // Handle touch continuously while the worker reconnects or waits on the server.
  if(!configured){delay(50);return;}
  WifiReply wifiIncoming;
  if(xQueueReceive(wifiReplies,&wifiIncoming,0)==pdTRUE){
    wifiBusy=false;strlcpy(wifiMessage,wifiIncoming.message,sizeof(wifiMessage));
    if(wifiIncoming.type==WIFI_SCAN){wifiView=wifiIncoming;wifiPage=0;}
    else if(wifiIncoming.ok){uiMode=UI_DASHBOARD;refresh();}
    paint();
  }
  Snapshot incoming;
  if(xQueueReceive(results,&incoming,0)==pdTRUE&&incoming.serial==selection.serial){
    pending=false;
    if(incoming.ok&&incoming.directoryOnly){
      participantCount=incoming.total;allAllowed=incoming.allAllowed;visible.peopleOffset=incoming.peopleOffset;visible.peopleCount=incoming.peopleCount;
      for(int i=0;i<incoming.peopleCount;i++)strlcpy(visible.people[i],incoming.people[i],sizeof(visible.people[i]));
    }
    else if(incoming.ok){visible=incoming;lastSuccess=millis();participantCount=incoming.total;allAllowed=incoming.allAllowed;if(!allAllowed)selection.individual=true;}
    else{if(incoming.clear){visible={};allAllowed=false;participantCount=0;selection.individual=true;}strlcpy(visible.message,incoming.message,sizeof(visible.message));}
    paint();
  }
  uint16_t x=0,y=0;bool touched=screen.getTouch(&x,&y);
  if(touched&&!touchDown){
    if(uiMode==UI_PEOPLE)handlePeopleTouch(x,y);
    else if(uiMode==UI_WIFI)handleWifiTouch(x,y);
    else if(uiMode==UI_WIFI_PASSWORD)handleWifiPasswordTouch(x,y);
    else if(y>=405&&y<=449){
      if(x>=20&&x<132){selection.days=1;refresh(true);}
      else if(x>=140&&x<252){selection.days=7;refresh(true);}
      else if(x>=260&&x<372){selection.days=31;refresh(true);}
      else if(x>=380&&x<600){uiMode=UI_PEOPLE;paint();}
      else if(x>=608&&x<688){uiMode=UI_WIFI;scanWifi();}
      else if(x>=696&&x<780)refresh(false);
    }
  }
  touchDown=touched;
  const uint32_t interval=visible.ok?max(uint32_t(10000),POLL_INTERVAL_MS):10000;
  if(!pending&&millis()-lastPoll>=interval)refresh();
  if(millis()-lastPaint>=10000){lastPaint=millis();if(uiMode==UI_DASHBOARD)paintStatus();}
  delay(8);
}
