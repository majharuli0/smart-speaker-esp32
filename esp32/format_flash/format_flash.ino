// One-time utility: formats the W25Q64JV SPI flash chip as a FAT
// filesystem so esp32.ino can mount and use it for sound storage.
//
// You only need to run this ONCE per physical flash chip (or again if you
// swap in a different/blank one). It is *not* part of the normal
// build/flash cycle for esp32.ino.
//
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// !!  NOTE: THIS ERASES ALL DATA ON THE FLASH CHIP!            !!
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
//
// Usage:
// - Upload this sketch (board: ESP32 Dev Module, same port as esp32.ino).
// - Open Serial Monitor at 115200 baud.
// - Type OK (all caps) and press enter when prompted.
// - Takes ~30-60 seconds. Prints a success message when done.
// - Then re-open and re-upload esp32.ino as normal.
//
// IMPORTANT — a required library patch: Adafruit_SPIFlash has a known bug
// on ESP32 (https://github.com/adafruit/Adafruit_SPIFlash/issues/120) where
// Adafruit_SPIFlashBase::begin() unconditionally force-casts the transport
// to Adafruit_FlashTransport_ESP32* (meant only for the ESP32's own
// *internal* flash) even when you're using Adafruit_FlashTransport_SPI (an
// external chip, our case) — crashing with a LoadProhibited panic. Fixed by
// removing `defined(ARDUINO_ARCH_ESP32) ||` from the #if in the installed
// library's src/Adafruit_SPIFlashBase.cpp, so ESP32 falls through to the
// same generic JEDEC-ID auto-detect path other platforms use (it already
// recognizes W25Q64JV_IQ). That fix lives in the library's install
// location, not here — if you ever reinstall/update "Adafruit SPIFlash" via
// Library Manager, this sketch (and esp32.ino) will crash again the same
// way until the patch is reapplied.
//
// Adapted from Adafruit_SPIFlash's bundled "SdFat_format" example.

#include "SdFat_Adafruit_Fork.h"
#include <Adafruit_SPIFlash.h>
#include <SPI.h>

// Since SdFat doesn't fully support FAT12 such as format a new flash
// We will use Elm Cham's fatfs f_mkfs() to format
#include "ff.h"
#include "diskio.h"

// up to 11 characters
#define DISK_LABEL "EXT FLASH"

// Same SPI flash wiring as esp32.ino: CS on GPIO 5, hardware VSPI bus
// (default VSPI pins SCK=18, MISO=19, MOSI=23 match the physical wiring).
#define FLASH_CS_GPIO 5

Adafruit_FlashTransport_SPI flashTransport(FLASH_CS_GPIO, &SPI);

Adafruit_SPIFlash flash(&flashTransport);
FatVolume fatfs;

void format_fat12(void) {
// Working buffer for f_mkfs.
#ifdef __AVR__
  uint8_t workbuf[512];
#else
  uint8_t workbuf[4096];
#endif

  // Elm Cham's fatfs objects
  FATFS elmchamFatfs;

  // Make filesystem.
  FRESULT r = f_mkfs("", FM_FAT, 0, workbuf, sizeof(workbuf));
  if (r != FR_OK) {
    Serial.print(F("Error, f_mkfs failed with error code: "));
    Serial.println(r, DEC);
    while (1)
      yield();
  }

  // mount to set disk label
  r = f_mount(&elmchamFatfs, "0:", 1);
  if (r != FR_OK) {
    Serial.print(F("Error, f_mount failed with error code: "));
    Serial.println(r, DEC);
    while (1)
      yield();
  }

  // Setting label
  Serial.println(F("Setting disk label to: " DISK_LABEL));
  r = f_setlabel(DISK_LABEL);
  if (r != FR_OK) {
    Serial.print(F("Error, f_setlabel failed with error code: "));
    Serial.println(r, DEC);
    while (1)
      yield();
  }

  // unmount
  f_unmount("0:");

  // sync to make sure all data is written to flash
  flash.syncBlocks();

  Serial.println(F("Formatted flash!"));
}

void check_fat12(void) {
  // Check new filesystem
  if (!fatfs.begin(&flash)) {
    Serial.println(F("Error, failed to mount newly formatted filesystem!"));
    while (1)
      delay(1);
  }
}

void setup() {
  // Initialize serial port and wait for it to open before continuing.
  Serial.begin(115200);
  while (!Serial)
    delay(100);

  Serial.println(F("Adafruit SPI Flash FatFs Format Example"));

  // Same SPI init esp32.ino does before touching the flash chip.
  SPI.begin();

  // Initialize flash library and check its chip ID.
  if (!flash.begin()) {
    Serial.println(F("Error, failed to initialize flash chip!"));
    while (1)
      yield();
  }

  Serial.print(F("Flash chip JEDEC ID: 0x"));
  Serial.println(flash.getJEDECID(), HEX);
  Serial.print(F("Flash size: "));
  Serial.print(flash.size() / 1024);
  Serial.println(F(" KB"));

  // Uncomment to flash LED while writing to flash
  // flash.setIndicator(LED_BUILTIN, true);

  // Wait for user to send OK to continue.
  Serial.setTimeout(
      30000); // Increase timeout to print message less frequently.
  do {
    Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
                     "!!!!!!!!!!!!"));
    Serial.println(F("This sketch will ERASE ALL DATA on the flash chip and "
                     "format it with a new filesystem!"));
    Serial.println(F("Type OK (all caps) and press enter to continue."));
    Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
                     "!!!!!!!!!!!!"));
  } while (!Serial.find((char *)"OK"));

  // Call fatfs begin and passed flash object to initialize file system
  Serial.println(
      F("Creating and formatting FAT filesystem (this takes ~60 seconds)..."));

  format_fat12();

  check_fat12();

  // Done!
  Serial.println(
      F("Flash chip successfully formatted with new empty filesystem!"));
}

void loop() {
  // Nothing to be done in the main loop.
}

//--------------------------------------------------------------------+
// fatfs diskio
//--------------------------------------------------------------------+
extern "C" {

DSTATUS disk_status(BYTE pdrv) {
  (void)pdrv;
  return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
  (void)pdrv;
  return 0;
}

DRESULT disk_read(BYTE pdrv,  /* Physical drive nmuber to identify the drive */
                  BYTE *buff, /* Data buffer to store read data */
                  DWORD sector, /* Start sector in LBA */
                  UINT count    /* Number of sectors to read */
) {
  (void)pdrv;
  return flash.readBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, /* Physical drive nmuber to identify the drive */
                   const BYTE *buff, /* Data to be written */
                   DWORD sector,     /* Start sector in LBA */
                   UINT count        /* Number of sectors to write */
) {
  (void)pdrv;
  return flash.writeBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, /* Physical drive nmuber (0..) */
                   BYTE cmd,  /* Control code */
                   void *buff /* Buffer to send/receive control data */
) {
  (void)pdrv;

  switch (cmd) {
  case CTRL_SYNC:
    flash.syncBlocks();
    return RES_OK;

  case GET_SECTOR_COUNT:
    *((DWORD *)buff) = flash.size() / 512;
    return RES_OK;

  case GET_SECTOR_SIZE:
    *((WORD *)buff) = 512;
    return RES_OK;

  case GET_BLOCK_SIZE:
    *((DWORD *)buff) = 8; // erase block size in units of sector size
    return RES_OK;

  default:
    return RES_PARERR;
  }
}
}
