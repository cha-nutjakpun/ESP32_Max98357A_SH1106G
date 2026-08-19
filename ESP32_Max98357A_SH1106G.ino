#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h>
#include <DNSServer.h> 

// 1. Async Web Server
#include <ESPAsyncWebServer.h>  //https://github.com/ESP32Async
#include <ESPAsyncWiFiManager.h> //https://github.com/alanswx/ESPAsyncWiFiManager

// 2. ESP32 Audio Libraries
#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioOutputI2S.h"
#include "AudioGeneratorMP3.h"

#define SCREEN_ADDRESS 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1 

Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

Preferences preferences;
char stream_url[256] = "http://utulsa.streamguys1.com/KWGSHD1-MP3";

AsyncWebServer server(80);
DNSServer dns;

AudioFileSourceICYStream *file = nullptr;
AudioFileSourceBuffer *buff = nullptr;
AudioOutputI2S *out = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;

String currentTitle = "Waiting for stream...";
volatile bool isUrlChanged = false;
volatile bool shouldRestart = false;
bool shouldSaveConfig = false;

unsigned long lastRetryTime = 0;
const unsigned long retryInterval = 5000; // ลองเชื่อมต่อใหม่ทุก 5 วินาทีแบบ Non-blocking

void saveConfigCallback() {
  Serial.println("Should save config from WiFiManager");
  shouldSaveConfig = true;
}

// หน้าเว็บ HTML
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title>ESP32 Stream Controller</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; background: #f4f4f9; }
    .container { display: inline-block; text-align: left; padding: 20px; background: white; border-radius: 8px; box-shadow: 0px 0px 10px rgba(0,0,0,0.1); width: 90%; max-width: 400px; font-size: 12px; }
    input[type=text] { width: 100%; padding: 10px; margin: 10px 0; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; }
    input[type=submit] { width: 100%; background-color: #007bff; color: white; padding: 10px; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; }
    input[type=submit]:hover { background-color: #0056b3; }
    .btn-reset { width: 100%; background-color: #dc3545; color: white; padding: 10px; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; margin-top: 15px; }
  </style>
</head>
<body>
  <div class='container'>
    <h2>ESP32 Audio Controller</h2>
    <form action='/update' method='GET'>
      <p><label for='url'>Change MP3 Stream URL:</label></p>
      <p><textarea id='url' name='url' rows='3' cols='50'>%CURRENT_URL%</textarea></p>
      <p><input type='submit' value='Play New Stream'></p>
    </form>
    <p><b>Example BBC news : </b>http://utulsa.streamguys1.com/KWGSHD1-MP3</p>
    <p><b>Moon Phase Radio : </b>https://cp12.serverse.com/proxy/moonphase/stream</p>
    <hr style='border: 0; border-top: 1px solid #ccc; margin: 20px 0;'>
    <form action='/reset_wifi' method='GET'>
      <input type='submit' class='btn-reset' value='Reset Wi-Fi Settings' onclick="return confirm('Are you sure you want to clear Wi-Fi settings?');">
    </form>
  </div>
</body>
</html>
)rawliteral";

String processor(const String& var) {
  if (var == "CURRENT_URL") {
    return String(stream_url);
  }
  return String();
}

void updateDisplay(String header, String body) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println(header);
  display.drawLine(0, 10, 128, 10, SH110X_WHITE);
  
  display.setCursor(0, 16);
  display.println(body);
  display.display();
}

void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string) {
  if (strstr(type, "Title")) {
    String newTitle = String(string);
    newTitle.trim();
    if (newTitle != currentTitle && newTitle.length() > 0) {
      currentTitle = newTitle;
      Serial.printf("New Title: %s\n", currentTitle.c_str());
      updateDisplay("NOW PLAYING:", currentTitle);
    }
  }
}

void startAudioStream(const char* url) {
  if (mp3) { mp3->stop(); delete mp3; mp3 = nullptr; }
  if (buff) { delete buff; buff = nullptr; }
  if (file) { delete file; file = nullptr; }

  Serial.printf("Connecting to URL: %s\n", url);
  updateDisplay("STREAMING:", "Connecting...");

  file = new AudioFileSourceICYStream(url);
  file->RegisterMetadataCB(MDCallback, NULL);
  buff = new AudioFileSourceBuffer(file, 8192); 
  
  mp3 = new AudioGeneratorMP3();
  if(!mp3->begin(buff, out)) {
    updateDisplay("ERROR", "Link error / Offline");
  } else {
    currentTitle = "Fetching title...";
    updateDisplay("NOW PLAYING:", currentTitle);
  }
}

// Trim from start (left)
inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
}
// Trim from end (right)
inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}
inline void trim(std::string &s) {
    ltrim(s);
    rtrim(s);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. จอแสดงผล
  display.begin(SCREEN_ADDRESS);
  display.clearDisplay();
  updateDisplay("STATUS", "Booting up...");

  // 2. โหลด URL เดิมจาก Flash Memory
  preferences.begin("audio-config", false);
  if (preferences.isKey("url")) {
    preferences.getString("url", stream_url, sizeof(stream_url));
  }
  Serial.printf("Loaded URL from Flash: %s\n", stream_url);

  // 3. เริ่มระบบ WiFiManager
  AsyncWiFiManager wm(&server, &dns);
  wm.setSaveConfigCallback(saveConfigCallback);

  AsyncWiFiManagerParameter custom_stream_url("url_key", "Default MP3 Stream URL", stream_url, 256);
  wm.addParameter(&custom_stream_url);

  updateDisplay("WiFi Setup", "Connect Wi-Fi AP:\n'ESP32-Audio-Setup'");

  if (!wm.autoConnect("ESP32-Audio-Setup")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
  }

  String ipStr = WiFi.localIP().toString();
  Serial.println("WiFi Connected. IP: " + ipStr);
  updateDisplay("CONNECTED!", "IP Address:\nhttp://" + ipStr);

  // 4. บันทึกค่า URL กรณีแก้ไขตอนตั้งค่า AP Mode
  if (shouldSaveConfig) {
    strcpy(stream_url, custom_stream_url.getValue());
    preferences.putString("url", stream_url);
    Serial.printf("Saved new setup URL to Flash: %s\n", stream_url);
  }
  preferences.end();

  delay(2000);

  // 5. ตั้งค่า Web Server Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html, processor);
  });

  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("url")) {
      String new_url = request->getParam("url")->value();
      
      preferences.begin("audio-config", false);
      preferences.putString("url", new_url);
      preferences.getString("url", stream_url, sizeof(stream_url));
      preferences.end();

      isUrlChanged = true;
      request->send(200, "text/html", "<h3>URL Updated! Streaming new song...</h3><a href='/'>Go Back</a>");
    } else {
      request->send(400, "text/plain", "Bad Request");
    }
  });

  server.on("/reset_wifi", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "Clearing Wi-Fi settings and restarting...");
    shouldRestart = true;
  });

  server.begin();

  // 6. ตั้งค่าพิน I2S (MAX98357A)
  out = new AudioOutputI2S();
  out->SetPinout(26, 25, 19); // BCLK=26, LRC=25, DIN=19
  out->SetGain(1.0);

  // 7. เริ่มเล่นสตรีม
  startAudioStream(stream_url);
}

void loop() {
  // หากสั่งรีเซ็ต Wi-Fi จากหน้าเว็บ ให้รอแป๊บแล้วรีบูตบอร์ด
  if (shouldRestart) {
    delay(1000);
    WiFi.disconnect(true, true);
    ESP.restart();
  }

  // เปลี่ยน URL เมื่อรับค่าจาก Web Server
  if (isUrlChanged) {
    isUrlChanged = false;
    startAudioStream(stream_url);
  }

  // เล่นเพลงและถอดรหัส MP3 Continuous Loop
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      updateDisplay("STATUS", "Stream Stopped");
    }
  } else {
    // พยายามเชื่อมต่อใหม่แบบ Non-blocking (ใช้ millis() แทน delay)
    if (WiFi.status() == WL_CONNECTED && millis() - lastRetryTime > retryInterval) {
      lastRetryTime = millis();
      if (mp3 && !mp3->isRunning()) {
        startAudioStream(stream_url);
      }
    }
  }
}