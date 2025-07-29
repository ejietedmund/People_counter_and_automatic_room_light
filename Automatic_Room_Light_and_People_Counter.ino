#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoBLE.h>

// LCD Configuration (0x27 or 0x3F)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// BLE Setup
BLEService uartService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
BLECharacteristic txChar("6E400003-B5A3-F393-E0A9-E50E24DCCA9E", BLENotify, 20);
BLECharacteristic rxChar("6E400002-B5A3-F393-E0A9-E50E24DCCA9E", BLEWrite, 20);

// Hardware Pins
const int LED_PIN = 2;          // External LED on D2
const int ENTRY_SENSOR = A0;    // Entrance sensor
const int EXIT_SENSOR = A1;     // Exit sensor

// Sensor Configuration
#define MAX_RANG 520            // Max range in cm
#define DETECTION_THRESHOLD 40  // Detection range (cm)
#define TIMEOUT_MS 3000         // Max time between triggers

// People Counting
int in_count = 0;
int out_count = 0;
int current_count = 0;
unsigned long lastTriggerTime = 0;

// LED Control
bool manualControl = false;
bool ledState = false;

// Display Management
unsigned long lastDisplayUpdate = 0;
const int DISPLAY_UPDATE_INTERVAL = 200; // ms
void updateDisplay(bool forceUpdate = false) {
  static int last_in = -1, last_out = -1;
  static bool last_led = false;
  
  bool led_actual = digitalRead(LED_PIN) == HIGH;
  current_count = in_count - out_count;

  // Only update if something changed or forced
  if (forceUpdate || (millis() - lastDisplayUpdate > DISPLAY_UPDATE_INTERVAL) ||
      (in_count != last_in) || (out_count != last_out) || (led_actual != last_led)) {
    
    lastDisplayUpdate = millis();
    last_in = in_count;
    last_out = out_count;
    last_led = led_actual;
    ledState = led_actual; // Sync variable

    lcd.clear();
    
    // Line 1: Counts
    lcd.setCursor(0, 0);
    lcd.print("IN:");
    lcd.print(in_count);
    lcd.print(" OUT:");
    lcd.print(out_count);
    
    // Line 2: Current count + LED status
    lcd.setCursor(0, 1);
    lcd.print("NOW:");
    lcd.print(current_count >= 0 ? " " : "");
    lcd.print(current_count);
    
    lcd.setCursor(10, 1);
    lcd.print("LED:");
    lcd.print(led_actual ? "ON " : "OFF");
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  // Initialize hardware
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(ENTRY_SENSOR, INPUT);
  pinMode(EXIT_SENSOR, INPUT);

  // Initialize BLE
  if (!BLE.begin()) {
    Serial.println("BLE initialization failed!");
    lcd.print("BLE Init Failed!");
    while(1);
  }

  BLE.setLocalName("PeopleCounter");
  BLE.setAdvertisedService(uartService);
  uartService.addCharacteristic(txChar);
  uartService.addCharacteristic(rxChar);
  BLE.addService(uartService);
  
  rxChar.setEventHandler(BLEWritten, onRxCharWritten);
  BLE.advertise();
  
  updateDisplay(true); // Force initial update
  Serial.println("System Ready");
}

void onRxCharWritten(BLEDevice central, BLECharacteristic characteristic) {
  String command = readBLECommand();
  command.trim();

  if (command == "MANUAL") {
    manualControl = true;
    sendBLEMessage("MODE: MANUAL\n");
  } 
  else if (command == "AUTO") {
    manualControl = false;
    sendBLEMessage("MODE: AUTO\n");
  }
  else if (manualControl) {
    if (command == "LED_ON") {
      digitalWrite(LED_PIN, HIGH);
      sendBLEMessage("LED: ON\n");
    } 
    else if (command == "LED_OFF") {
      digitalWrite(LED_PIN, LOW);
      sendBLEMessage("LED: OFF\n");
    }
  }
  updateDisplay(true); // Force update after command
}

String readBLECommand() {
  const uint8_t* data = rxChar.value();
  size_t length = rxChar.valueLength();
  String command = "";
  for (size_t i = 0; i < length; i++) {
    command += (char)data[i];
  }
  return command;
}

void sendBLEMessage(String message) {
  txChar.writeValue(message.c_str());
  Serial.print("BLE >> "); Serial.print(message);
}

float readDistance(int pin) {
  float sensity_t = analogRead(pin);
  float dist_cm = sensity_t * MAX_RANG / 1023.0;
  return (dist_cm >= 2 && dist_cm <= MAX_RANG) ? dist_cm : -1;
}



void loop() {
  // Handle BLE connections
  BLEDevice central = BLE.central();
  if (central) {
    while (central.connected()) {
      handlePeopleCounting();
      delay(50);
    }
    Serial.println("Disconnected");
  } else {
    handlePeopleCounting();
  }
}

void handlePeopleCounting() {
  float dist1 = readDistance(ENTRY_SENSOR);
  float dist2 = readDistance(EXIT_SENSOR);

  // Entrance detection (Sensor1 -> Sensor2)
  if (dist1 > 0 && dist1 <= DETECTION_THRESHOLD && millis() - lastTriggerTime > TIMEOUT_MS) {
    unsigned long startTime = millis();
    while (millis() - startTime < TIMEOUT_MS) {
      if (readDistance(EXIT_SENSOR) > 0 && readDistance(EXIT_SENSOR) <= DETECTION_THRESHOLD) {
        in_count++;
        lastTriggerTime = millis();
        updateCounts();
        Serial.println("Person ENTERED");
        break;
      }
      delay(50);
    }
    waitForSensorsClear();
  }
  
  // Exit detection (Sensor2 -> Sensor1)
  else if (dist2 > 0 && dist2 <= DETECTION_THRESHOLD && millis() - lastTriggerTime > TIMEOUT_MS) {
    unsigned long startTime = millis();
    while (millis() - startTime < TIMEOUT_MS) {
      if (readDistance(ENTRY_SENSOR) > 0 && readDistance(ENTRY_SENSOR) <= DETECTION_THRESHOLD) {
        if (out_count < in_count) {
          out_count++;
          lastTriggerTime = millis();
          updateCounts();
          Serial.println("Person EXITED");
        }
        break;
      }
      delay(50);
    }
    waitForSensorsClear();
  }

  // Auto LED control with display sync
  if (!manualControl) {
    bool shouldBeOn = (current_count > 0);
    if (shouldBeOn != digitalRead(LED_PIN)) {
      digitalWrite(LED_PIN, shouldBeOn ? HIGH : LOW);
      updateDisplay(true); // Force immediate update
    }
  }

  // Periodic display refresh
  updateDisplay();
  delay(50);
}

void updateCounts() {
  current_count = in_count - out_count;
  updateDisplay(true); // Force update after count change
  
  // Send BLE update
  String data = String("IN:") + in_count + 
               " OUT:" + out_count + 
               " NOW:" + current_count + "\n";
  sendBLEMessage(data);
}

void waitForSensorsClear() {
  while (readDistance(ENTRY_SENSOR) <= DETECTION_THRESHOLD || 
         readDistance(EXIT_SENSOR) <= DETECTION_THRESHOLD) {
    delay(50);
  }
}