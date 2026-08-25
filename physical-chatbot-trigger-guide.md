# Physical Chatbot Trigger — Full Build Guide

**Stack:** ESP32 → AWS IoT Core (MQTT/TLS) → IoT Rule → Lambda → DynamoDB + Slack/Discord webhook
**Bonus path:** API Gateway + Lambda → DynamoDB (read API for a history/dashboard)

This uses IoT Core for the actual button event (that's what it's built for — persistent, secure, low-power MQTT), and API Gateway for a REST endpoint that lets you *query* past button-press history (that's what API Gateway is built for — HTTP request/response). Using both this way is defensible in a report, not just "technology soup."

---

## Architecture

```
 [Push Button] --GPIO--> [ESP32]
                            |
                     MQTT over TLS (mutual auth cert)
                            |
                            v
                    [AWS IoT Core]  (topic: button/press)
                            |
                     IoT Rule (SQL: SELECT * FROM 'button/press')
                            |
                            v
                  [Lambda: IngestButtonPress]
                    /                    \
                   v                      v
          [DynamoDB: ButtonEvents]   [Slack/Discord Webhook]

 Separately, for a dashboard:

 [Browser/Postman] --HTTPS GET--> [API Gateway] --> [Lambda: GetHistory] --> [DynamoDB]
```

---

## Part 1 — AWS IoT Core Setup

### 1.1 Create the "Thing"
AWS Console → **IoT Core** → **Manage → All devices → Things → Create things** → "Create single thing"
- Name: `physical-trigger-button-01`
- Skip thing type/groups (not needed for a course project)

### 1.2 Create and download the certificate
On the same flow, choose **Auto-generate a new certificate**, then **download all four files**:
- Device certificate (`xxxxx-certificate.pem.crt`)
- Private key (`xxxxx-private.pem.key`)
- Public key (you won't need this one on the device)
- Amazon Root CA 1 (`AmazonRootCA1.pem`) — download this from the link on the same page

Keep these safe. You'll paste the cert + private key + root CA into your ESP32 firmware.

**Activate the certificate** (it's created inactive by default) — click it in IoT Core → Certificates → Actions → Activate.

### 1.3 Create and attach an IoT policy
IoT Core → **Security → Policies → Create policy**. Name it `ButtonPublishPolicy`. Use the **JSON** editor:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:REGION:ACCOUNT_ID:client/physical-trigger-button-01"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:REGION:ACCOUNT_ID:topic/button/press"
    }
  ]
}
```

Replace `REGION` and `ACCOUNT_ID`. This is intentionally scoped — the device can only connect as itself and publish to one topic. That's the kind of least-privilege detail worth calling out in your report.

Attach this policy to the certificate you created in 1.2 (IoT Core → Certificates → your cert → Actions → Attach policy).

### 1.4 Note your IoT endpoint
IoT Core → **Settings** → copy the **Device data endpoint** (looks like `a1b2c3d4e5-ats.iot.ap-south-1.amazonaws.com`). You'll hardcode this into the ESP32 firmware.

---

## Part 2 — DynamoDB Table

Go to **DynamoDB** → **Create table**

- Table name: `ButtonEvents`
- Partition key: `deviceId` (String)
- Sort key: `timestamp` (Number) — use epoch millis so it sorts naturally
- Everything else default (on-demand capacity mode is fine and free-tier friendly)

---

## Part 3 — Slack/Discord Webhook

**Slack:**
1. https://api.slack.com/apps → Create New App → From scratch
2. Enable **Incoming Webhooks** → Add New Webhook to Workspace → pick a channel
3. Copy the webhook URL (`https://hooks.slack.com/services/...`)

**Discord (alternative/addition):**
1. Channel settings → Integrations → Webhooks → New Webhook
2. Copy Webhook URL (`https://discord.com/api/webhooks/...`)

Store this URL as a **Lambda environment variable**, not hardcoded in code — that's a small detail that reads well in a course report as "secrets management."

---

## Part 4 — Lambda #1: IngestButtonPress

Lambda console → **Create function** → Author from scratch
- Name: `IngestButtonPress`
- Runtime: Python 3.12

**Execution role permissions** (attach to the auto-created role, IAM → the role → add inline policy):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "dynamodb:PutItem",
      "Resource": "arn:aws:dynamodb:REGION:ACCOUNT_ID:table/ButtonEvents"
    }
  ]
}
```
(The `AWSLambdaBasicExecutionRole` managed policy should already be attached for CloudWatch logging.)

**Environment variables:**
- `SLACK_WEBHOOK_URL` = your Slack URL
- `DISCORD_WEBHOOK_URL` = your Discord URL (optional, leave blank if not using)
- `TABLE_NAME` = `ButtonEvents`

**Code (`lambda_function.py`):**

```python
import json
import os
import time
import urllib.request
import boto3

dynamodb = boto3.resource("dynamodb")
table = dynamodb.Table(os.environ["TABLE_NAME"])

SLACK_WEBHOOK_URL = os.environ.get("SLACK_WEBHOOK_URL", "")
DISCORD_WEBHOOK_URL = os.environ.get("DISCORD_WEBHOOK_URL", "")


def post_webhook(url, payload):
    if not url:
        return
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        urllib.request.urlopen(req, timeout=5)
    except Exception as e:
        print(f"Webhook post failed for {url}: {e}")


def lambda_handler(event, context):
    # event is the raw MQTT payload published by the ESP32, e.g.
    # {"deviceId": "physical-trigger-button-01", "pressCount": 42}
    device_id = event.get("deviceId", "unknown-device")
    press_count = event.get("pressCount", 0)
    ts = int(time.time() * 1000)

    table.put_item(
        Item={
            "deviceId": device_id,
            "timestamp": ts,
            "pressCount": press_count,
        }
    )

    message = f"Button pressed on {device_id} (press #{press_count})"

    post_webhook(SLACK_WEBHOOK_URL, {"text": message})
    post_webhook(DISCORD_WEBHOOK_URL, {"content": message})

    return {"statusCode": 200, "body": "ok"}
```

Deploy. `boto3` and `urllib` are both already available in the Lambda Python runtime, so no extra layers/packages needed — one less moving part for you to debug.

---

## Part 5 — IoT Rule (the bridge from IoT Core to Lambda)

IoT Core → **Message Routing → Rules → Create rule**
- Rule name: `ButtonPressRule`
- SQL statement:
  ```sql
  SELECT * FROM 'button/press'
  ```
- Rule action: **Lambda** → select `IngestButtonPress`

This grants IoT Core permission to invoke the Lambda automatically. Confirm it under Lambda → `IngestButtonPress` → Configuration → Triggers — you should see the IoT rule listed.

---

## Part 6 — API Gateway + Lambda #2: History Dashboard (bonus/optional but uses API Gateway meaningfully)

### 6.1 Lambda function `GetButtonHistory`

Same runtime, same table permission but `dynamodb:Query` instead of `PutItem`:

```json
{
  "Effect": "Allow",
  "Action": "dynamodb:Query",
  "Resource": "arn:aws:dynamodb:REGION:ACCOUNT_ID:table/ButtonEvents"
}
```

```python
import os
import json
import boto3
from boto3.dynamodb.conditions import Key

dynamodb = boto3.resource("dynamodb")
table = dynamodb.Table(os.environ["TABLE_NAME"])


def lambda_handler(event, context):
    params = event.get("queryStringParameters") or {}
    device_id = params.get("deviceId", "physical-trigger-button-01")

    response = table.query(
        KeyConditionExpression=Key("deviceId").eq(device_id),
        ScanIndexForward=False,  # most recent first
        Limit=20,
    )

    return {
        "statusCode": 200,
        "headers": {
            "Content-Type": "application/json",
            "Access-Control-Allow-Origin": "*",
        },
        "body": json.dumps(response["Items"]),
    }
```

### 6.2 Wire up API Gateway
API Gateway console → **Create API → HTTP API** (simpler/cheaper than REST API for this)
- Add route: `GET /events`
- Integration: Lambda `GetButtonHistory`
- Deploy → note the invoke URL, e.g. `https://xxxxx.execute-api.REGION.amazonaws.com/events`

Test it in a browser: `https://xxxxx.execute-api.../events?deviceId=physical-trigger-button-01`

---

## Part 7 — ESP32 Firmware

**Libraries to install (Arduino IDE → Library Manager):**
- `PubSubClient` (by Nick O'Leary)
- `ArduinoJson` (by Benoit Blanchon)
- ESP32 board support (Boards Manager → esp32 by Espressif Systems)

**Wiring:** button between GPIO 4 and GND, using `INPUT_PULLUP` (no external resistor needed).

```cpp
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ---- WiFi ----
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ---- AWS IoT ----
const char* AWS_IOT_ENDPOINT = "a1b2c3d4e5-ats.iot.ap-south-1.amazonaws.com"; // from Part 1.4
const char* MQTT_TOPIC = "button/press";
const char* THING_NAME = "physical-trigger-button-01";

const int BUTTON_PIN = 4;
const unsigned long DEBOUNCE_MS = 300;

unsigned long lastPressTime = 0;
int pressCount = 0;

// ---- Certificates (paste contents of the downloaded files here) ----
static const char AMAZON_ROOT_CA1[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE AmazonRootCA1.pem CONTENTS HERE
-----END CERTIFICATE-----
)EOF";

static const char DEVICE_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE xxxxx-certificate.pem.crt CONTENTS HERE
-----END CERTIFICATE-----
)EOF";

static const char PRIVATE_KEY[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
PASTE xxxxx-private.pem.key CONTENTS HERE
-----END RSA PRIVATE KEY-----
)EOF";

WiFiClientSecure net;
PubSubClient client(net);

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println(" connected");
}

void connectAWS() {
  net.setCACert(AMAZON_ROOT_CA1);
  net.setCertificate(DEVICE_CERT);
  net.setPrivateKey(PRIVATE_KEY);

  client.setServer(AWS_IOT_ENDPOINT, 8883);

  Serial.print("Connecting to AWS IoT Core");
  while (!client.connected()) {
    if (client.connect(THING_NAME)) {
      Serial.println(" connected");
    } else {
      Serial.print(".");
      delay(1000);
    }
  }
}

void publishButtonPress() {
  StaticJsonDocument<128> doc;
  doc["deviceId"] = THING_NAME;
  doc["pressCount"] = ++pressCount;

  char buffer[128];
  size_t n = serializeJson(doc, buffer);

  bool ok = client.publish(MQTT_TOPIC, buffer, n);
  Serial.printf("Published press #%d -> %s\n", pressCount, ok ? "OK" : "FAILED");
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  connectWiFi();
  connectAWS();
}

void loop() {
  if (!client.connected()) {
    connectAWS();
  }
  client.loop();

  // Button is active LOW because of INPUT_PULLUP
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastPressTime > DEBOUNCE_MS) {
      lastPressTime = now;
      publishButtonPress();
    }
  }
}
```

A few things worth understanding, not just copying:
- The debounce logic matters — physical buttons "bounce" and can register 5–10 presses in the space of one real press without it.
- `WiFiClientSecure` does the TLS handshake; the cert + private key are what let IoT Core verify this specific device (mutual TLS, not just a password).
- If you get stuck connecting, 90% of the time it's either a pasted-cert formatting issue (missing newline, truncated copy) or the IoT policy resource ARN not matching your actual topic/thing name exactly.

---

## Part 8 — End-to-End Test

1. Power the ESP32, check Serial Monitor for "connected" on both WiFi and AWS IoT.
2. Press the button. Serial Monitor should show `Published press #1 -> OK`.
3. Check **CloudWatch Logs** for `IngestButtonPress` to confirm the Lambda was invoked.
4. Check Slack/Discord — you should see the message.
5. Check DynamoDB → `ButtonEvents` table → Explore items — a new row should exist.
6. Hit the API Gateway URL from Part 6 — you should see that same row back as JSON.

If step 2 fails but step 1 works: check the IoT policy ARNs again — this is the single most common failure point.
If step 3 never fires: check the IoT Rule's SQL statement matches your exact topic name.

---

## Part 9 — Cost & Cleanup

Everything here fits comfortably in the AWS Free Tier (IoT Core: 2,500 free msgs/month for 12 months, Lambda: 1M free requests/month always, DynamoDB: 25GB + generous throughput always free, API Gateway: 1M free calls/month for 12 months). For a course project you'll spend effectively $0.

When you're done and graded, delete: the IoT Thing + certificate, the DynamoDB table, both Lambda functions, the IoT Rule, and the API Gateway API — otherwise stray resources (rare, but IoT Core message overage or a runaway Lambda loop) can incur small charges.

---

## Part 10 — Ways to Extend This for Extra Credit

- **LED feedback**: light an LED on the ESP32 when the Lambda confirms success (would need a return path — e.g. subscribe to a `button/ack` topic).
- **Multiple buttons/devices**: reuse the same pipeline with different `deviceId` values; DynamoDB partition key already supports this.
- **Rate limiting**: add a check in Lambda to ignore presses within N seconds of the last one, to prevent spam if someone holds the button.
- **Simple web dashboard**: a static HTML/JS page (host on S3) that calls the API Gateway endpoint from Part 6 and renders press history as a table/chart — this alone makes for a good final demo screen.
- **IAM tightening**: add a resource-based condition so the Lambda role can only act on specific DynamoDB item patterns, and write up the least-privilege reasoning in your report — professors like seeing this called out explicitly.

---

## Report-Writing Notes

For your course write-up, the parts worth explaining *why*, not just *what*, are: why MQTT/IoT Core over plain HTTP for the device (persistent connection, lower overhead, built for constrained devices), why the IoT policy is scoped to one topic and one thing (least privilege), and why API Gateway is separate from the ingest path (read vs. write concerns, different scaling/access patterns). Markers tend to reward that kind of reasoning more than the code itself.
