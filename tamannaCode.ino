

#include <WiFiS3.h>
#include <DHT.h>

/* ---------------- PIN DEFINITIONS ---------------- */
#define MIC_PIN A0
#define DHT_PIN 3
#define BUZZER_PIN 8
#define DHT_TYPE DHT11

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient client;

/* ---------------- WIFI & CLOUD ---------------- */
const char* ssid = "Lobley";
const char* password = "12345678";
const char* server = "api.thingspeak.com";
const char* apiKey = "0204GER5UFVPCWP0";  
/* ---------------- THRESHOLDS ---------------- */
const float TEMP_HIGH = 24.0;
const float TEMP_LOW  = 18.0;
const float HUM_HIGH  = 70.0;
const float HUM_LOW   = 30.0;

/* ---------------- SOUND DETECTION ---------------- */
const int SOUND_THRESHOLD = 10;
const int SAMPLE_WINDOW = 100;
const int BUFFER_SIZE = 12;
const unsigned long CRY_COOLDOWN = 8000;
const int CRY_CONFIDENCE = 7;

/* ---------------- STATE ---------------- */
int soundBuffer[BUFFER_SIZE];
int bufIndex = 0;
bool cryDetected = false;
bool buzzerActive = false;
unsigned long lastCryTime = 0;
unsigned long lastUpload = 0;
unsigned long lastDHTRead = 0;

float lastTemp = 23.0;
float lastHum = 50.0;
int dhtFailCount = 0;

enum AlertLevel { NORMAL, WARNING, CRITICAL };
AlertLevel currentAlert = NORMAL;

/* ---------------- SETUP ---------------- */
void setup() {
  Serial.begin(115200);
  while (!Serial);
  delay(500);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();
  delay(2000);

  for (int i = 0; i < BUFFER_SIZE; i++) {
    soundBuffer[i] = 0;
  }

  Serial.println("========================================");
  Serial.println("  Baby Monitor System v2.0");
  Serial.println("========================================");
  
  connectWiFi();
  
  Serial.println("\nSound Detection Levels:");
  Serial.println("  0-5   = Silence/Background");
  Serial.println("  5-9  = Talking/Movement");
  Serial.println("  10-15 = Baby fussing/Crying");
  Serial.println("  15+   = Loud crying/Distress");
  Serial.println("========================================\n");
}

/* ---------------- MAIN LOOP ---------------- */
void loop() {
  // Read sound continuously
  int sound = readSoundLevel();
  bool crying = detectCry(sound);

  // Read DHT only every 2 seconds
  if (millis() - lastDHTRead >= 2000 && !buzzerActive) {
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    
    if (!isnan(temp) && !isnan(hum)) {
      lastTemp = temp;
      lastHum = hum;
      dhtFailCount = 0;
    } else {
      dhtFailCount++;
      if (dhtFailCount > 5) {
        Serial.println("WARNING: DHT sensor unstable");
        dhtFailCount = 0;
      }
    }
    lastDHTRead = millis();
  }

  // Display status
  Serial.print("Sound: ");
  Serial.print(sound);
  Serial.print(" | Temp: ");
  Serial.print(lastTemp, 1);
  Serial.print("C | Hum: ");
  Serial.print(lastHum, 1);
  Serial.print("% | Cry: ");
  Serial.println(crying ? "YES ⚠️" : "NO");

  // Evaluate and handle alerts
  currentAlert = evaluate(lastTemp, lastHum, crying);
  handleAlert(currentAlert, crying);

  // Upload to cloud every 20 seconds
  if (millis() - lastUpload > 20000) {
    uploadToThingSpeak(lastTemp, lastHum, sound, crying);
    lastUpload = millis();
  }

  delay(500);
}

/* ---------------- WIFI ---------------- */
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int tries = 0;
  
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    Serial.print(".");
    delay(500);
    tries++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi Connected!");
    Serial.print("  IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n✗ WiFi Failed - Will retry");
  }
}

/* ---------------- SOUND DETECTION ---------------- */
int readSoundLevel() {
  if (buzzerActive) return 0;

  unsigned long start = millis();
  int maxV = 0, minV = 1023;

  while (millis() - start < SAMPLE_WINDOW) {
    int sample = analogRead(MIC_PIN);
    maxV = max(maxV, sample);
    minV = min(minV, sample);
    delayMicroseconds(100);
  }
  
  return maxV - minV;
}

/* ---------------- CRY DETECTION ---------------- */
bool detectCry(int sound) {
  soundBuffer[bufIndex] = sound;
  bufIndex = (bufIndex + 1) % BUFFER_SIZE;

  int sum = 0;
  int highCount = 0;
  int veryHighCount = 0;
  
  for (int i = 0; i < BUFFER_SIZE; i++) {
    sum += soundBuffer[i];
    if (soundBuffer[i] > SOUND_THRESHOLD) highCount++;
    if (soundBuffer[i] > SOUND_THRESHOLD * 2) veryHighCount++;
  }

  int avg = sum / BUFFER_SIZE;
  unsigned long now = millis();

  bool loudSustained = (avg > SOUND_THRESHOLD && highCount >= CRY_CONFIDENCE);
  bool veryLoud = (veryHighCount >= 3);

  if ((loudSustained || veryLoud) && !cryDetected) {
    if (now - lastCryTime > CRY_COOLDOWN) {
      cryDetected = true;
      lastCryTime = now;
      Serial.println("\n*** CRY DETECTED BY EDGE AI ***");
      Serial.print("    Avg: "); Serial.print(avg);
      Serial.print(" | High readings: "); Serial.print(highCount);
      Serial.print("/"); Serial.println(BUFFER_SIZE);
    }
  } 
  else if (avg < SOUND_THRESHOLD * 0.4 && highCount < 3) {
    cryDetected = false;
  }

  return cryDetected;
}

/* ---------------- ALERT EVALUATION ---------------- */
AlertLevel evaluate(float t, float h, bool cry) {
  if (cry) return CRITICAL;
  if (t > TEMP_HIGH || t < TEMP_LOW) return CRITICAL;
  if (h > HUM_HIGH || h < HUM_LOW) return WARNING;
  return NORMAL;
}

void handleAlert(AlertLevel level, bool cry) {
  if (level == CRITICAL) {
    if (cry) {
      soundAlarm(3, 100);
    } else {
      soundAlarm(2, 200);
    }
  } else if (level == WARNING) {
    soundAlarm(1, 150);
  }
}

/* ---------------- BUZZER ---------------- */
void soundAlarm(int beeps, int duration) {
  buzzerActive = true;
  
  for (int i = 0; i < beeps; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(duration);
    digitalWrite(BUZZER_PIN, LOW);
    
    if (i < beeps - 1) {
      delay(120);
    }
  }
  
  buzzerActive = false;
}

/* ---------------- THINGSPEAK UPLOAD ---------------- */
void uploadToThingSpeak(float t, float h, int s, bool cry) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi disconnected - reconnecting...");
    connectWiFi();
    return;
  }

  String url = "/update?api_key=";
  url += apiKey;
  url += "&field1=";
  url += String(t, 2);
  url += "&field2=";
  url += String(h, 2);
  url += "&field3=";
  url += String(s);
  url += "&field4=";
  url += (cry ? "1" : "0");
 

  if (client.connect(server, 80)) {
    client.print("GET ");
    client.print(url);
    client.println(" HTTP/1.1");
    client.print("Host: ");
    client.println(server);
    client.println("Connection: close");
    client.println();
    
    delay(500);
    
    while (client.available()) {
      String line = client.readStringUntil('\r');
      if (line.indexOf("200 OK") > 0) {
        Serial.println("✓ Data uploaded successfully!");
      }
    }
    
    client.stop();
  } else {
    Serial.println("✗ ThingSpeak connection failed");
  }
}