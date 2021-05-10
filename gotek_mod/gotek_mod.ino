#include <HardwareSerial.h>
#include "WiFi.h"
#include <esp_now.h>
#include "driver/rtc_io.h"
#include <EEPROM.h>

#define SERIAL_UART_BAUD   57600  

#define RX_DATA_START -9    
#define OLED_STATUS_REQUEST -6
#define OLED_STATUS_UNKNOWN -8
#define OLED_ON -7    
#define OLED_OFF -5
#define SWITCH_ROTATION_CCW -3
#define SWITCH_ROTATION_CW -4
#define CONNECTION_TEST -2
#define CTRL_PROBE_SUCCESS 0
#define CTRL_PROBE_UNKNOWN -1
#define CTRL_PROBE_FAIL 1

#define LED_PIN GPIO_NUM_2
#define ROTARY_ENC_OUT_A  GPIO_NUM_33 
#define ROTARY_ENC_OUT_B  GPIO_NUM_17 
#define FLOPPY_LED_PLUS   GPIO_NUM_35
#define FLOPPY_LED_MINUS    GPIO_NUM_34
#define FLOPPY_READ_THRESHOLD 4000      
#define OLED_TIMEOUT_MSEC 21000     
#define IMAGE_DATA_NUM 1024

#define ROTARY_OUTPUTS_DELAY 10
#define TIMEOUT_TIMER_ID 0

//gotek_ctrl mac is CC:50:E3:B5:9E:38
//gotek_mod mac is CC:50:E3:B5:A1:1C
//uint8_t gotekCtrlAddress[] = {0xCC, 0x50, 0xE3, 0xB5, 0x9E, 0x38};
uint8_t gotekCtrlAddress[6];
hw_timer_t * timeOutTimer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
bool aState = LOW, bState = LOW, oledIsOn = true, prevOledIsOn = false,  floppyBusy = false, receivingInitDataFromCtrl = false;
uint32_t oledTimeOutMsec = OLED_TIMEOUT_MSEC, countDownTimerMsec = OLED_TIMEOUT_MSEC;
int16_t imageIdx = -1, totalAvailableImages = 0;
int8_t ctrlProbeResult = CTRL_PROBE_UNKNOWN, macRxIdx = 0;
const int imageIdx_eepromAdress = 0;

void saveInt16ToEEPROM (int eepromAddress,int16_t value) {  
  EEPROM.put(eepromAddress, value);    
  EEPROM.commit();
}

int16_t loadInt16FromEEPROM(int eepromAddress) {
  int16_t readData;
  EEPROM.get(eepromAddress, readData);
  return readData;
}

void clearEEPROM() {
  Serial.print("clear EEPROM ");
  for (int i = 0; i < 512; i++) {
    EEPROM.write(i, -1);
  }
  EEPROM.commit();
  delay(500);
  Serial.println("done!");
}

// callback when data is sent
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  char macStr[18];
  Serial.print("Packet to: ");
  // Copies the sender mac address to a string
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  Serial.print(macStr);
  Serial.print(" send status:\t");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
  (status == ESP_NOW_SEND_SUCCESS) ? ctrlProbeResult = CTRL_PROBE_SUCCESS : ctrlProbeResult = CTRL_PROBE_FAIL;  
}

//callback function that will be executed when data is received
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  int16_t dataReceived = SWITCH_ROTATION_CCW;
  memcpy(&dataReceived, incomingData, sizeof(int16_t));
  Serial.print("Bytes received: ");
  Serial.println(len);  

  if (receivingInitDataFromCtrl) {
    if ((macRxIdx >= 0) && (macRxIdx <=5)) {
      Serial.println("dataReceived = 0x" + String(dataReceived,HEX));
      gotekCtrlAddress[macRxIdx] = (uint8_t)dataReceived;
    }
    else if (macRxIdx == 6) {
      Serial.println("dataReceived = " + String(dataReceived));
      totalAvailableImages = dataReceived;
      receivingInitDataFromCtrl = false;       
    }    
    macRxIdx++;
  }
  else {      
    Serial.println("dataReceived = " + String(dataReceived));
    if (dataReceived != CONNECTION_TEST) { //ignore connection test message
      if (dataReceived < 0) {
        if (dataReceived == (int16_t)OLED_STATUS_REQUEST) {
          int16_t tmpOledStatus = (oledIsOn) ? OLED_ON : OLED_OFF;
          sendDataToCtrl(tmpOledStatus);
        }
        else if (dataReceived == (int16_t)RX_DATA_START) {
          receivingInitDataFromCtrl = true;
        }
        else {
          setRotaryEncOutputsGotekStates(dataReceived == SWITCH_ROTATION_CW);   
        }
      }
      else {
        imageIdx = dataReceived;
        Serial.println("imageIdx = " + String(imageIdx));
      }    
    }
  }
}

void sendDataToCtrl(int16_t dataToSend) {
  esp_err_t result = esp_now_send(0, (uint8_t *) &dataToSend, sizeof(int16_t));  
}

void IRAM_ATTR timerExpired() {
  portENTER_CRITICAL_ISR(&timerMux);  
  oledIsOn = false;
  portEXIT_CRITICAL_ISR(&timerMux); 
}

void stopTimeoutTimer() {
  Serial.println("stopTimeoutTimer..");  
  timerEnd(timeOutTimer);
  timeOutTimer = NULL;
}

void startTimeoutTimer() {    
  if (timeOutTimer != NULL) {
    stopTimeoutTimer();
  }
  Serial.println("startTimeoutTimer..");  
  timeOutTimer = timerBegin(TIMEOUT_TIMER_ID, 80, true);
  timerAttachInterrupt(timeOutTimer, &timerExpired, true);
  timerAlarmWrite(timeOutTimer, 1000 * OLED_TIMEOUT_MSEC, true);
  timerAlarmEnable(timeOutTimer);  
}

void setRotaryEncOutputA(bool newState) {
  digitalWrite(ROTARY_ENC_OUT_A, newState);
  aState = newState;
}

void setRotaryEncOutputB(bool newState) {
  digitalWrite(ROTARY_ENC_OUT_B, newState);
  bState = newState;
}

void setRotaryEncOutputsGotekStates(bool cw) {
  Serial.println("setRotaryEncOutputsGotekStates, cw = " + String(cw));  
  if (cw) {
    //CW: 10 > 00 > 01 > 11 > 10
    setRotaryEncOutputA(HIGH); setRotaryEncOutputB(LOW);
    Serial.print(String(aState) + String(bState) + " -> ");     
    delay(ROTARY_OUTPUTS_DELAY); 
    setRotaryEncOutputA(LOW); setRotaryEncOutputB(LOW);
    Serial.print(String(aState) + String(bState) + " -> ");  
    delay(ROTARY_OUTPUTS_DELAY);
    setRotaryEncOutputA(LOW); setRotaryEncOutputB(HIGH);
    Serial.print(String(aState) + String(bState) + " -> ");    
    delay(ROTARY_OUTPUTS_DELAY);
    setRotaryEncOutputA(HIGH); setRotaryEncOutputB(HIGH);
    Serial.print(String(aState) + String(bState) + " -> ");   
    delay(ROTARY_OUTPUTS_DELAY);
    setRotaryEncOutputA(HIGH); setRotaryEncOutputB(LOW);
    Serial.println(String(aState) + String(bState));      
    delay(ROTARY_OUTPUTS_DELAY);
  }
  else {
    //CCW: 00 > 10 > 11 > 01 > 00
    setRotaryEncOutputA(LOW); setRotaryEncOutputB(LOW);
    Serial.print(String(aState) + String(bState) + " -> ");  
    delay(ROTARY_OUTPUTS_DELAY); 
    setRotaryEncOutputA(HIGH); setRotaryEncOutputB(LOW);
    Serial.print(String(aState) + String(bState) + " -> ");
    delay(ROTARY_OUTPUTS_DELAY);
    setRotaryEncOutputA(HIGH); setRotaryEncOutputB(HIGH);
    Serial.print(String(aState) + String(bState) + " -> ");
    delay(ROTARY_OUTPUTS_DELAY);
    setRotaryEncOutputA(LOW); setRotaryEncOutputB(HIGH);
    Serial.print(String(aState) + String(bState) + " -> ");
    delay(ROTARY_OUTPUTS_DELAY);
    setRotaryEncOutputA(LOW); setRotaryEncOutputB(LOW);
    Serial.println(String(aState) + String(bState));      
    delay(ROTARY_OUTPUTS_DELAY);
  }  
  updateIndex(cw);
  oledIsOn = true;
  startTimeoutTimer();
}

void floppyIsBusy() {
  uint32_t floppyLedPlusState = 0, floppyLedMinusState = 0;
  floppyLedPlusState = analogRead(FLOPPY_LED_PLUS);  
  floppyLedMinusState = analogRead(FLOPPY_LED_MINUS);   
  //Serial.println("floppyLedPlusState = " + String(floppyLedPlusState) + ", floppyLedMinusState = " + String(floppyLedMinusState)); 
  
  if ((floppyLedPlusState > 0) && (floppyLedPlusState < FLOPPY_READ_THRESHOLD)) {
    oledIsOn = true;   
    startTimeoutTimer();
  }    
}

void updateIndex(bool cw) {
  Serial.println("updateIndex, cw = " + String(cw));
  if (oledIsOn) {  //oled is currently on, so we can update the selected image index
    (cw) ? imageIdx++ : imageIdx--;

    if (imageIdx == totalAvailableImages) imageIdx = 0;
    if (imageIdx < 0) imageIdx = totalAvailableImages-1;
    
    sendDataToCtrl(imageIdx);
    saveInt16ToEEPROM(imageIdx_eepromAdress,imageIdx);
  } 
  Serial.println("imageIdx = " + String(imageIdx)); 
}

void setup() {
  Serial.begin(SERIAL_UART_BAUD);
  delay(1000);  
  Serial.println("Starting gotek mod...");
  WiFi.mode(WIFI_MODE_STA);
  Serial.println("MAC address: " + WiFi.macAddress());

  EEPROM.begin(512);  
  //clearEEPROM(); 

  //Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  else {
    Serial.println("ESP-NOW initialized!");
  }

  pinMode(ROTARY_ENC_OUT_A, OUTPUT);
  setRotaryEncOutputA(LOW);
  pinMode(ROTARY_ENC_OUT_B, OUTPUT);
  setRotaryEncOutputB(LOW);  
  pinMode(FLOPPY_LED_PLUS, INPUT);  
  pinMode(FLOPPY_LED_MINUS, INPUT); 
  pinMode(LED_PIN,OUTPUT);

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  delay(100);  
  //wait for the gotekCtrl mac address..
  Serial.println("wait for the gotekCtrl mac address..");  
  while (macRxIdx <= 6) {
    delay(1000);  
  }
  
  // register gotekCtrl
  esp_now_peer_info_t gotekCtrlInfo;
  gotekCtrlInfo.channel = 0;  
  gotekCtrlInfo.encrypt = false;
  // register first peer  
  memcpy(gotekCtrlInfo.peer_addr, gotekCtrlAddress, 6);
  if (esp_now_add_peer(&gotekCtrlInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    return;
  }
  else {
    Serial.println("Successfully added Gotek ctrl!");    
  }

  imageIdx = loadInt16FromEEPROM(imageIdx_eepromAdress);

  //send the imageIdx to the gotek ctrl
  do {
    sendDataToCtrl((int16_t)CONNECTION_TEST);
    Serial.println("ctrlProbeResult = " + String(ctrlProbeResult));
    delay(1000);  
  }
  while (ctrlProbeResult != CTRL_PROBE_SUCCESS);

  delay(1000);  
  sendDataToCtrl(imageIdx);

  oledIsOn = true;   
  startTimeoutTimer();  
}

void loop() {   
  
  floppyIsBusy();
  delay(100);  
  if (prevOledIsOn != oledIsOn) {
    Serial.println("oledIsOn = " + String(oledIsOn));    
    if (!oledIsOn) {
      sendDataToCtrl((int16_t)OLED_OFF);
    }
    digitalWrite(LED_PIN,oledIsOn);
  }
  prevOledIsOn = oledIsOn;
}
