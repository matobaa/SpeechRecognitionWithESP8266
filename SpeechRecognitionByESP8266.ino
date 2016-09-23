#define _DEBUG_

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <osapi.h>
#include <FS.h>

#define DBG_OUTPUT_PORT Serial

#define _BYTE1(x) (  x        & 0xFF )
#define _BYTE2(x) ( (x >>  8) & 0xFF )

const char* host = "api.github.com";
const int httpsPort = 443;

#define FREQUENCY 8000
#define DURATION 5000

// Use web browser to view and copy
// SHA1 fingerprint of the certificate
const char* fingerprint = "CF 05 98 89 CA FF 8E D8 5E 5C E0 C2 E4 F7 E6 C3 C7 50 DD 5C";
// Use WiFiClientSecure class to create TLS connection
WiFiClientSecure client;

ESP8266WebServer server(80);

int adc_bias = 0;

static long toggle_counts;

#define BUFSIZE 0b111111111111
int16_t buffer[2][BUFSIZE+1];

typedef void wdtfnuint32(uint32);
static wdtfnuint32 *ets_delay_us = (wdtfnuint32 *)0x40002ecc;

void init_adc_bias() {
  int i;
  for (i = 0; i < 1 << 8; i++) {
    adc_bias += analogRead(A0);
    os_delay_us(125);
  }
  adc_bias >>= 8;
}

File fd;

ICACHE_RAM_ATTR void t1IntHandler() {
  uint16_t sensorValue = analogRead(A0) - adc_bias;
  buffer[0][toggle_counts&BUFSIZE]=sensorValue;
//  fd.write(_BYTE1(sensorValue));
//  fd.write(_BYTE2(sensorValue));
  if (toggle_counts-- < 0) {
    stop_sampling();
  }
}

void stop_sampling() {
  timer1_disable();
  timer1_detachInterrupt();
  digitalWrite(13, LOW);
  fd.close();
}

// frequency (in hertz) and duration (in milliseconds).
void start_sampling(unsigned int frequency, unsigned long duration) {
  toggle_counts = frequency * (duration / 1000);
  // preparing FS
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS.begin fail");
    return;
  }
  fd = SPIFFS.open("/sample.wav", "w");
  writeRiffHeader(&fd);
  if (!fd) {
    Serial.println("open error");
    return;
  }
  pinMode(13, OUTPUT);
  digitalWrite(13, HIGH);
  // initialize timer
  timer1_disable();
  timer1_isr_init();
  timer1_attachInterrupt(t1IntHandler);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_LOOP);
  timer1_write((clockCyclesPerMicrosecond() * 1000000) / frequency);
}

typedef void wdtfntype();
static wdtfntype *ets_wdt_disable = (wdtfntype *)0x400030f0;
static wdtfntype *ets_wdt_enable = (wdtfntype *)0x40002fa0;
// static wdtfntype *ets_task=(wdtfntype *)0x40000dd0;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(ESP.getResetReason());

  wifi_client();
  // https_connect();
  init_adc_bias();
  ets_wdt_disable();
}

void wifi_client() {
  // Wifi connect
  Serial.print("connecting to ");
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin();
  // WiFi.begin();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void https_connect() {
  Serial.print("connecting to ");
  Serial.println(host);
  if (!client.connect(host, httpsPort)) {
    Serial.println("connection failed");
    return;
  }

  Serial.println(client.verify(fingerprint, host) ? "certificate matches" : "certificate doesn't match");

  String url = "/repos/esp8266/Arduino/commits/master/status";
  Serial.print("requesting URL: ");
  Serial.println(url);

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: BuildFailureDetectorESP8266\r\n" +
               "Connection: close\r\n\r\n");

  Serial.println("request sent");
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    Serial.println(line);    if (line == "\r") {
      Serial.println("headers received");
      break;
    }
  }
  String line = client.readStringUntil('\n');
  if (line.startsWith("{\"state\":\"success\"")) {
    Serial.println("esp8266/Arduino CI successfull!");
  } else {
    Serial.println("esp8266/Arduino CI has failed");
  }
  Serial.println("reply was:");
  Serial.println("==========");
  //Serial.println(line);
  Serial.println("==========");
  Serial.println("closing connection");
}


void doGet() {
  String html = "<h1>WiFi Settings</h1>";
  html += "<form method='post'>";
  html += "  <input type='text' name='ssid' placeholder='ssid'><br>";
  html += "  <input type='text' name='pass' placeholder='pass'><br>";
  html += "  <input type='submit'><br>";
  html += "</form>";
  html += "<a href='start'>start</a> ";
  html += "<a href='sample.wav'>sample.wav</a>";
  server.send(200, "text/html", html);
}

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

void writeRiffHeader(Stream *fd) {
  int size = sizeof(riff_header) - 4 + 4 + FREQUENCY * (DURATION / 1000) * 2;
  strncpy(riff_header.riff, "RIFF", 4);
  riff_header.len1 = size;
  strncpy(riff_header.wave, "WAVE", 4);
  strncpy(riff_header.fmt, "fmt ", 4);              // start of format chunk
  riff_header.formatSize = 16;        // format size
  riff_header.formatCode = 1;         // uncompressed PCM
  riff_header.channelCount = 1;       // 1=monoral
  riff_header.samplingRate = FREQUENCY;
  riff_header.bytesPerSecond = FREQUENCY * 2;
  riff_header.bytesPerBlock = 2;      // 16bit monoral -> 2byte
  riff_header.bitsPerSample = 16;
  strncpy(riff_header.data, "data", 4); // start of data chunk
  riff_header.len2 = FREQUENCY * (DURATION / 1000) * 2; // wave data size
  fd->write((uint8_t*)&riff_header, sizeof(riff_header));
}

void doGetWave() {
  File file = SPIFFS.open("/sample.wav", "r");
  size_t sent = server.streamFile(file, "application/octet-stream");
  file.close();
}

void startRecording() {
  start_sampling(FREQUENCY, DURATION);
  doGet();
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

void webserver() {
  server.on("/", HTTP_GET, doGet);
  server.on("/sample.wav", HTTP_GET, doGetWave);
  server.on("/start", HTTP_GET, startRecording);
  server.on("/", HTTP_POST, doPost);
  server.begin();
  Serial.println("HTTP server started.");
  // do nothing forevermore:
  while (true) {
    server.handleClient();
  }
}

// the loop routine runs over and over again forever:
void loop() {
  //t1IntHandler();
  //os_delay_us(125);
  //  ets_task();
  // yield();
  start_sampling(FREQUENCY, DURATION);
  webserver();
  Serial.println("loop end");
  delay(0);
  // delay(5000);
  // delayMicroseconds(125);
}

void escaped() {
  // read the input on analog pin 0:
  // if the server's disconnected, stop the client:
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(WiFi.status());
    WiFi.printDiag(Serial);
    Serial.println("disconnecting from server.");
    webserver();
  }
}

