/*
  Physical Chatbot Trigger — ESP32 firmware
  Publishes an MQTT message to AWS IoT Core when a button is pressed.

  Required library: PubSubClient (by Nick O'Leary) — install via Arduino Library Manager.
  Board package: "esp32" by Espressif Systems — install via Boards Manager.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ---------- EDIT THESE ----------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// From IoT Core -> Settings -> Device data endpoint
const char* AWS_IOT_ENDPOINT = "xxxxxxxxxxxxx-ats.iot.REGION.amazonaws.com";
const int   MQTT_PORT = 8883;

const char* MQTT_CLIENT_ID = "esp32-button-01";
const char* MQTT_TOPIC     = "buttontrigger/press";

const int   BUTTON_PIN = 4;          // wired to GND when pressed (INPUT_PULLUP)
const unsigned long DEBOUNCE_MS = 200;

// Paste certificate contents below, keeping the R"EOF( ... )EOF" wrapper.
// Amazon Root CA 1 (download from AWS IoT Core "Connect device" step or
// https://www.amazontrust.com/repository/AmazonRootCA1.pem)
static const char AWS_ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_AMAZON_ROOT_CA1_HERE
-----END CERTIFICATE-----
)EOF";

// Device certificate (xxxxx-certificate.pem.crt from IoT Core)
static const char DEVICE_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_DEVICE_CERTIFICATE_HERE
-----END CERTIFICATE-----
)EOF";

// Device private key (xxxxx-private.pem.key from IoT Core)
static const char DEVICE_PRIVATE_KEY[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
PASTE_PRIVATE_KEY_HERE
-----END RSA PRIVATE KEY-----
)EOF";
// ---------- END EDIT ----------

WiFiClientSecure netClient;
PubSubClient mqttClient(netClient);

unsigned long lastPressMs = 0;
int lastButtonState = HIGH;

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected, IP: " + WiFi.localIP().toString());
}

void connectAWSIoT() {
  netClient.setCACert(AWS_ROOT_CA);
  netClient.setCertificate(DEVICE_CERT);
  netClient.setPrivateKey(DEVICE_PRIVATE_KEY);

  mqttClient.setServer(AWS_IOT_ENDPOINT, MQTT_PORT);

  Serial.print("Connecting to AWS IoT Core");
  while (!mqttClient.connected()) {
    if (mqttClient.connect(MQTT_CLIENT_ID)) {
      Serial.println("\nMQTT connected");
    } else {
      Serial.print(".");
      delay(1000);
    }
  }
}

void publishButtonPress() {
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"deviceId\":\"%s\",\"event\":\"button_press\",\"uptime_ms\":%lu}",
           MQTT_CLIENT_ID, millis());

  bool ok = mqttClient.publish(MQTT_TOPIC, payload);
  Serial.println(ok ? "Published" : "Publish FAILED");
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  connectWiFi();
  connectAWSIoT();
}

void loop() {
  if (!mqttClient.connected()) {
    connectAWSIoT();
  }
  mqttClient.loop();

  int reading = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  // Falling edge (HIGH -> LOW) with debounce
  if (reading == LOW && lastButtonState == HIGH && (now - lastPressMs) > DEBOUNCE_MS) {
    publishButtonPress();
    lastPressMs = now;
  }
  lastButtonState = reading;
}
