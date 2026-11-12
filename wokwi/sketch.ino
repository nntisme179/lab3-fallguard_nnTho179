/* De 5 - ESP32 / MPU6050 / FreeRTOS / MQTT
   Core 0: TaskTruyenData. Core 1: TaskDocData + TaskCanhBao.
   dataQueue va dataAlert la mailbox 1 phan tu: luon giu mau moi nhat.
   Demo hoc tap; nguong can hieu chinh khi gan len co the thuc.
*/
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <math.h>
#include "detector.h"

constexpr uint8_t PIN_LED = 26, PIN_BUZZER = 25;
constexpr char MQTT_HOST[] = "broker.emqx.io";
constexpr char MQTT_BASE[] = "fd-demo-81472"; // trung dashboard
constexpr uint32_t SAMPLE_MS = 50, PUBLISH_MS = 400;
Adafruit_MPU6050 mpu;
WiFiClient transport;
PubSubClient mqtt(transport); // CHI TaskTruyenData duoc su dung
QueueHandle_t dataQueue, dataAlert, commandQueue, simulationQueue;
SemaphoreHandle_t stateMutex;

// Dinh nghia struct trong header de Arduino tu tao prototype dung.
#include "messages.h"
DeviceState sharedState{};
String topicTelemetry, topicMode, topicLed, topicBuzzer, topicSim, topicReset;

DeviceState snapshot() {
  DeviceState copy;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  copy = sharedState;
  xSemaphoreGive(stateMutex);
  return copy;
}

void onMessage(char *topic, byte *payload, unsigned int len) {
  if (len == 0 || len > 16) return;
  char text[17]; memcpy(text, payload, len); text[len] = 0;
  Command cmd{};
  const String t(topic);
  if (t == topicSim) {
    if (strcmp(text, "START") && strcmp(text, "STOP")) return;
    bool start = !strcmp(text, "START");
    xQueueOverwrite(simulationQueue, &start);
    return;
  }
  if (t == topicMode && (!strcmp(text,"AUTO") || !strcmp(text,"MANUAL"))) {
    cmd.type = CMD_MODE; cmd.on = !strcmp(text,"AUTO");
  } else if ((t == topicLed || t == topicBuzzer) && (!strcmp(text,"ON") || !strcmp(text,"OFF"))) {
    cmd.type = t == topicLed ? CMD_LED : CMD_BUZZER; cmd.on = !strcmp(text,"ON");
  } else if (t == topicReset && !strcmp(text,"ACK")) {
    cmd.type = CMD_RESET;
  } else return;
  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE)
    Serial.println("[CMD] Queue full; command rejected");
}

void TaskDocData(void *) {
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t sequence = 0, simStart = 0, lastSerial = 0;
  bool simulated = false;
  Serial.printf("[RTOS] TaskDocData core=%d period=50ms\n", xPortGetCoreID());
  for (;;) {
    const uint32_t now = millis();
    bool request;
    if (xQueueReceive(simulationQueue, &request, 0) == pdTRUE) {
      // Khong khoi dong simulation khi MANUAL; STOP luon duoc chap nhan.
      simulated = request && snapshot().autoMode;
      simStart = now;
    }
    if (simulated && (!snapshot().autoMode || now - simStart >= 12500)) simulated = false;
    Sample s{}; s.ms = now; s.seq = ++sequence; s.simulated = simulated;
    if (simulated) {
      const uint32_t elapsed = now - simStart;
      if (elapsed < 1000) { s.az=1; s.phase=1; }
      else if (elapsed < 1500) { s.az=0.1f; s.phase=2; }
      else if (elapsed < 1800) { s.ax=2.8f; s.phase=3; }
      else { s.ax=1; s.az=0.3f; s.phase=4; }
    } else {
      sensors_event_t a, g, temp;
      if (!mpu.getEvent(&a, &g, &temp)) {
        // Khong bao du lieu cu thanh du lieu moi; task canh bao phat hien timeout.
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_MS)); continue;
      }
      s.ax=a.acceleration.x/9.80665f; s.ay=a.acceleration.y/9.80665f; s.az=a.acceleration.z/9.80665f;
    }
    s.accel=sqrtf(s.ax*s.ax+s.ay*s.ay+s.az*s.az);
    s.tilt=atan2f(sqrtf(s.ax*s.ax+s.ay*s.ay), s.az)*180.0f/PI;
    xQueueOverwrite(dataAlert, &s);
    xQueueOverwrite(dataQueue, &s);
    if (now-lastSerial >= PUBLISH_MS) {
      lastSerial=now;
      Serial.printf("[READ C%d] #%lu X=%+.2fg Y=%+.2fg Z=%+.2fg | A=%.2fg Tilt=%.1f deg | %s\n",
        xPortGetCoreID(), (unsigned long)s.seq,s.ax,s.ay,s.az,s.accel,s.tilt,simulated?"TEST":"MPU6050");
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

void TaskCanhBao(void *) {
  FallDetector detector;
  DeviceState st;
  bool manualLed=false, manualBuzzer=false, oldLed=false, oldBuzzer=false;
  uint32_t lastSample=millis();
  Serial.printf("[RTOS] TaskCanhBao core=%d\n",xPortGetCoreID());
  for (;;) {
    Command cmd;
    while (xQueueReceive(commandQueue,&cmd,0)==pdTRUE) {
      if (cmd.type==CMD_MODE) {
        st.autoMode=cmd.on;
        manualLed=false; manualBuzzer=false; // vao MANUAL voi dau ra tat
      } else if (cmd.type==CMD_LED && !st.autoMode) manualLed=cmd.on;
      else if (cmd.type==CMD_BUZZER && !st.autoMode) manualBuzzer=cmd.on;
      else if (cmd.type==CMD_RESET) detector.reset();
      Serial.printf("[CMD C%d] type=%d value=%d\n",xPortGetCoreID(),cmd.type,cmd.on);
    }
    Sample s;
    if (xQueueReceive(dataAlert,&s,pdMS_TO_TICKS(20))==pdTRUE) {
      lastSample=millis(); st.sensorOk=true; st.statusSeq=s.seq;
      const bool fired=detector.update(s.accel,s.tilt,s.ms,s.simulated);
      if (fired) {
        if (s.simulated) ++st.simEvents; else ++st.events;
        Serial.printf("[ALERT C%d] %s count=%lu\n",xPortGetCoreID(),s.simulated?"SIMULATED":"SENSOR",(unsigned long)(s.simulated?st.simEvents:st.events));
      }
    }
    if (millis()-lastSample>1000) st.sensorOk=false;
    st.alarm=detector.alarm; st.checking=detector.checking;
    st.led=st.autoMode?st.alarm:manualLed;
    st.buzzer=st.autoMode?st.alarm:manualBuzzer;
    if (st.led!=oldLed) { digitalWrite(PIN_LED,st.led?HIGH:LOW); oldLed=st.led; }
    if (st.buzzer!=oldBuzzer) {
      if (st.buzzer) tone(PIN_BUZZER,1000); else noTone(PIN_BUZZER);
      oldBuzzer=st.buzzer;
    }
    xSemaphoreTake(stateMutex,portMAX_DELAY);
    sharedState=st;
    xSemaphoreGive(stateMutex);
  }
}

void TaskTruyenData(void *) {
  Serial.printf("[RTOS] TaskTruyenData core=%d\n",xPortGetCoreID());
  WiFi.mode(WIFI_STA); WiFi.begin("Wokwi-GUEST","",6);
  mqtt.setServer(MQTT_HOST,1883); mqtt.setCallback(onMessage);
  mqtt.setBufferSize(1024); mqtt.setSocketTimeout(2); mqtt.setKeepAlive(20);
  const String id="fall_rtos_"+String((uint32_t)ESP.getEfuseMac(),HEX);
  uint32_t lastWifi=millis(),lastTry=0,lastPub=0;
  for (;;) {
    const uint32_t now=millis();
    if (WiFi.status()!=WL_CONNECTED) {
      if (mqtt.connected()) mqtt.disconnect();
      if (now-lastWifi>=10000) { lastWifi=now; WiFi.reconnect(); Serial.println("[WIFI] Reconnect"); }
    } else {
      if (!mqtt.connected() && (lastTry==0 || now-lastTry>=3000)) {
        lastTry=now;
        if (mqtt.connect(id.c_str())) {
          mqtt.subscribe(topicMode.c_str()); mqtt.subscribe(topicLed.c_str());
          mqtt.subscribe(topicBuzzer.c_str()); mqtt.subscribe(topicSim.c_str()); mqtt.subscribe(topicReset.c_str());
          Serial.println("[MQTT C0] Connected / subscribed");
        } else Serial.printf("[MQTT] Retry, state=%d\n",mqtt.state());
      }
      if (mqtt.connected()) {
        mqtt.loop();
        if (now-lastPub>=PUBLISH_MS) {
          Sample s;
          // Chi lay du lieu sau khi ket noi thanh cong, dung yeu cau de bai.
          if (xQueueReceive(dataQueue,&s,0)==pdTRUE) {
            const DeviceState st=snapshot();
            char json[768];
            const char* phases[]={"IDLE","NORMAL","FREEFALL","IMPACT","TILT"};
            const int n=snprintf(json,sizeof(json),
              "{\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"accel\":%.3f,\"tilt\":%.1f,"
              "\"fall\":%s,\"motion\":\"%s\",\"events\":%lu,\"simEvents\":%lu,"
              "\"led\":%s,\"buzzer\":%s,\"mode\":\"%s\",\"seconds\":%lu,"
              "\"wifi\":true,\"sensorOk\":%s,\"simulated\":%s,\"simPhase\":\"%s\","
              "\"seq\":%lu,\"statusSeq\":%lu,\"sampleMs\":%lu,\"rtos\":true,\"readCore\":1,\"alertCore\":1,\"txCore\":0}",
              s.ax,s.ay,s.az,s.accel,s.tilt,st.alarm?"true":"false",st.alarm?"FALL":st.checking?"CHECKING":"NORMAL",
              (unsigned long)st.events,(unsigned long)st.simEvents,st.led?"true":"false",st.buzzer?"true":"false",
              st.autoMode?"AUTO":"MANUAL",(unsigned long)(now/1000),st.sensorOk?"true":"false",s.simulated?"true":"false",phases[s.phase],
              (unsigned long)s.seq,(unsigned long)st.statusSeq,(unsigned long)s.ms);
            if (n>0 && n<(int)sizeof(json) && mqtt.publish(topicTelemetry.c_str(),json,false)) lastPub=now;
            else Serial.println("[MQTT] Publish failed");
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED,OUTPUT); pinMode(PIN_BUZZER,OUTPUT);
  digitalWrite(PIN_LED,LOW); noTone(PIN_BUZZER);
  Wire.begin(21,22); Wire.setTimeOut(50);
  if (!mpu.begin(0x68,&Wire)) {
    Serial.println("[ERROR] MPU6050 missing: SDA21 SCL22 3V3 GND");
    while(true) delay(1000);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  dataQueue=xQueueCreate(1,sizeof(Sample)); dataAlert=xQueueCreate(1,sizeof(Sample));
  commandQueue=xQueueCreate(16,sizeof(Command)); simulationQueue=xQueueCreate(1,sizeof(bool));
  stateMutex=xSemaphoreCreateMutex();
  configASSERT(dataQueue && dataAlert && commandQueue && simulationQueue && stateMutex);
  topicTelemetry=String(MQTT_BASE)+"/telemetry";
  topicMode=String(MQTT_BASE)+"/cmd/mode"; topicLed=String(MQTT_BASE)+"/cmd/led";
  topicBuzzer=String(MQTT_BASE)+"/cmd/buzzer"; topicSim=String(MQTT_BASE)+"/cmd/simulate";
  topicReset=String(MQTT_BASE)+"/cmd/reset";
  configASSERT(xTaskCreatePinnedToCore(TaskCanhBao,"TaskCanhBao",4096,nullptr,3,nullptr,1)==pdPASS);
  configASSERT(xTaskCreatePinnedToCore(TaskDocData,"TaskDocData",4096,nullptr,2,nullptr,1)==pdPASS);
  configASSERT(xTaskCreatePinnedToCore(TaskTruyenData,"TaskTruyenData",6144,nullptr,1,nullptr,0)==pdPASS);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
