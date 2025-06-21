#include <Arduino.h>

// ----------- BLYNK CONFIG -----------
#define BLYNK_TEMPLATE_ID   "TMPL6WT2fDqju"
#define BLYNK_TEMPLATE_NAME "Hệ thống tưới tự động"
#define BLYNK_AUTH_TOKEN    "6sG3aJEtbfXQd7v9gr1tmEdGh5t3gcnh"

#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

// Wi-Fi credentials
char ssid[] = "Quang";
char pass[] = "Mt26012003@";

// Blynk Virtual Pins
#define VPIN_TEMP  V0
#define VPIN_HUMI  V1
#define VPIN_SOIL  V4

// ---------- ESP-NOW CONFIG ----------
#include <esp_now.h>
#include "esp_wifi.h"        

// Địa chỉ MAC của Gateway ESP32
uint8_t gatewayAddress[] = {0x14, 0x2B, 0x2F, 0xC0, 0xBA, 0x54};

// Struct truyền qua ESP-NOW
typedef struct {
  int id;
  float temp;
  float humid;
  float soilMoisture;
} struct_message;

struct_message dataToSend;

// ---------- SENSOR CONFIG ----------
#include <Wire.h>
#include <Adafruit_HTU21DF.h>

Adafruit_HTU21DF htu;
const int soilPin = 35;  // GPIO35 (analog)

// ---------- TIMING ----------
unsigned long lastRead = 0;
const unsigned long READ_INTERVAL = 10000;          // 10 giây
unsigned long lastBlynkSend = 0;
const unsigned long BLYNK_SEND_INTERVAL = 300000;   // 100 giây

// ---------- CALLBACK ----------
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("ESP-NOW gửi: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Thành công" : "Thất bại");
}

// ---------- BLYNK CONNECTED ----------
BLYNK_CONNECTED() {
  Blynk.syncVirtual(VPIN_TEMP, VPIN_HUMI, VPIN_SOIL);
}

// ---------- SENSOR READING + SEND ----------
void readAndSendData() {
  float temp = htu.readTemperature();
  float humid = htu.readHumidity();
  int rawSoil = analogRead(soilPin);
  float soilMoisture = constrain(map(rawSoil, 3200, 1200, 0, 100), 0, 100);

  Serial.printf("Nhiệt độ: %.2f°C | Độ ẩm: %.2f%% | Độ ẩm đất: %.2f%%\n",
                temp, humid, soilMoisture);

  // Gửi lên Blynk
  unsigned long now = millis();
  if (now - lastBlynkSend >= BLYNK_SEND_INTERVAL) {
    Blynk.virtualWrite(VPIN_TEMP, temp);
    Blynk.virtualWrite(VPIN_HUMI, humid);
    Blynk.virtualWrite(VPIN_SOIL, soilMoisture);
    lastBlynkSend = now;
    Serial.println(">> Đã gửi dữ liệu lên Blynk");
  }

  // Gửi qua ESP-NOW
  dataToSend.id = 1;
  dataToSend.temp = temp;
  dataToSend.humid = humid;
  dataToSend.soilMoisture = soilMoisture;

  esp_err_t result = esp_now_send(gatewayAddress, (uint8_t *)&dataToSend, sizeof(dataToSend));
  if (result != ESP_OK) {
    Serial.print("Gửi ESP-NOW lỗi code: ");
    Serial.println(result);
  }
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Khởi tạo I2C và cảm biến
  Wire.begin(21, 22);
  if (!htu.begin()) {
    Serial.println("Không tìm thấy cảm biến HTU21D!");
    while (1) delay(1000);
  }
  pinMode(soilPin, INPUT);

  // Kết nối Wi-Fi để lấy channel
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  Serial.println("\n>> Đã kết nối Wi-Fi");

  // Lấy kênh Wi-Fi hiện tại
  wifi_second_chan_t second;
  uint8_t primary_channel;
  esp_wifi_get_channel(&primary_channel, &second);
  Serial.printf("Kênh Wi-Fi hiện tại: %d\n", primary_channel);

  // Cài đặt channel cho ESP-NOW
  esp_wifi_set_channel(primary_channel, WIFI_SECOND_CHAN_NONE);

  // Khởi tạo Blynk
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  // Khởi tạo ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Lỗi khởi tạo ESP-NOW");
    return;
  }
  esp_now_register_send_cb(OnDataSent);

  // Cấu hình peer Gateway
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, gatewayAddress, 6);
  peerInfo.channel = primary_channel;  // dùng đúng kênh đã lấy
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Không thêm được peer Gateway!");
    return;
  }

  Serial.println("Khởi động hoàn tất!");
}

// ---------- LOOP ----------
void loop() {
  Blynk.run();

  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL) {
    readAndSendData();
    lastRead = now;
  }
}
