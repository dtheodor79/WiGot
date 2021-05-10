#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <esp_now.h>
#include <WiFi.h>
#include "SD.h"
#include "SPI.h"
#include "FS.h"
#include <EEPROM.h>

#define SERIAL_UART_BAUD   57600 
#define LED_PIN       GPIO_NUM_2 
#define I2C_SDA       GPIO_NUM_21
#define I2C_SCL       GPIO_NUM_22
#define KY040_S2      GPIO_NUM_17
#define KY040_S1      GPIO_NUM_16
#define KY040_BUTTON  GPIO_NUM_26
#define INC10_BUTTON  GPIO_NUM_35  //TODO
#define DEC10_BUTTON  GPIO_NUM_33  //TODO
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels    
#define DISPLAY_I2C_ADDRESS 0x3C 

#define ROTARY_ENC_OUT_A  GPIO_NUM_19
#define ROTARY_ENC_OUT_B  GPIO_NUM_18

#define SD_MISO     GPIO_NUM_19
#define SD_MOSI     GPIO_NUM_23
#define SD_SCLK     GPIO_NUM_18
#define SD_CS       GPIO_NUM_5

#define OLED_TIMEOUT_MSEC 21000 
#define RX_DATA_START -9    
#define OLED_STATUS_REQUEST -6
#define OLED_STATUS_UNKNOWN -8
#define OLED_ON -7    
#define OLED_OFF -5
#define CONNECTION_TEST -2
#define ROTARY_DELAY_USEC 100000
#define ROTARY_OUTPUTS_DELAY 10
#define MIN_BUTTON_PRESSES_INTERVAL_MSEC 300
#define SWITCH_ROTATION_CCW -3
#define SWITCH_ROTATION_CW -4
#define MOD_PROBE_SUCCESS 0
#define MOD_PROBE_UNKNOWN -1
#define MOD_PROBE_FAIL 1
#define IMAGE_DATA_NUM 1024

uint32_t rotatorPressedTimestamp = 0;

typedef struct {
  char imageName[64];
  char currentDiskNum;  
  char totalDisksNum;  
} imageEntryData;

imageEntryData imageData[IMAGE_DATA_NUM];

bool aState = LOW, bState = LOW, settingGotekImage = false, dataFileFound = false, initProcessSuccess = false;
int8_t counter = 0, rotatorPressedCounter = 0, modProbeResult = MOD_PROBE_UNKNOWN;
bool oledAvailable = false, clearOledDisplay = false;

const int imageSettingIdx_eepromAdress = 0;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static uint8_t prevNextCode = 0;
static uint16_t store = 0;
static int16_t imageSettingIdx = -1, totalAvailableImages = 0, initOledStatus = OLED_STATUS_UNKNOWN;
uint32_t selectionSleepTimeMsec = OLED_TIMEOUT_MSEC;

//gotek_ctrl mac is CC:50:E3:B5:9E:38
//gotek_mod mac is CC:50:E3:B5:A1:1C
uint8_t gotekCtrlAddress[6];
//uint8_t gotekModAddress[] = {0xCC, 0x50, 0xE3, 0xB5, 0xA1, 0x1C};
uint8_t gotekModAddress[6]; //read from the SD card
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

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

//callback function that will be executed when data is received
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {  
  int16_t dataReceived = 0;
  
  memcpy(&dataReceived, incomingData, sizeof(int16_t));
  Serial.print("Bytes received: ");
  Serial.println(len);  
  Serial.println("dataReceived = " + String(dataReceived)); 
  if (dataReceived >= 0) {
    imageSettingIdx = dataReceived;
    Serial.println("imageSettingIdx = " + String(imageSettingIdx)); 
    saveInt16ToEEPROM(imageSettingIdx_eepromAdress,imageSettingIdx);   
  }  
  else {
    if (dataReceived == (int16_t)OLED_OFF) {
      clearOledDisplay = true;
      initOledStatus = (int16_t)OLED_OFF;
    }
    else if (dataReceived == (int16_t)OLED_ON) {
      clearOledDisplay = false;
      initOledStatus = (int16_t)OLED_ON;
    }
  }  
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
  (status == ESP_NOW_SEND_SUCCESS) ? modProbeResult = MOD_PROBE_SUCCESS : modProbeResult = MOD_PROBE_FAIL;  
}

void rotatorPressedRoutine() {
  rotatorPressedCounter++;
  rotatorPressedTimestamp = millis();
  Serial.println("rotatorPressedRoutine, rotatorPressedCounter = " + String(rotatorPressedCounter) + ", settingGotekImage = " + String(settingGotekImage));  
  if (settingGotekImage) {
    if (imageSettingIdx >= 0) {
      settingGotekImage = false;
      sendDataToMod(imageSettingIdx);    
      writeToDisplay("Image set!");    
      delay(1000);
      showImageDataToDisplay();
    }
  }
  else {    
    setGotekImage();  
  }  
}

// A vald CW or  CCW move returns 1, invalid returns 0.
//credits for the debouncing function here: https://www.best-microcontroller-projects.com/rotary-encoder.html
int8_t readRotary() {
  static int8_t rotEncTable[] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0};

  prevNextCode <<= 2;
  if (digitalRead(KY040_S1)) prevNextCode |= 0x02;
  if (digitalRead(KY040_S2)) prevNextCode |= 0x01;
  prevNextCode &= 0x0f;

  // If valid then store as 16 bit data.
  if  (rotEncTable[prevNextCode] ) {
    store <<= 4;
    store |= prevNextCode;      
    if ((store&0xff)==0x2b) return -1;
    if ((store&0xff)==0x17) return 1;
  }
  return 0;
}

void sendDataToMod(int16_t dataToSend) {
  esp_err_t result = esp_now_send(0, (uint8_t *) &dataToSend, sizeof(int16_t));  
}

void detectRotation() {  
  int8_t val;  bool cw = false;   

  if (val=readRotary()) {    
    cw = (val == 1);

    if (settingGotekImage) {
      imageSettingIdx +=val;      
      if (imageSettingIdx < 0) {
        imageSettingIdx = totalAvailableImages-1;  
      }
      if (imageSettingIdx > totalAvailableImages-1) {
        imageSettingIdx = 0; 
      }
      Serial.println("detectRotation , imageSettingIdx = " + String(imageSettingIdx));
      saveInt16ToEEPROM(imageSettingIdx_eepromAdress,imageSettingIdx);
    }
    setRotaryEncOutputsGotekStates(cw); 
  }  
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

  showImageDataToDisplay();
  
  if (!settingGotekImage) {    
    int16_t cwRotation = (cw == true) ? SWITCH_ROTATION_CW : SWITCH_ROTATION_CCW;
    //esp_err_t result = esp_now_send(0, (uint8_t *) &cwRotation, sizeof(uint8_t));  
    sendDataToMod(cwRotation);
  }
}

void showImageDataToDisplay() {
  Serial.println("imageSettingIdx = " + String(imageSettingIdx));
  if ((imageSettingIdx >=0) && (imageSettingIdx < IMAGE_DATA_NUM)) {
    String textToDisplay = String("Image ") + String(imageSettingIdx) + String( " / ") + String(totalAvailableImages-1) + String("\n\n") + String(imageData[imageSettingIdx].imageName) + String("\nDisk ") + String(imageData[imageSettingIdx].currentDiskNum) + String(" / ") + String(imageData[imageSettingIdx].totalDisksNum);
    writeToDisplay(textToDisplay);  
  }
  else {  //ask the user to set the currently selected image in Gotek
    setGotekImage();
  }
}

void setGotekImage(){
  settingGotekImage = true;    
  writeToDisplay("Set active image.");
}

void writeToDisplay(String text) {
  if (oledAvailable) {    
    Serial.println("writeToDisplay: text = " + text);      
    display.clearDisplay();
    display.setCursor(0, 0);  
    display.println(text);
    display.display(); 

    digitalWrite(LED_PIN,text.length() > 0);
  }  
}

void readDataFile(File imagesFile) {
  Serial.println("readDataFile...");      
  char buffer[64]; 
  int i=0;
  while (imagesFile.available()) {
    int l = imagesFile.readBytesUntil('\n', buffer, sizeof(buffer));
    buffer[l] = 0;

    char* imageMetaData = strtok(buffer, ","); 
    if (i==0) {  //contains the mod MAC address      
      sscanf(imageMetaData,"%02x",&gotekModAddress[0]);                       
      imageMetaData = strtok(NULL, ","); sscanf(imageMetaData,"%02x",&gotekModAddress[1]);  
      imageMetaData = strtok(NULL, ","); sscanf(imageMetaData,"%02x",&gotekModAddress[2]);  
      imageMetaData = strtok(NULL, ","); sscanf(imageMetaData,"%02x",&gotekModAddress[3]);  
      imageMetaData = strtok(NULL, ","); sscanf(imageMetaData,"%02x",&gotekModAddress[4]);  
      imageMetaData = strtok(NULL, ","); sscanf(imageMetaData,"%02x",&gotekModAddress[5]);  
      
      Serial.print("mod MAC address: ");
      for (int j=0;j<5;j++) {
        Serial.print(String(gotekModAddress[j],HEX) + ":");
      }
      Serial.println(String(gotekModAddress[5],HEX));
    }
    else {
      strcpy(&(imageData[i-1].imageName[0]),imageMetaData);
      imageMetaData = strtok(NULL, ","); 
      strcpy(&(imageData[i-1].currentDiskNum),imageMetaData);
      imageMetaData = strtok(NULL, ","); 
      strcpy(&(imageData[i-1].totalDisksNum),imageMetaData);
    }      
    i++;     
  }
  totalAvailableImages = i-1;
  Serial.println("totalAvailableImages = " + String(totalAvailableImages));
  printImageNames(); 
}

void printImageNames() {  
  for (int i=0;i<totalAvailableImages;i++) {    
    Serial.println("imageName: " + String(imageData[i].imageName) + ", currentDiskNum: " + String(imageData[i].currentDiskNum) + ", totalDisksNum: " + String(imageData[i].totalDisksNum));
  }
}


bool initializeOLED() {
  byte errorResult;
  Wire.beginTransmission(DISPLAY_I2C_ADDRESS);
  errorResult = Wire.endTransmission(); 

  if (errorResult == 0) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, DISPLAY_I2C_ADDRESS)) { // Address 0x3C for 128x32
      Serial.println("SSD1306 allocation failed"); 
      return false;   
    }
    else {
      Serial.println("SSD1306 allocation successful!");    
      // Clear the buffer
      display.clearDisplay();
      display.setTextSize(1); // Draw 2X-scale text
      display.setTextColor(SSD1306_WHITE);    
    }
    return true;
  }

  Serial.println("SSD1306 not available!");    
  return false;
}

void checkIfRotatorPressed() {
  if (digitalRead(KY040_BUTTON) == LOW) {
    if (rotatorPressedTimestamp == 0) {
      rotatorPressedRoutine();    
    }
    else {    
      if (millis() - rotatorPressedTimestamp > MIN_BUTTON_PRESSES_INTERVAL_MSEC) {
        rotatorPressedRoutine();
      }
    }
  }  
}

bool sendInitDataToMod() {
  uint8_t maxTries = 20, progress = 0, progressStep = 12;
  
  Serial.println("sendInitDataToMod..");

  WiFi.macAddress(gotekCtrlAddress);
  for (int i=0;i<6;i++) {
    Serial.println("gotekCtrlAddress[" + String(i) + "] = " + String(gotekCtrlAddress[i],HEX));    
  } 
  
  do {
    delay(1000);
    sendDataToMod((int16_t)RX_DATA_START);
    if (modProbeResult != MOD_PROBE_SUCCESS) {
      maxTries--;      
    }    
  }
  while ((modProbeResult != MOD_PROBE_SUCCESS) && (maxTries >= 0));
  progress = progress + progressStep;
  writeToDisplay("Connecting to mod...\n" + String(progress) + "%");

  for (int i=0;i<6;i++) {
    do {
    delay(1000);
      sendDataToMod((int16_t)gotekCtrlAddress[i]);  
      if (modProbeResult != MOD_PROBE_SUCCESS) {
        maxTries--;      
      } 
    }
    while ((modProbeResult != MOD_PROBE_SUCCESS) && (maxTries >= 0));
    progress = progress + progressStep;
    writeToDisplay("Connecting to mod...\n" + String(progress) + "%");
  }

  do {
    delay(1000);
    sendDataToMod((int16_t)totalAvailableImages);
    if (modProbeResult != MOD_PROBE_SUCCESS) {
      maxTries--;      
    }    
  }
  while ((modProbeResult != MOD_PROBE_SUCCESS) && (maxTries >= 0));

  Serial.println("sendInitDataToMod, maxTries = " + String(maxTries));
  if (maxTries >=0) {
    writeToDisplay("Connecting to mod...\ndone!");
    return true;
  }

  writeToDisplay("Connecting to mod...\nerror :(");
  return false;
   
}


void setup() {
  int i=0;
  Serial.begin(SERIAL_UART_BAUD);
  delay(1000);  
  Serial.println("Starting gotek ctrl..."); 
  Wire.begin();  
  delay(1000);  
  WiFi.mode(WIFI_STA);  
  delay(1000);  
  Serial.println("MAC address: " + WiFi.macAddress());  

  EEPROM.begin(512);  
  //clearEEPROM(); 

  imageSettingIdx = loadInt16FromEEPROM(imageSettingIdx_eepromAdress); 
  Serial.println("imageSettingIdx = " + String(imageSettingIdx));

  pinMode(KY040_S2, INPUT);  
  pinMode(KY040_S2, INPUT_PULLUP);  
  pinMode(KY040_S1, INPUT);  
  pinMode(KY040_S1, INPUT_PULLUP);
  pinMode(KY040_BUTTON, INPUT); 

  pinMode(ROTARY_ENC_OUT_A, OUTPUT);
  setRotaryEncOutputA(LOW);
  pinMode(ROTARY_ENC_OUT_B, OUTPUT);
  setRotaryEncOutputB(LOW);
  pinMode(LED_PIN, OUTPUT);

  oledAvailable = initializeOLED();  
  writeToDisplay("Initializing...");  
  
  if (!SD.begin(SD_CS)){
    Serial.println("Card Mount Failed.");
    writeToDisplay("Card Mount Failed.");    
    return;
  }
  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE){
    Serial.println("No SD card attached.");
    writeToDisplay("No SD card attached.");
    return;
  }

  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC){
    Serial.println("MMC");
  } 
  else if (cardType == CARD_SD){
    Serial.println("SDSC");
  } 
  else if (cardType == CARD_SDHC){
    Serial.println("SDHC");
  } 
  else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);
  
  
  File imagesFile = SD.open("/data.cfg"); 
  if (!imagesFile){
    Serial.println("There was an error opening imagesFile");  
    writeToDisplay("\"data.cfg\" file not\nfound!"); 
    dataFileFound = false; 
    return;
  }
  else {
    Serial.println("imagesFile size = " + String(imagesFile.size()) + " bytes"); 
    dataFileFound = true;
    readDataFile(imagesFile);   
  }
  imagesFile.close();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    writeToDisplay("Error connecting with Gotek.");
    return;
  }
  else {
    Serial.println("ESP-NOW initialized!");
  }
  
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  // register gotekMod
  esp_now_peer_info_t gotekModInfo;
  gotekModInfo.channel = 0;  
  gotekModInfo.encrypt = false;
  // register first peer  
  memcpy(gotekModInfo.peer_addr, gotekModAddress, 6);
  if (esp_now_add_peer(&gotekModInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    return;
  }
  else {
    Serial.println("Successfully added Gotek mod!");
    writeToDisplay("Connecting to mod...");
  }

  do {
    sendDataToMod((int16_t)CONNECTION_TEST);
    Serial.println("modProbeResult = " + String(modProbeResult));
    delay(1000);  
  }
  while (modProbeResult != MOD_PROBE_SUCCESS); 

  if (!dataFileFound) {
    writeToDisplay("\"data.cfg\" file not\nfound!"); 
    delay(1000);
  }

  //send initial data to mod
  initProcessSuccess = sendInitDataToMod();
  
  
}

void loop() {
  static bool connectionSuccess = true; 
  static int16_t prevImageSettingIdx = -1; 

  if (!initProcessSuccess) {
    return;
  }

  if (modProbeResult == MOD_PROBE_SUCCESS) {
    if (!connectionSuccess) {
      connectionSuccess = true;    
      showImageDataToDisplay();             
    }
    detectRotation();
    checkIfRotatorPressed(); 
    if (prevImageSettingIdx != imageSettingIdx) {
      showImageDataToDisplay(); 
      prevImageSettingIdx = imageSettingIdx;
    }      

    if (clearOledDisplay) {
      writeToDisplay("");
      clearOledDisplay = false;
    }
    
  }
  else {
    connectionSuccess = false;
    writeToDisplay("Not connected to mod!");
    sendDataToMod((int16_t)CONNECTION_TEST);
    delay(1000);
  }  
}
