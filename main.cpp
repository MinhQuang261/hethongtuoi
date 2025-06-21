#include <Arduino.h>
// ----------- BLYNK CONFIG -----------
#define BLYNK_TEMPLATE_ID   "TMPL6WT2fDqju"
#define BLYNK_TEMPLATE_NAME "Hệ thống tưới tự động"
#define BLYNK_AUTH_TOKEN    "6sG3aJEtbfXQd7v9gr1tmEdGh5t3gcnh"
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <esp_now.h>
#include "esp_wifi.h"
#include <BH1750.h>
#include <BlynkSimpleEsp32.h>


// GPIO định nghĩa
#define RELAY_BOM   26  // Bơm (V6)
#define RELAY_VAN1  27  // Van 1 (V8)
#define RELAY_VAN2  14  // Van 2 (V9)

// Blynk Virtual Pins
#define VPIN_PUMP     V6
#define VPIN_VAN1     V8
#define VPIN_VAN2     V9
#define VPIN_AUTO_OFF V10
#define VPIN_TIME1    V13
#define VPIN_TIME2    V14
#define VPIN_MODE     V11
#define VPIN_LIGHT    V7
#define VPIN_SOIL_THRESHOLD V15 
#define VPIN_LIGHT_THRESHOLD V16

#define MAX_NODES 3  // Số lượng cảm biến (id: 1, 2, ...)
#define PUMP_DELAY 2000  // 2 giây

char ssid[] = "Quang";
char pass[] = "Mt26012003@";

// Biến chế độ
int systemMode = 0;  // 0: Theo độ ẩm, 1: Theo thời gian

// Biến cho chế độ tưới theo thời gian
RTC_DS3231 rtc;
BlynkTimer timer;
int auto_off_duration_sec = 0;  // V10 - hẹn giờ tắt thủ công
int hour1 = -1, minute1 = -1;   // V13 - thời gian tưới 1
int hour2 = -1, minute2 = -1;   // V14 - thời gian tưới 2
bool hasWatered1 = false;
bool hasWatered2 = false;
int currentDay = -1;
int rtcTaskID = -1;            
bool wasWifiConnected = true;  // Đánh dấu trạng thái Wi-Fi trước đó

// Biến cho chế độ tưới theo độ ẩm
BH1750 lightMeter;
float soilMoistureThreshold = 40.0;  // Ngưỡng độ ẩm mặc định
float lightThreshold = 800.0;  // Ngưỡng ánh sáng mặc định (lux)
float currentLux = 0;  
bool pumpOn = false;
bool van1On = false;
bool van2On = false;
bool pumpDelayRunning = false;
unsigned long pumpDelayStart = 0;
bool zoneSoilDry[MAX_NODES + 1] = {false};
unsigned long lastLightSend = 0;
const unsigned long LIGHT_SEND_INTERVAL = 20UL * 60 * 1000;

// Biến cho chức năng tắt theo thứ tự (áp dụng cho tất cả chế độ)
bool pumpShutdownSequence = false;
unsigned long pumpShutdownStart = 0;
bool van1ToTurnOff = false;
bool van2ToTurnOff = false;
void startPumpShutdownSequence();
// Cấu trúc dữ liệu ESP-NOW
typedef struct {
  int id;
  float temp;
  float humid;
  float soilMoisture;
} struct_message;

// ----------- HÀM CHUNG -----------
void setRelay(int relayPin, bool state, int blynkPin) {
  digitalWrite(relayPin, state ? HIGH : LOW);
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.virtualWrite(blynkPin, state ? 255 : 0);
  }

  // Cập nhật biến trạng thái tương ứng
  if (relayPin == RELAY_BOM) {
    pumpOn = state;
    if (state) { 
      // Nếu bơm được bật lại trong khi đang trong quá trình tắt
      if (pumpShutdownSequence) {
        pumpShutdownSequence = false;
        van1ToTurnOff = false;
        van2ToTurnOff = false;
        Serial.println("Hủy quá trình tắt do bơm được bật lại");
      }
      pumpDelayRunning = false;
      // Lập lịch tắt (áp dụng cho tất cả chế độ)
      if (auto_off_duration_sec > 0) {
        timer.setTimeout(auto_off_duration_sec * 1000L, []() {
          startPumpShutdownSequence();
          Serial.printf("Tự động bắt đầu quá trình tắt sau %d giây bật bơm (áp dụng cho tất cả chế độ).\n", auto_off_duration_sec);
        });
      }
    }
  } 
  else if (relayPin == RELAY_VAN1) van1On = state;
  else if (relayPin == RELAY_VAN2) van2On = state;

  // Nếu van được bật và bơm chưa bật, kích hoạt timer bật bơm (áp dụng cho tất cả chế độ)
  if ((relayPin == RELAY_VAN1 || relayPin == RELAY_VAN2) && state && !pumpOn && !pumpDelayRunning) {
    pumpDelayStart = millis();
    pumpDelayRunning = true;
    Serial.println("Van được bật, bắt đầu đếm 2s để bật bơm");
  }
}

// Hàm bắt đầu quá trình tắt bơm trước, van sau (áp dụng cho tất cả chế độ)
void startPumpShutdownSequence() {
  if (pumpOn) {
    // Bắt đầu quá trình tắt: tắt bơm trước
    setRelay(RELAY_BOM, false, VPIN_PUMP);
    pumpShutdownSequence = true;
    pumpShutdownStart = millis();
    
    // Lưu trạng thái van hiện tại để tắt sau
    van1ToTurnOff = van1On;
    van2ToTurnOff = van2On;
    
    Serial.println("Bắt đầu quá trình tắt: bơm đã tắt, chờ 2s trước khi tắt van");
  } else {
    // Nếu bơm đã tắt, tắt van ngay lập tức (chỉ khi không có van nào cần giữ)
    if (!van1ToTurnOff) setRelay(RELAY_VAN1, false, VPIN_VAN1);
    if (!van2ToTurnOff) setRelay(RELAY_VAN2, false, VPIN_VAN2);
  }
}

// Kiểm tra quá trình tắt (áp dụng cho tất cả chế độ)
void checkPumpShutdownSequence() {
  if (pumpShutdownSequence && millis() - pumpShutdownStart >= PUMP_DELAY) {
    // Sau 20s, tắt các van cần tắt
    if (van1ToTurnOff) {
      setRelay(RELAY_VAN1, false, VPIN_VAN1);
      Serial.println("Đã tắt van 1 sau khi bơm tắt 20s");
    }
    if (van2ToTurnOff) {
      setRelay(RELAY_VAN2, false, VPIN_VAN2);
      Serial.println("Đã tắt van 2 sau khi bơm tắt 20s");
    }
    
    // Reset các biến
    pumpShutdownSequence = false;
    van1ToTurnOff = false;
    van2ToTurnOff = false;
  }
}

// ----------- CHẾ ĐỘ TƯỚI THEO THỜI GIAN -----------
void startIrrigation() {
  Serial.println(">> Bật van 1 và van 2...");
  setRelay(RELAY_VAN1, true, VPIN_VAN1);
  setRelay(RELAY_VAN2, true, VPIN_VAN2);
  
  // Bơm sẽ tự động bật sau 20s nhờ cơ chế trong checkPumpDelay
}

void checkTimeMatch() {
  if (systemMode != 1) return;  // Chỉ chạy trong chế độ tưới theo thời gian

  DateTime now = rtc.now();
  int h = now.hour();
  int m = now.minute();
  int d = now.day();

  Serial.printf("%02d:%02d:%02d | Ngày %02d\n", h, m, now.second(), d);

  if (currentDay != d) {
    hasWatered1 = false;
    hasWatered2 = false;
    currentDay = d;
    Serial.println("Reset tưới cho ngày mới");
  }

  if (h == hour1 && m == minute1 && !hasWatered1) {
    hasWatered1 = true;
    Serial.println("Đến giờ tưới 1");
    startIrrigation();
  }

  if (h == hour2 && m == minute2 && !hasWatered2) {
    hasWatered2 = true;
    Serial.println("Đến giờ tưới 2");
    startIrrigation();
  }
}

void checkWifiStatus() {
  bool isConnected = WiFi.status() == WL_CONNECTED;

  if (isConnected && !wasWifiConnected) {
    // Wi-Fi mới kết nối lại
    if (rtcTaskID != -1) {
      timer.deleteTimer(rtcTaskID);
      rtcTaskID = -1;
      Serial.println("Wi-Fi đã kết nối -> Dừng tưới theo RTC");
    }
    wasWifiConnected = true;
    
    // Đồng bộ lại các giá trị từ Blynk
    if (systemMode == 1) {
      Blynk.syncVirtual(VPIN_TIME1);
      Blynk.syncVirtual(VPIN_TIME2);
      Blynk.syncVirtual(VPIN_AUTO_OFF);
    }
  }

  if (!isConnected && wasWifiConnected) {
    // Wi-Fi mới mất
    if (systemMode == 1) {  // Chỉ bật tưới theo RTC nếu đang ở chế độ thời gian
      rtcTaskID = timer.setInterval(5000L, checkTimeMatch);
      Serial.println("Wi-Fi bị mất -> Bật tưới theo RTC");
    }
    wasWifiConnected = false;
  }
}

// ----------- CHẾ ĐỘ TƯỚI THEO ĐỘ ẨM -----------
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  if (systemMode != 0) return;  // Chỉ xử lý trong chế độ tưới theo độ ẩm

  // Kiểm tra kích thước dữ liệu nhận được
  if (len != sizeof(struct_message)) {
    Serial.printf("Lỗi kích thước dữ liệu! Nhận được %d byte, cần %d byte\n", len, sizeof(struct_message));
    return;
  }

  // Ép kiểu dữ liệu nhận được vào struct
  struct_message *sensorData = (struct_message *)incomingData;
  
  int id = sensorData->id;
  float soil = sensorData->soilMoisture;

  Serial.printf("Nhận dữ liệu từ ID %d | Nhiệt độ: %.1fC | Ẩm không khí: %.1f%% | Ẩm đất: %.1f%%\n", 
                id, sensorData->temp, sensorData->humid, soil);

  // Xử lý logic tưới theo độ ẩm đất
  if (soil < soilMoistureThreshold) {
    if (currentLux > lightThreshold) {
      Serial.printf("Không tưới khu vực %d do trời nắng (%.0f lux > %.0f lux)\n", id, currentLux, lightThreshold);
      zoneSoilDry[id] = false;
    } else {
      zoneSoilDry[id] = true;
      Serial.printf("Khu vực %d cần tưới (độ ẩm %.1f%% < %.1f%%)\n", id, soil, soilMoistureThreshold);

      // Bật van tương ứng
      if (id == 1) {
        setRelay(RELAY_VAN1, true, VPIN_VAN1);
      } else if (id == 2) {
        setRelay(RELAY_VAN2, true, VPIN_VAN2);
      }
    }
  } else {
    zoneSoilDry[id] = false;
    Serial.printf("Khu vực %d đủ ẩm (%.1f%% >= %.1f%%)\n", id, soil, soilMoistureThreshold);

    // Đánh dấu van cần tắt (sẽ tắt sau khi tắt bơm)
    if (id == 1 && van1On) {
      van1ToTurnOff = true;
    } else if (id == 2 && van2On) {
      van2ToTurnOff = true;
    }

    // Kiểm tra nếu tất cả khu vực đã đủ ẩm thì tắt bơm
    bool needWatering = false;
    for (int i = 1; i <= MAX_NODES; i++) {
      if (zoneSoilDry[i]) {
        needWatering = true;
        break;
      }
    }

    if (!needWatering && pumpOn) {
      startPumpShutdownSequence();
      Serial.println("Tất cả khu vực đã đủ ẩm, bắt đầu quy trình tắt bơm");
    }
  }
}

void checkPumpDelay() {
  // Kiểm tra timer bật bơm (áp dụng cho tất cả chế độ)
  if (pumpDelayRunning && !pumpOn && millis() - pumpDelayStart >= PUMP_DELAY) {
    bool shouldTurnOnPump = false;
    
    if (systemMode == 0) {
      // Chế độ độ ẩm: kiểm tra nếu có vùng nào cần tưới
      for (int i = 1; i <= MAX_NODES; i++) {
        if (zoneSoilDry[i]) {
          shouldTurnOnPump = true;
          break;
        }
      }
    } else {
      // Chế độ thời gian: kiểm tra nếu có van nào đang bật
      shouldTurnOnPump = van1On || van2On;
    }

    if (shouldTurnOnPump) {
      setRelay(RELAY_BOM, true, VPIN_PUMP);
      Serial.println("Bơm đã được bật sau 2s chờ van mở");
      
      // Lập lịch tắt (áp dụng cho tất cả chế độ)
      if (auto_off_duration_sec > 0) {
        timer.setTimeout(auto_off_duration_sec * 1000L, []() {
          startPumpShutdownSequence();
          Serial.printf("Tự động bắt đầu quá trình tắt sau %d giây bật bơm.\n", auto_off_duration_sec);
        });
      }
    }
    pumpDelayRunning = false;
  }
}

void sendLightData() {
  currentLux = lightMeter.readLightLevel();
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.virtualWrite(VPIN_LIGHT, currentLux);
  }
  Serial.printf("Cường độ ánh sáng: %.2f lux\n", currentLux);
}

// ----------- BLYNK CALLBACKS -----------
BLYNK_WRITE(VPIN_PUMP) {
  int value = param.asInt();
  if (value == 0) {
    startPumpShutdownSequence(); // Tắt theo trình tự nếu tắt từ Blynk (áp dụng cho tất cả chế độ)
  } else {
    setRelay(RELAY_BOM, value, VPIN_PUMP);
  }
  Serial.printf("Bơm set to: %d\n", value);
}

BLYNK_WRITE(VPIN_VAN1) {
  int value = param.asInt();
  setRelay(RELAY_VAN1, value, VPIN_VAN1);
  Serial.printf("Van 1 set to: %d\n", value);
}

BLYNK_WRITE(VPIN_VAN2) {
  int value = param.asInt();
  setRelay(RELAY_VAN2, value, VPIN_VAN2);
  Serial.printf("Van 2 set to: %d\n", value);
}

BLYNK_WRITE(VPIN_AUTO_OFF) {
  auto_off_duration_sec = param.asInt();
  Serial.printf("Thời gian tự động tắt: %d giây (áp dụng cho tất cả chế độ)\n", auto_off_duration_sec);
}

BLYNK_WRITE(VPIN_TIME1) {
  int seconds = param.asInt();
  hour1 = seconds / 3600;
  minute1 = (seconds % 3600) / 60;
  Serial.printf("Đã nhận giờ tưới 1: %02d:%02d\n", hour1, minute1);
}

BLYNK_WRITE(VPIN_TIME2) {
  int seconds = param.asInt();
  hour2 = seconds / 3600;
  minute2 = (seconds % 3600) / 60;
  Serial.printf("Đã nhận giờ tưới 2: %02d:%02d\n", hour2, minute2);
}

BLYNK_WRITE(VPIN_SOIL_THRESHOLD) {
  int newThreshold = param.asInt();
  if (newThreshold >= 0 && newThreshold <= 100) {
    soilMoistureThreshold = newThreshold;
    Serial.printf("Đã cập nhật ngưỡng độ ẩm tưới: %d%%\n", newThreshold);
  } else {
    Serial.println("Giá trị ngưỡng không hợp lệ! (0 - 100%)");
  }
}

BLYNK_WRITE(VPIN_LIGHT_THRESHOLD) {
  int newLightThreshold = param.asInt();
  if (newLightThreshold >= 0 && newLightThreshold <= 65565) {
    lightThreshold = newLightThreshold;
    Serial.printf("Đã cập nhật ngưỡng ánh sáng: %d lux\n", newLightThreshold);
  } else {
    Serial.println("Ngưỡng ánh sáng không hợp lệ!");
  }
}

BLYNK_WRITE(VPIN_MODE) {
  int newMode = param.asInt();
  if (newMode == 0 || newMode == 1) {
    systemMode = newMode;
    Serial.printf("Đã chuyển sang chế độ: %d\n", systemMode);
    
    // Tắt tất cả relay khi chuyển chế độ (áp dụng cho tất cả chế độ)
    startPumpShutdownSequence();
    
    // Reset các trạng thái
    pumpDelayRunning = false;
    if (systemMode == 0) {
      for (int i = 0; i <= MAX_NODES; i++) {
        zoneSoilDry[i] = false;
      }
    } else {
      hasWatered1 = false;
      hasWatered2 = false;
      // Nếu đang mất kết nối WiFi, bật ngay chế độ RTC
      if (WiFi.status() != WL_CONNECTED) {
        if (rtcTaskID == -1) {
          rtcTaskID = timer.setInterval(5000L, checkTimeMatch);
          Serial.println("Đang mất WiFi -> Bật tưới theo RTC ngay khi chuyển chế độ");
        }
      }
    }
  } else {
    Serial.println("Chế độ không hợp lệ! (0: Theo độ ẩm, 1: Theo thời gian)");
  }
}

// ----------- SETUP -----------
void setup() {
  Serial.begin(115200);

  // Khởi tạo relay
  pinMode(RELAY_BOM, OUTPUT);
  pinMode(RELAY_VAN1, OUTPUT);
  pinMode(RELAY_VAN2, OUTPUT);
  setRelay(RELAY_BOM, false, VPIN_PUMP);
  setRelay(RELAY_VAN1, false, VPIN_VAN1);
  setRelay(RELAY_VAN2, false, VPIN_VAN2);

  // Khởi tạo RTC
  Wire.begin();
  rtc.begin();
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println("RTC bị mất nguồn, thiết lập lại thời gian.");
  }

  // Khởi tạo ESP-NOW
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(9, WIFI_SECOND_CHAN_NONE);
  esp_wifi_start();
  if (esp_now_init() != ESP_OK) {
    Serial.println("Lỗi khởi tạo ESP-NOW");
  } else {
    esp_now_register_recv_cb(OnDataRecv);
  }

  // Khởi tạo cảm biến ánh sáng
  Wire.begin(21, 22);
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("Không tìm thấy cảm biến BH1750!");
  }

  // Kết nối Blynk
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  
  // Thiết lập timer
  timer.setInterval(10000L, checkWifiStatus);
  timer.setInterval(1000L, checkPumpDelay);
  timer.setInterval(1000L, checkPumpShutdownSequence);
  
  // Gửi giá trị ban đầu lên Blynk
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.syncVirtual(VPIN_SOIL_THRESHOLD);
    Blynk.syncVirtual(VPIN_LIGHT_THRESHOLD);
    Blynk.syncVirtual(VPIN_AUTO_OFF);
  }
  sendLightData();
  lastLightSend = millis();
}

// ----------- LOOP -----------
void loop() {
  Blynk.run();
  timer.run();

  // Gửi dữ liệu ánh sáng định kỳ
  if (millis() - lastLightSend > LIGHT_SEND_INTERVAL) {
    sendLightData();
    lastLightSend = millis();
  }
}