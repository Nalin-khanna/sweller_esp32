#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>

// --- PIN DEFINITIONS ---
// Buttons (Active Low)
#define BTN_UP 33
#define BTN_DOWN 27
#define BTN_SEL 13

// Mic (I2S)
#define I2S_WS 25
#define I2S_SCK 26
#define I2S_SD 32
#define I2S_PORT I2S_NUM_0

// SD Card (SPI) - Pins for Wrover (CS=14)
#define SD_CS 14
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23

// OLED (I2C)
#define OLED_RST 4
#define OLED_SDA 21
#define OLED_SCL 22

// --- CONFIG ---
#define SAMPLE_RATE 16000
#define SD_SPI_FREQ 400000
#define MAX_RECORD_TIME_MS 1800000 // 30 Minutes
#define SCREEN_DIM_LEVEL 1         // Lowest brightness (1-255)
#define SCREEN_BRIGHT_LEVEL 255    // Full brightness

const char* TEACHER_NAMES[10] = {
  "Priya Sharma", "Rajesh Kumar", "Anita Patel", "Vikram Singh",
  "Meera Reddy", "Amit Gupta", "Kavita Rao", "Sanjay Mehta",
  "Deepa Nair", "Arjun Verma"
};

// --- GLOBALS ---
U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2(U8G2_R0, OLED_RST);
int currentSelection = 0;

enum MenuState { MENU_TEACHER_LIST, MENU_TEACHER_SELECTED, MENU_RECORDING };
MenuState currentMenu = MENU_TEACHER_LIST;
int selectedTeacher = 0;

struct wav_header_t {
  char riff[4]; uint32_t flength; char wave[4]; char fmt[4]; uint32_t chunk_size;
  uint16_t format_tag; uint16_t num_chans; uint32_t srate; uint32_t bytes_per_sec;
  uint16_t bytes_per_samp; uint16_t bits_per_samp; char data[4]; uint32_t dlength;
};

void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK, .ws_io_num = I2S_WS, .data_out_num = -1, .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
}

void updateWavHeader(File &file) {
  unsigned long fileSize = file.size();
  wav_header_t header;
  memcpy(header.riff, "RIFF", 4); header.flength = fileSize - 8;
  memcpy(header.wave, "WAVE", 4); memcpy(header.fmt, "fmt ", 4);
  header.chunk_size = 16; header.format_tag = 1; header.num_chans = 1;
  header.srate = SAMPLE_RATE; header.bits_per_samp = 16;
  header.bytes_per_sec = header.srate * header.num_chans * (header.bits_per_samp / 8);
  header.bytes_per_samp = header.num_chans * (header.bits_per_samp / 8);
  memcpy(header.data, "data", 4); header.dlength = fileSize - 44;
  file.seek(0); file.write((uint8_t*)&header, sizeof(header));
}

String getNextFilename(String folderPath) {
  int id = 1;
  while (true) {
    String filename = folderPath + "/" + String(id) + ".wav";
    if (!SD.exists(filename)) return filename;
    id++;
    if (id > 9999) return folderPath + "/overflow.wav";
  }
}

void recordAudio(String teacherName) {
  // 1. GO DIM (Start of Recording)
  u8g2.setContrast(SCREEN_DIM_LEVEL); 
  
  String folderPath = "/" + teacherName;
  folderPath.replace(" ", "_");
  if (!SD.exists(folderPath)) SD.mkdir(folderPath);
  
  String filename = getNextFilename(folderPath);
  File file = SD.open(filename, FILE_WRITE);
  
  if(!file) {
    u8g2.setContrast(SCREEN_BRIGHT_LEVEL); // Restore brightness for error
    u8g2.clearBuffer(); u8g2.drawStr(10, 30, "Write Error!"); u8g2.sendBuffer();
    delay(2000); return;
  }

  uint8_t header[44] = {0};
  file.write(header, 44);

  // Initial Draw
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(20, 10, "RECORDING");
  u8g2.drawLine(0, 15, 127, 15);
  u8g2.setCursor(5, 30);
  u8g2.print(teacherName.length() > 18 ? teacherName.substring(0, 15) + "..." : teacherName);
  u8g2.setCursor(5, 45);
  u8g2.print("File: " + filename.substring(folderPath.length() + 1));
  u8g2.sendBuffer();

  // 8KB Buffer for Power Efficiency
  const int i2s_buffer_size = 8192;
  char *i2s_buff = (char*) calloc(i2s_buffer_size, sizeof(char));
  size_t bytes_read;
  
  // Flush Mic
  i2s_read(I2S_PORT, i2s_buff, i2s_buffer_size, &bytes_read, 0);

  unsigned long recordingStart = millis();
  unsigned long lastScreenUpdate = 0;
  
  // --- RECORDING LOOP ---
  while (true) {
    // 1. Read Audio
    i2s_read(I2S_PORT, i2s_buff, i2s_buffer_size, &bytes_read, portMAX_DELAY);
    
    // 2. Write to SD
    file.write((const uint8_t*)i2s_buff, bytes_read);

    unsigned long currentMillis = millis();
    unsigned long duration = currentMillis - recordingStart;

    // 3. Update Screen (Stays DIM)
    if (currentMillis - lastScreenUpdate > 1000) {
      lastScreenUpdate = currentMillis;
      unsigned long seconds = duration / 1000;
      u8g2.setDrawColor(0); u8g2.drawBox(70, 50, 58, 14); u8g2.setDrawColor(1);
      u8g2.setCursor(70, 60); u8g2.printf("%02lu:%02lu", seconds / 60, seconds % 60);
      u8g2.sendBuffer();
    }
    
    // Check Stop Button
    if (digitalRead(BTN_SEL) == LOW || duration >= MAX_RECORD_TIME_MS) {
      if (duration < MAX_RECORD_TIME_MS) { 
        delay(50); while(digitalRead(BTN_SEL) == LOW); 
      }
      break; 
    }
  }

  // 2. GO BRIGHT (End of Recording)
  u8g2.setContrast(SCREEN_BRIGHT_LEVEL); 
  
  free(i2s_buff);
  updateWavHeader(file);
  file.close();
  
  unsigned long totalDuration = (millis() - recordingStart) / 1000;
  u8g2.clearBuffer(); u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(35, 25, "SAVED!");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor(25, 45);
  u8g2.printf("Time: %lu:%02lu", totalDuration / 60, totalDuration % 60);
  u8g2.sendBuffer();
  delay(2000);
  
  currentMenu = MENU_TEACHER_SELECTED;
}

void drawTeacherList() {
  u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(15, 10, "Select Teacher"); u8g2.drawLine(0, 12, 127, 12);
  
  int startIdx = currentSelection - 1;
  if (startIdx < 0) startIdx = 0;
  if (startIdx > 6) startIdx = 6;

  for (int i = 0; i < 4; i++) {
    int idx = startIdx + i;
    if (idx >= 10) break;
    int y = 25 + (i * 10);
    if (idx == currentSelection) { u8g2.drawBox(0, y - 8, 128, 10); u8g2.setDrawColor(0); }
    else { u8g2.setDrawColor(1); }
    u8g2.setCursor(5, y); u8g2.print(TEACHER_NAMES[idx]);
  }
  u8g2.setDrawColor(1); u8g2.sendBuffer();
}

void drawTeacherSelected() {
  u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x10_tr);
  String tName = String(TEACHER_NAMES[selectedTeacher]);
  u8g2.setCursor((128 - (tName.length() > 20 ? 17 : tName.length()) * 6) / 2, 12);
  u8g2.print(tName.length() > 20 ? tName.substring(0, 17) + "..." : tName);
  u8g2.drawLine(0, 15, 127, 15);
  
  int option = currentSelection; 
  if (option == 0) { u8g2.drawBox(5, 25, 118, 15); u8g2.setDrawColor(0); }
  else { u8g2.setDrawColor(1); u8g2.drawFrame(5, 25, 118, 15); }
  u8g2.drawStr(20, 36, "Start Recording");
  
  u8g2.setDrawColor(1);
  if (option == 1) { u8g2.drawBox(5, 45, 118, 15); u8g2.setDrawColor(0); }
  else { u8g2.drawFrame(5, 45, 118, 15); }
  u8g2.drawStr(45, 56, "Back");
  u8g2.setDrawColor(1); u8g2.sendBuffer();
}

void setup() {
  // CPU @ 80MHz for power saving
  setCpuFrequencyMhz(80);

  Serial.begin(115200);
  WiFi.mode(WIFI_OFF); // Radios OFF
  
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);

  u8g2.begin(); 
  u8g2.setContrast(SCREEN_BRIGHT_LEVEL); // Start BRIGHT
  u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(25, 30, "System Init..."); u8g2.sendBuffer();

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(200);

  if (!SD.begin(SD_CS, SPI, SD_SPI_FREQ)) {
    u8g2.clearBuffer(); u8g2.drawStr(20, 30, "SD FAIL!");
    u8g2.sendBuffer(); while(1) delay(1000);
  }

  initI2S();
  u8g2.clearBuffer(); u8g2.drawStr(35, 30, "Ready!"); u8g2.sendBuffer();
  delay(1000);
  currentMenu = MENU_TEACHER_LIST;
}

void loop() {
  if (currentMenu == MENU_TEACHER_LIST) {
    drawTeacherList();
    if (digitalRead(BTN_UP) == LOW) { if (currentSelection > 0) currentSelection--; delay(150); }
    if (digitalRead(BTN_DOWN) == LOW) { if (currentSelection < 9) currentSelection++; delay(150); }
    if (digitalRead(BTN_SEL) == LOW) {
      delay(200); selectedTeacher = currentSelection; currentSelection = 0; currentMenu = MENU_TEACHER_SELECTED;
    }
  }
  else if (currentMenu == MENU_TEACHER_SELECTED) {
    drawTeacherSelected();
    if (digitalRead(BTN_UP) == LOW) { currentSelection = 0; delay(150); }
    if (digitalRead(BTN_DOWN) == LOW) { currentSelection = 1; delay(150); }
    if (digitalRead(BTN_SEL) == LOW) {
      delay(200);
      if (currentSelection == 0) recordAudio(String(TEACHER_NAMES[selectedTeacher]));
      else { currentSelection = selectedTeacher; currentMenu = MENU_TEACHER_LIST; }
    }
  }
}