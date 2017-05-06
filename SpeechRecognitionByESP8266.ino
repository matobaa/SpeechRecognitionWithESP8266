#include <GDBStub.h>

/* Flash mode: DIO
   Flash Freq: 40MHz
   CPU Freq: 80MHz
   Flash size: 512K (64K SPIFFS)
   Debug Port: Disabled
   Debug Level: Core
   Reset Method: ck
   Upload Speed: 115200
*/

#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <Ticker.h>

#ifdef DEBUG_ESP_PORT
#define DEBUG_MSG(...) DEBUG_ESP_PORT.printf( __VA_ARGS__ )
#else
#define DEBUG_MSG(...)
#endif

#define FREQUENCY 8000
#define DURATION 5000
#define INDICATORPIN 13

const char* host = "stream.watsonplatform.net";
const int httpsPort = 443;

// wifi credentials
const char* ssid = "07056547823";
const char* pass = "0466369371";
// bluemix credentials
const String username = "0857b1a4-5d6c-4e57-b49d-e4a01164c9ac";
const String password = "5UFTIalV1g4h";
const char* fingerprint = "07 AA 36 ED 31 A0 FE BC 5B 41 9E D2 BF 8F 5D 62 05 D2 56 EE";

String payload = "";

ESP8266WebServer server(80);
Ticker bufferWriter;
File fd;

#define BUFBIT 9
#define BUFSIZE (1<<BUFBIT)
#define BUFMASK (BUFSIZE-1)

int adc_bias = 0;
long toggle_counts;
long counter = 0;
int16_t buffer[2][BUFSIZE];

typedef void wdtfnuint32(uint32);
static wdtfnuint32 *ets_delay_us = (wdtfnuint32 *)0x40002ecc;
typedef void wdtfntype();
static wdtfntype *ets_wdt_disable = (wdtfntype *)0x400030f0;
static wdtfntype *ets_wdt_enable = (wdtfntype *)0x40002fa0;
// static wdtfntype *ets_task = (wdtfntype *)0x40000dd0;

void init_adc_bias() {
  for (int i = 0; i < 1 << 8; i++) {
    adc_bias += analogRead(A0);
    os_delay_us(125);
  }
  adc_bias >>= 8;
}

ICACHE_RAM_ATTR void t1IntHandler() {
  uint16_t sensorValue = analogRead(A0) - adc_bias;
  buffer[(counter >> BUFBIT) & 1][counter & BUFMASK] = sensorValue;
  counter++;
  if ((counter & BUFMASK) == 0) { // overflow
    digitalWrite(INDICATORPIN, LOW);
  }
  if (counter > toggle_counts) {
    stop_sampling();
  }
}

void flush_buffer() {
  if (digitalRead(INDICATORPIN) == HIGH) return; // do nothing
  DEBUG_MSG("Flushing Buffer:%d %d\n", 1 - (counter >> BUFBIT) & 1, 1 << (BUFBIT + 1));
  int result = fd.write((uint8_t*)buffer[1 - (counter >> BUFBIT) & 1], 1 << (BUFBIT + 1));
  if (!result) {
    //    Serial.println(SPIFFS_errno(&fd));
  }
  digitalWrite(INDICATORPIN, HIGH);
}

// frequency (in hertz) and duration (in milliseconds).
void start_sampling(unsigned int frequency, unsigned long duration) {

  toggle_counts = frequency * (duration / 1000);
  // preparing FS

  fd = SPIFFS.open("/sample.wav", "w");
  if (!fd) {
    Serial.println("open error");
    return;
  }
  writeRiffHeader(&fd);
  pinMode(INDICATORPIN, OUTPUT);
  digitalWrite(INDICATORPIN, HIGH);
  DEBUG_MSG("Sampling Started\n");
  // initialize buffer writer
  bufferWriter.attach(0.02, flush_buffer);
  // initialize timer
  counter = 0;
  timer1_disable();
  timer1_isr_init();
  timer1_attachInterrupt(t1IntHandler);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_LOOP);
  timer1_write((clockCyclesPerMicrosecond() * 1000000) / frequency);
}

void stop_sampling() {
  timer1_disable();
  timer1_detachInterrupt();
  bufferWriter.detach();
  flush_buffer();
  DEBUG_MSG("Flushing Buffer:%d %d\n", (counter >> BUFBIT) & 1, (counter & BUFMASK) * 2 - 2);
  fd.write((uint8_t*)buffer[(counter >> BUFBIT) & 1], (counter & BUFMASK) * 2 - 2);
  fd.close();
  digitalWrite(INDICATORPIN, LOW);
  DEBUG_MSG("Sampling Finished\n");
}

void wifi_client() {
  // Wifi connect
  Serial.print("connecting to ");
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, pass);
  // WiFi.begin();
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED)  break;
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.localIP());
}

/*
   curl.exe -X POST -u username:password
   --header "Content-Type: audio/wav"
   --data-binary @sample.wav
   https://stream.watsonplatform.net/speech-to-text/api/v1/models/ja-JP_NarrowbandModel/recognize
*/
void https_connect() {
  HTTPClient client;
  String url = "https://";
  url += username;
  url += ":";
  url += password;
  url += "@";
  url += "stream.watsonplatform.net/speech-to-text/api/v1/models/ja-JP_NarrowbandModel/recognize";
  client.begin(url, String(fingerprint));
  client.addHeader("Content-Type", "audio/wav");
  client.addHeader("Connection", "close");

  Serial.println("Sending for recognize request...");
  // start connection and send HTTP header

  File file = SPIFFS.open("/sample.wav", "r");
  long size = file.size();
  DEBUG_MSG("Content-Length: %d\n", size);
  int httpCode = client.sendRequest("POST", &file, size);
  Serial.printf("Request for recognize sent.\n", size);
  file.close();

  // httpCode will be negative on error
  if (httpCode > 0) {
    // HTTP header has been send and Server response header has been handled
    DEBUG_MSG("[HTTP] GET... code: %d\n", httpCode);

    // file found at server
    if (httpCode == HTTP_CODE_OK) {
      payload = client.getString();
      Serial.println(payload.c_str());
    }
  } else {
    DEBUG_MSG("[HTTP] GET... failed, error: %s\n", client.errorToString(httpCode).c_str());
  }
  client.end();
}

void doGet() {
  String html = "<h1>WiFi Settings</h1>";
  html += "<form method='post'>";
  html += "  <input type='text' name='ssid' placeholder='ssid'><br>";
  html += "  <input type='text' name='pass' placeholder='pass'><br>";
  html += "  <input type='submit'><br>";
  html += "</form>";
  html += "<a href='start'>sampling</a> ";
  html += "<a href='sample.wav'>download</a> ";
  html += "<a href='recognize'>recognize</a> ";
  html += payload;
  server.send(200, "text/html", html);
}

void writeRiffHeader(Stream *fd) {
  struct {
    char riff[4];
    int32_t len1;
    char wave[4];
    char fmt[4];
    int32_t formatSize;
    int16_t formatCode;
    int16_t channelCount;
    int32_t samplingRate;
    int32_t bytesPerSecond;
    int16_t bytesPerBlock;
    int16_t bitsPerSample;
    char data[4];
    int32_t len2;
  } riff_header;
  int size = sizeof(riff_header) - 4 + 4 + FREQUENCY * (DURATION / 1000) * 2;
  strncpy(riff_header.riff, "RIFF", 4);
  riff_header.len1 = size;
  strncpy(riff_header.wave, "WAVE", 4);
  strncpy(riff_header.fmt, "fmt ", 4);  // start of format chunk
  riff_header.formatSize = 16;
  riff_header.formatCode = 1;  // uncompressed PCM
  riff_header.channelCount = 1; // monoral
  riff_header.samplingRate = FREQUENCY; // Heltz
  riff_header.bytesPerSecond = FREQUENCY * 2;
  riff_header.bytesPerBlock = 2;  // 16bit monoral -> 2byte
  riff_header.bitsPerSample = 16; // 16bit
  strncpy(riff_header.data, "data", 4);  // start of data chunk
  riff_header.len2 = FREQUENCY * (DURATION / 1000) * 2;  // wave data size
  fd->write((uint8_t*)&riff_header, sizeof(riff_header));
}

void doGetWave() {
  File file = SPIFFS.open("/sample.wav", "r");
  size_t sent = server.streamFile(file, "application/octet-stream");
  file.close();
}

void startRecording() {
  start_sampling(FREQUENCY, DURATION);
  server.sendHeader("Location", "/");
  server.send(302);
}

void doPost() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  WiFi.begin(ssid.c_str(), pass.c_str());
  String html = "<h1>WiFi Settings</h1>";
  html += ssid + "<br>";
  html += pass + "<br>";
  server.send(200, "text/html", html);
}

void doRecognize() {
  server.sendHeader("Location", "/");
  server.send(302);
  https_connect();
}

void webserver() {
  server.on("/", HTTP_GET, doGet);
  server.on("/sample.wav", HTTP_GET, doGetWave);
  server.on("/start", HTTP_GET, startRecording);
  server.on("/recognize", HTTP_GET, https_connect);
  server.on("/", HTTP_POST, doPost);
  server.begin();
  Serial.println("HTTP server started.");
  // do nothing forevermore:
  while (true) {
    server.handleClient();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.print("Reset by ");
  Serial.println(ESP.getResetReason());


  init_adc_bias();
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS.begin fail");
    return;
  }
  SPIFFS.format();

  ESP.wdtDisable();

  //   start_sampling(FREQUENCY, DURATION);
  delay(5000);
  wifi_client();
  //  https_connect();
  webserver();
}

void loop() {
}
