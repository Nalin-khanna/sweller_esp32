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

// Mic (I2S INMP441)
#define I2S_WS 25
#define I2S_SCK 26
#define I2S_SD 32
#define I2S_PORT I2S_NUM_0

// SD Card (SPI) - Pins for Wrover (CS=5)
#define SD_CS 5
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

typedef struct __attribute__((packed)) {
  char riff[4];
  uint32_t flength;
  char wave[4];
  char fmt[4];
  uint32_t chunk_size;
  uint16_t format_tag;
  uint16_t num_chans;
  uint32_t srate;
  uint32_t bytes_per_sec;
  uint16_t bytes_per_samp;
  uint16_t bits_per_samp;
  char data[4];
  uint32_t dlength;
} wav_header_t;


void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    // INMP441 outputs 32-bit data (18-bit actual, left-justified in 32-bit)
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    // Standard I2S format for INMP441
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    // Buffer size in samples - INMP441 works well with 1024
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,      // Serial Clock (SCK) - GPIO 26
    .ws_io_num = I2S_WS,        // Word Select (WS/LRCK) - GPIO 25
    .data_out_num = I2S_PIN_NO_CHANGE,  // Not used for input
    .data_in_num = I2S_SD       // Serial Data (SD) - GPIO 32
  };
  
  // Install I2S driver
  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("ERROR: I2S driver install failed: %d\n", err);
  } else {
    Serial.println("I2S driver installed successfully");
  }
  
  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("ERROR: I2S set pin failed: %d\n", err);
  } else {
    Serial.println("I2S pins configured successfully");
  }
  
  // Set sample rate explicitly
  err = i2s_set_sample_rates(I2S_PORT, SAMPLE_RATE);
  if (err != ESP_OK) {
    Serial.printf("ERROR: I2S set sample rate failed: %d\n", err);
  }
  
  Serial.println("\nINMP441 I2S Configuration:");
  Serial.println("- Sample Rate: 16000 Hz");
  Serial.println("- Bits per Sample: 32-bit (input)");
  Serial.println("- Output Format: 16-bit WAV");
  Serial.println("- Channel: Mono (Left)");
}

void updateWavHeader(File &file) {
  if (!file) {
    Serial.println("ERROR: Cannot update WAV header - invalid file");
    return;
  }
  file.flush();
  uint32_t fileSize = file.size();
  
  if (fileSize < 44) {
    Serial.println("ERROR: File too small for WAV header");
    return;
  }
  
  wav_header_t header;
  
  memcpy(header.riff, "RIFF", 4); 
  header.flength = fileSize - 8;
  memcpy(header.wave, "WAVE", 4); 
  memcpy(header.fmt, "fmt ", 4);
  header.chunk_size = 16; 
  header.format_tag = 1;  // PCM
  header.num_chans = 1;   // Mono
  header.srate = SAMPLE_RATE; 
  header.bits_per_samp = 16;  // Output 16-bit WAV
  header.bytes_per_sec = header.srate * header.num_chans * (header.bits_per_samp / 8);
  header.bytes_per_samp = header.num_chans * (header.bits_per_samp / 8);
  memcpy(header.data, "data", 4); 
  header.dlength = fileSize - 44;
  
  file.seek(0); 
  size_t written = file.write((uint8_t*)&header, sizeof(header));
  
  if (written != sizeof(header)) {
    Serial.printf("ERROR: WAV header write incomplete: %d/%d bytes\n", written, sizeof(header));
  } else {
    Serial.println("WAV header updated successfully");
  }
}

String getNextFilename(String folderPath) {
  int id = 1;
  while (true) {
    String filename = folderPath + "/" + String(id) + ".wav";
    if (!SD.exists(filename)) return filename;
    id++;
    if (id > 9999) {
      Serial.println("WARNING: Max file count reached, using overflow.wav");
      return folderPath + "/overflow.wav";
    }
  }
}

void recordAudio(String teacherName) {
  Serial.println("\n========== RECORDING STARTED ==========");
  
  // 1. GO DIM (Start of Recording)
  u8g2.setContrast(SCREEN_DIM_LEVEL); 
  
  String folderPath = "/" + teacherName;
  folderPath.replace(" ", "_");
  
  if (!SD.exists(folderPath)) {
    if (!SD.mkdir(folderPath)) {
      Serial.printf("ERROR: Failed to create folder: %s\n", folderPath.c_str());
      u8g2.setContrast(SCREEN_BRIGHT_LEVEL);
      u8g2.clearBuffer(); 
      u8g2.drawStr(10, 30, "Folder Error!"); 
      u8g2.sendBuffer();
      delay(2000); 
      return;
    }
    Serial.printf("Created folder: %s\n", folderPath.c_str());
  }
  
  String filename = getNextFilename(folderPath);
  Serial.printf("Opening file: %s\n", filename.c_str());
  
  File file = SD.open(filename, FILE_WRITE);
  
  if(!file) {
    Serial.println("ERROR: Failed to open file for writing");
    u8g2.setContrast(SCREEN_BRIGHT_LEVEL);
    u8g2.clearBuffer(); 
    u8g2.drawStr(10, 30, "Write Error!"); 
    u8g2.sendBuffer();
    delay(2000); 
    return;
  }

  // Write placeholder header (will be updated at end)
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

  // Buffer sizes for INMP441 32-bit processing
  const int samples_to_read = 2048;  // Number of samples per read
  const int i2s_buffer_size = samples_to_read * 4;  // 4 bytes per 32-bit sample
  const int output_buffer_size = samples_to_read * 2;  // 2 bytes per 16-bit output
  
  int32_t *i2s_buff = (int32_t*) calloc(samples_to_read, sizeof(int32_t));
  int16_t *output_buff = (int16_t*) calloc(samples_to_read, sizeof(int16_t));
  
  if (i2s_buff == NULL || output_buff == NULL) {
    Serial.println("ERROR: Failed to allocate buffers!");
    Serial.printf("  i2s_buff: %s\n", i2s_buff ? "OK" : "FAILED");
    Serial.printf("  output_buff: %s\n", output_buff ? "OK" : "FAILED");
    
    if (i2s_buff) free(i2s_buff);
    if (output_buff) free(output_buff);
    file.close();
    
    u8g2.setContrast(SCREEN_BRIGHT_LEVEL);
    u8g2.clearBuffer(); 
    u8g2.drawStr(10, 30, "Memory Error!"); 
    u8g2.sendBuffer();
    delay(2000); 
    return;
  }
  
  Serial.printf("Buffers allocated: %d samples (%d bytes I2S, %d bytes output)\n", 
                samples_to_read, i2s_buffer_size, output_buffer_size);
  
  size_t bytes_read;
  
  // Flush microphone buffer - INMP441 needs time to stabilize
  Serial.println("Flushing INMP441 buffer (5 cycles)...");
  for (int i = 0; i < 5; i++) {
    esp_err_t result = i2s_read(I2S_PORT, i2s_buff, i2s_buffer_size, &bytes_read, 100);
    if (result != ESP_OK) {
      Serial.printf("  Flush cycle %d failed: %d\n", i + 1, result);
    }
    delay(100);
  }
  
  // Zero the I2S DMA buffer
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("Microphone ready");
  
  Serial.println("Recording loop started...");

  unsigned long recordingStart = millis();
  unsigned long lastScreenUpdate = 0;
  unsigned long totalBytesWritten = 0;
  unsigned long totalSamplesProcessed = 0;
  int debugCounter = 0;
  int errorCount = 0;
  
  // --- RECORDING LOOP ---
  while (true) {
    // 1. Read 32-bit audio from INMP441
    esp_err_t result = i2s_read(I2S_PORT, i2s_buff, i2s_buffer_size, &bytes_read, portMAX_DELAY);
    
    if (result != ESP_OK) {
      errorCount++;
      if (errorCount % 100 == 1) {  // Print every 100th error to avoid spam
        Serial.printf("I2S read error: %d (count: %d)\n", result, errorCount);
      }
      continue;
    }
    
    if (bytes_read == 0) {
      errorCount++;
      if (errorCount % 100 == 1) {
        Serial.printf("Warning: 0 bytes read (count: %d)\n", errorCount);
      }
      continue;
    }
    
    if (bytes_read % 4 != 0) {
      Serial.printf("ERROR: Invalid byte count (not multiple of 4): %d\n", bytes_read);
      continue;
    }
    
    int samples_read = bytes_read / 4;  // 4 bytes per 32-bit sample
    
    // 2. Convert 32-bit to 16-bit
    // INMP441 outputs 18-bit audio left-justified in 32-bit format
    // We shift right by 14 bits to get the most significant 16 bits
    for (int i = 0; i < samples_read; i++) {
      // Right shift by 14 bits to convert 32-bit to 16-bit
      // This preserves the 18-bit audio data in the 16-bit output
      output_buff[i] = (i2s_buff[i] >> 14);
    }
    
    // 3. Write 16-bit data to SD card
    size_t bytes_to_write = samples_read * 2;  // 2 bytes per 16-bit sample
    size_t bytes_written = file.write((const uint8_t*)output_buff, bytes_to_write);
    
    if (bytes_written != bytes_to_write) {
      Serial.printf("SD write error: wrote %d of %d bytes\n", bytes_written, bytes_to_write);
    }
    
    totalBytesWritten += bytes_written;
    totalSamplesProcessed += samples_read;

    unsigned long currentMillis = millis();
    unsigned long duration = currentMillis - recordingStart;

    // 4. Update Screen (Stays DIM)
    if (currentMillis - lastScreenUpdate > 1000) {
      lastScreenUpdate = currentMillis;
      unsigned long seconds = duration / 1000;
      u8g2.setDrawColor(0); 
      u8g2.drawBox(70, 50, 58, 14); 
      u8g2.setDrawColor(1);
      u8g2.setCursor(70, 60); 
      u8g2.printf("%02lu:%02lu", seconds / 60, seconds % 60);
      u8g2.sendBuffer();
      
      // Debug output every 5 seconds
      if (debugCounter % 5 == 0) {
        Serial.printf("[%02lu:%02lu] Bytes: %lu | Samples: %lu | Errors: %d | Sample: %d\n", 
                      seconds / 60, seconds % 60, totalBytesWritten, 
                      totalSamplesProcessed, errorCount, output_buff[0]);
      }
      debugCounter++;
    }
    
    // 5. Check Stop Button
    if (digitalRead(BTN_SEL) == LOW || duration >= MAX_RECORD_TIME_MS) {
      if (duration < MAX_RECORD_TIME_MS) { 
        Serial.println("Stop button pressed");
        delay(50); 
        while(digitalRead(BTN_SEL) == LOW) delay(10); 
      } else {
        Serial.println("Maximum recording time reached");
      }
      break; 
    }
  }

  Serial.println("Recording loop ended");
  
  // 2. GO BRIGHT (End of Recording)
  u8g2.setContrast(SCREEN_BRIGHT_LEVEL); 
  
  // Clean up
  free(i2s_buff);
  free(output_buff);
  Serial.println("Buffers freed");
  
  // Update WAV header with final file size
  file.flush();
  updateWavHeader(file);
  file.close();
  
  Serial.println("\n========== RECORDING STATISTICS ==========");
  Serial.printf("Total bytes written: %lu\n", totalBytesWritten);
  Serial.printf("Total samples: %lu\n", totalSamplesProcessed);
  Serial.printf("Sample rate: %d Hz\n", SAMPLE_RATE);
  Serial.printf("Expected duration: %.2f seconds\n", (float)totalBytesWritten / (SAMPLE_RATE * 2));
  Serial.printf("I2S errors: %d\n", errorCount);
  Serial.println("==========================================\n");
  
  unsigned long totalDuration = (millis() - recordingStart) / 1000;
  u8g2.clearBuffer(); 
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(35, 25, "SAVED!");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor(25, 45);
  u8g2.printf("Time: %lu:%02lu", totalDuration / 60, totalDuration % 60);
  u8g2.sendBuffer();
  delay(2000);
  
  currentMenu = MENU_TEACHER_SELECTED;
}

void drawTeacherList() {
  u8g2.clearBuffer(); 
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(15, 10, "Select Teacher"); 
  u8g2.drawLine(0, 12, 127, 12);
  
  int startIdx = currentSelection - 1;
  if (startIdx < 0) startIdx = 0;
  if (startIdx > 6) startIdx = 6;

  for (int i = 0; i < 4; i++) {
    int idx = startIdx + i;
    if (idx >= 10) break;
    int y = 25 + (i * 10);
    if (idx == currentSelection) { 
      u8g2.drawBox(0, y - 8, 128, 10); 
      u8g2.setDrawColor(0); 
    }
    else { 
      u8g2.setDrawColor(1); 
    }
    u8g2.setCursor(5, y); 
    u8g2.print(TEACHER_NAMES[idx]);
  }
  u8g2.setDrawColor(1); 
  u8g2.sendBuffer();
}

void drawTeacherSelected() {
  u8g2.clearBuffer(); 
  u8g2.setFont(u8g2_font_6x10_tr);
  String tName = String(TEACHER_NAMES[selectedTeacher]);
  u8g2.setCursor((128 - (tName.length() > 20 ? 17 : tName.length()) * 6) / 2, 12);
  u8g2.print(tName.length() > 20 ? tName.substring(0, 17) + "..." : tName);
  u8g2.drawLine(0, 15, 127, 15);
  
  int option = currentSelection; 
  if (option == 0) { 
    u8g2.drawBox(5, 25, 118, 15); 
    u8g2.setDrawColor(0); 
  }
  else { 
    u8g2.setDrawColor(1); 
    u8g2.drawFrame(5, 25, 118, 15); 
  }
  u8g2.drawStr(20, 36, "Start Recording");
  
  u8g2.setDrawColor(1);
  if (option == 1) { 
    u8g2.drawBox(5, 45, 118, 15); 
    u8g2.setDrawColor(0); 
  }
  else { 
    u8g2.drawFrame(5, 45, 118, 15); 
  }
  u8g2.drawStr(45, 56, "Back");
  u8g2.setDrawColor(1); 
  u8g2.sendBuffer();
}

void setup() {
  // CPU @ 80MHz for power saving
  setCpuFrequencyMhz(80);

  Serial.begin(115200);
  delay(500);
  
  Serial.println("\n\n");
  Serial.println("================================================");
  Serial.println("    Sweller ESP32 Audio Recorder v2.0");
  Serial.println("    Optimized for INMP441 Microphone");
  Serial.println("================================================");
  
  WiFi.mode(WIFI_OFF);
  btStop();
  Serial.println("WiFi and Bluetooth disabled");
  
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);
  Serial.println("Buttons configured");

  u8g2.begin(); 
  u8g2.setContrast(SCREEN_BRIGHT_LEVEL);
  u8g2.clearBuffer(); 
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(25, 30, "System Init..."); 
  u8g2.sendBuffer();
  Serial.println("OLED display initialized");

  Serial.println("\nInitializing SPI for SD card...");
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(200);

  Serial.println("Initializing SD card...");
  Serial.printf("  CS Pin: %d\n", SD_CS);
  Serial.printf("  SCK Pin: %d\n", SD_SCK);
  Serial.printf("  MISO Pin: %d\n", SD_MISO);
  Serial.printf("  MOSI Pin: %d\n", SD_MOSI);
  
  if (!SD.begin(SD_CS, SPI, SD_SPI_FREQ)) {
    Serial.println("ERROR: SD card initialization FAILED!");
    Serial.println("Check:");
    Serial.println("  - SD card is inserted");
    Serial.println("  - Wiring is correct");
    Serial.println("  - SD card is formatted (FAT32)");
    
    u8g2.clearBuffer(); 
    u8g2.drawStr(20, 30, "SD FAIL!");
    u8g2.sendBuffer(); 
    while(1) delay(1000);
  }
  
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD card initialized successfully - Size: %llu MB\n", cardSize);

  Serial.println("\nInitializing INMP441 I2S microphone...");
  Serial.println("CRITICAL: Verify INMP441 wiring:");
  Serial.println("  L/R  -> GND (for LEFT channel)");
  Serial.println("  WS   -> GPIO 25");
  Serial.println("  SCK  -> GPIO 26");
  Serial.println("  SD   -> GPIO 32");
  Serial.println("  VDD  -> 3.3V");
  Serial.println("  GND  -> GND");
  
  initI2S();
  
  u8g2.clearBuffer(); 
  u8g2.drawStr(35, 30, "Ready!"); 
  u8g2.sendBuffer();
  delay(1000);
  currentMenu = MENU_TEACHER_LIST;
  
  Serial.println("\n================================================");
  Serial.println("System ready! Waiting for user input...");
  Serial.println("Press SELECT to choose a teacher");
  Serial.println("================================================\n");
}

void loop() {
  if (currentMenu == MENU_TEACHER_LIST) {
    drawTeacherList();
    
    if (digitalRead(BTN_UP) == LOW) { 
      if (currentSelection > 0) {
        currentSelection--;
        Serial.printf("Selected: %s\n", TEACHER_NAMES[currentSelection]);
      }
      delay(150); 
    }
    
    if (digitalRead(BTN_DOWN) == LOW) { 
      if (currentSelection < 9) {
        currentSelection++;
        Serial.printf("Selected: %s\n", TEACHER_NAMES[currentSelection]);
      }
      delay(150); 
    }
    
    if (digitalRead(BTN_SEL) == LOW) {
      delay(200); 
      while(digitalRead(BTN_SEL) == LOW) delay(10);
      selectedTeacher = currentSelection;
      Serial.printf("Teacher chosen: %s\n", TEACHER_NAMES[selectedTeacher]);
      currentSelection = 0; 
      currentMenu = MENU_TEACHER_SELECTED;
    }
  }
  else if (currentMenu == MENU_TEACHER_SELECTED) {
    drawTeacherSelected();
    
    if (digitalRead(BTN_UP) == LOW) { 
      currentSelection = 0; 
      delay(150); 
    }
    
    if (digitalRead(BTN_DOWN) == LOW) { 
      currentSelection = 1; 
      delay(150); 
    }
    
    if (digitalRead(BTN_SEL) == LOW) {
      delay(200);
      while(digitalRead(BTN_SEL) == LOW) delay(10);
      
      if (currentSelection == 0) {
        Serial.printf("\nStarting recording for: %s\n", TEACHER_NAMES[selectedTeacher]);
        recordAudio(String(TEACHER_NAMES[selectedTeacher]));
      }
      else { 
        Serial.println("Returning to teacher list");
        currentSelection = selectedTeacher; 
        currentMenu = MENU_TEACHER_LIST; 
      }
    }
  }
}