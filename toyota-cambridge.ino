#include <driver/twai.h>
#include <AceButton.h>
using namespace ace_button;

#define CAN_RX GPIO_NUM_16  // SN65HVD230 TXD -> ESP32 RX
#define CAN_TX GPIO_NUM_4  // SN65HVD230 RXD -> ESP32 TX
#define TRIGGER GPIO_NUM_18
#define RELAY GPIO_NUM_19
#define BUTTON GPIO_NUM_5
#define INDICATOR GPIO_NUM_17

#define PARK    0
#define REVERSE 1
#define NEUTRAL 2
#define DRIVE   3
#define MANUAL  4

enum State {
  AUTO,
  FRONT_ON,
  FRONT_OFF,
};

const State fwdSeq1[] = { AUTO, FRONT_ON, FRONT_OFF };
const State fwdSeq2[] = { AUTO, FRONT_OFF, FRONT_ON };
const State revSeq[] = { AUTO, FRONT_ON};
const State* curSeq = fwdSeq1;
int curSeqIndex = 0;

AceButton button(BUTTON);

void handleButtonEvent(AceButton* button, uint8_t eventType, uint8_t buttonState) {
  if (eventType != AceButton::kEventPressed) {
    return;
  }

  int stateCount = curSeq == revSeq ? 2 : 3;
  curSeqIndex = (curSeqIndex + 1) % stateCount;
  State state = curSeq[curSeqIndex];

  switch (state) {
    case AUTO:
      Serial.println("State: auto");
      digitalWrite(INDICATOR, LOW);
      return;
    case FRONT_ON:
      Serial.println("State: front on");
      digitalWrite(RELAY, HIGH);
      digitalWrite(TRIGGER, HIGH);
      digitalWrite(INDICATOR, HIGH);
      return;
    case FRONT_OFF:
      Serial.println("State: front off");
      digitalWrite(TRIGGER, LOW);
      digitalWrite(INDICATOR, HIGH);
      return;
  }
}

int getLeverPosition(const twai_message_t &msg) {
  static int lastLeverPosition = PARK;

  if (msg.identifier != 0x127) {
    return lastLeverPosition;
  }
  
  if (msg.data_length_code < 6) {
    Serial.printf("Shift lever message is too short: %d bytes\n", msg.data_length_code);
    return lastLeverPosition;
  }

  int leverPosition = msg.data[5] >> 4;

  if (leverPosition == lastLeverPosition) {
    return lastLeverPosition;
  }

  switch (leverPosition) {
    case PARK:
      Serial.println("PARK");
      break;
    case REVERSE:
      Serial.println("REVERSE");
      break;
    case NEUTRAL:
      Serial.println("NEUTRAL");
      break;
    case DRIVE:
      Serial.println("DRIVE");
      break;
    case MANUAL:
      Serial.println("MANUAL");
      break;
    default:
      Serial.printf("UNKNOWN selector code: 0x%X\n", leverPosition);
      break;
  }
  
  lastLeverPosition = leverPosition;
  return leverPosition;
}

float getSpeed(const twai_message_t &msg) {
  static float lastSpeed = 0;

  if (msg.identifier != 0xAA) {
    return lastSpeed;
  }

  if (msg.data_length_code < 8) {
    Serial.printf("Wheel speed message is too short: %d bytes\n", msg.data_length_code);
    return lastSpeed;
  }

  int rawSum = 0;
  for (uint8_t i = 0; i < 4; i++) {
    int wheelSpeedRaw = (msg.data[i * 2] << 8) | msg.data[i * 2 + 1];
    rawSum += wheelSpeedRaw - 6767;
  }

  float speed = (rawSum / 4.0f) * 0.01f;

  if (speed == lastSpeed) {
    return lastSpeed;
  }

  Serial.printf("Speed: %.2f km/h\n", speed);
  lastSpeed = speed;
  return speed;
}


bool frontParktronic(const twai_message_t &msg) {
  static int lastVal = 0;

  if (msg.identifier != 0x396) {
    return lastVal != 0;
  }
    
  if (msg.data_length_code < 3) {
    Serial.printf("Parktronic message is too short: %d bytes\n", msg.data_length_code);
    return lastVal != 0;
  }

  int val = msg.data[1] << 8 | msg.data[2];
  
  if (val == lastVal) {
     return lastVal != 0;
  }

  Serial.printf("Parktronic: %x\n", val);
  lastVal = val;
  return lastVal != 0;
}

State buttonState(int position, bool isFrontCamEnabled) {
  static int lastPosition = PARK;
  if (lastPosition != position) {
    lastPosition = position;
    curSeqIndex = 0;
    digitalWrite(INDICATOR, LOW);
  }

  State state = curSeq[curSeqIndex];
  if (state == AUTO) {
    curSeq = position == REVERSE ? revSeq : isFrontCamEnabled ? fwdSeq2 :fwdSeq1;
  }
  
  return state;
}

void logic(int position, float speed, bool frontParktronic) {
  static long lastEnabledTime = 0;
  static bool isFrontCamEnabled = false;
  static int lastPosition = PARK;
  bool isSwitchedToDrive = lastPosition != DRIVE && position == DRIVE;
  lastPosition = position;

  if (buttonState(position, isFrontCamEnabled) != AUTO) {
    return;
  }

  if (position == NEUTRAL || speed > 9 && speed <= 10) {
    return;
  }

  if (position == DRIVE) {
    digitalWrite(RELAY, HIGH);
  } else {
    digitalWrite(RELAY, LOW);
  }

  if (position == REVERSE) {
    digitalWrite(TRIGGER, HIGH);
    return;
  }

  if (position != DRIVE) {
    digitalWrite(TRIGGER, LOW);
    isFrontCamEnabled = false;
    return;
  }

  long now = millis();
  if (speed > 0 || isSwitchedToDrive || frontParktronic) {
    lastEnabledTime = now;
  }

  isFrontCamEnabled = now - lastEnabledTime < 2000 && speed <= 10 || frontParktronic;
  if (isFrontCamEnabled) {
    digitalWrite(TRIGGER, HIGH);
  } else {
    digitalWrite(TRIGGER, LOW);
  }
}

void setup() {
  delay(100);
  Serial.begin(115200);
  Serial.println("Starting CAN bus...");

  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_LISTEN_ONLY);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  while (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK || twai_start() != ESP_OK) {
    Serial.println("Failed to start CAN driver");
    delay(1000);
  }

  pinMode(RELAY, OUTPUT);
  pinMode(TRIGGER, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(INDICATOR, OUTPUT);

  button.getButtonConfig()->setEventHandler(handleButtonEvent);

  Serial.println("CAN driver started successfully");
}

void loop() {
  button.check();
  twai_message_t msg;
  if (twai_receive(&msg, pdMS_TO_TICKS(100)) == ESP_OK) {
    logic(getLeverPosition(msg), getSpeed(msg), frontParktronic(msg));
  }
}
