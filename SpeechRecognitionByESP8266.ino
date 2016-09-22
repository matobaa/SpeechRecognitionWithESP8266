#define _DEBUG_

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <osapi.h>

#define DBG_OUTPUT_PORT Serial

#define _BYTE1(x) (  x        & 0xFF )
#define _BYTE2(x) ( (x >>  8) & 0xFF )
#define _BYTE3(x) ( (x >> 16) & 0xFF )
#define _BYTE4(x) ( (x >> 24) & 0xFF )
#define BYTE_SWAP_16(x) ((uint16_t)( _BYTE1(x)<<8 | _BYTE2(x) ))
#define BYTE_SWAP_32(x) ((uint32_t)( _BYTE1(x)<<24 | _BYTE2(x)<<16 | _BYTE3(x)<<8 | _BYTE4(x) ))


const char* ssid = "07056547823";
const char* password = "0466369371";

const char* host = "api.github.com";
const int httpsPort = 443;

#define FREQUENCY 8000
#define DURATION 2000
#define packsize FREQUENCY * DURATION / 1000 / 4

// Use web browser to view and copy
// SHA1 fingerprint of the certificate
const char* fingerprint = "CF 05 98 89 CA FF 8E D8 5E 5C E0 C2 E4 F7 E6 C3 C7 50 DD 5C";
// Use WiFiClientSecure class to create TLS connection
WiFiClientSecure client;

ESP8266WebServer server(80);

typedef struct {
  uint16_t wav0 : 10;
  uint16_t wav1 : 10;
  uint16_t wav2 : 10;
  uint16_t wav3 : 10;
} WavPack;

static WavPack wavData[packsize];

int adc_bias = 0;

static long toggle_counts;

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

ICACHE_RAM_ATTR void t1IntHandler() {
  uint16_t sensorValue = analogRead(A0);
  WavPack *slot = &wavData[toggle_counts / 4];
  switch (toggle_counts & 3) {
    case 0: slot->wav0 = sensorValue; break;
    case 1: slot->wav1 = sensorValue; break;
    case 2: slot->wav2 = sensorValue; break;
    case 3: slot->wav3 = sensorValue; break;
  }
  toggle_counts--;
  if (toggle_counts < 0) {
    stop_sampling();
  }
}

int decode(long c) {
  WavPack *slot = &wavData[c / 4];
  return ( ((c & 3) == 0) ? slot->wav0 :
           ((c & 3) == 1) ? slot->wav1 :
           ((c & 3) == 2) ? slot->wav2 :
           ((c & 3) == 3) ? slot->wav3 : 99999) - adc_bias;
}

void stop_sampling() {
  timer1_disable();
  timer1_detachInterrupt();
  Serial.println("stopped");
}

// frequency (in hertz) and duration (in milliseconds).
void start_sampling(unsigned int frequency, unsigned long duration) {
  toggle_counts = frequency * (duration / 1000);
  timer1_disable();
  timer1_isr_init();
  timer1_attachInterrupt(t1IntHandler);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_LOOP);
  timer1_write((clockCyclesPerMicrosecond() * 500000) / frequency);
  Serial.println(toggle_counts);
  Serial.println("started");
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
  WiFi.begin(ssid, password);
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
  html += "<a href='sample.wav'>sample.wav</a>";
  server.send(200, "text/html", html);
}


typedef union {
  struct {
    uint32_t long0;
  };
  struct {
    uint16_t int1;
    uint16_t int0;
  };
  struct {
    uint8_t char3;
    uint8_t char2;
    uint8_t char1;
    uint8_t char0;
  };
} Converter;

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


void doGetWave() {

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
  riff_header.len2 = FREQUENCY * (DURATION / 1000); // wave data size

  WiFiClient client = server.client();
  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n");
  client.write((uint8_t*)&riff_header, sizeof(riff_header));
  int counts = FREQUENCY * (DURATION / 1000);
  for (long c = counts; 0 <= c; c--) {
    int sensorValue = decode(c);
    // client.write(_BYTE2(sensorValue));
    // client.write(_BYTE1(sensorValue));
    Serial.println(sensorValue);
  }
  client.stop();
}


void doPost() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  // WiFi.begin(ssid, password);


  String html = "<h1>WiFi Settings</h1>";
  html += ssid + "<br>";
  html += pass + "<br>";
  server.send(200, "text/html", html);
}

void webserver() {
  server.on("/", HTTP_GET, doGet);
  server.on("/sample.wav", HTTP_GET, doGetWave);
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

