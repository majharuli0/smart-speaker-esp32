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
// diskio_impl.h (not diskio.h) -- this ESP32 core's bundled FatFs is
// ESP-IDF's registration-based port: the classic global disk_read()/
// disk_write()/etc. names are the *generic dispatcher* already compiled
// into libfatfs.a for every ESP32 sketch, not free for us to redefine
// (that's the "multiple definition of `ff_disk_read`" link error).
// Instead we register our own read/write/etc. callbacks under a chosen
// drive number via ff_diskio_register(), which the dispatcher then calls
// through -- diskio_impl.h declares that API and pulls in diskio.h itself.
#include "diskio_impl.h"

// up to 11 characters
#define DISK_LABEL "EXT FLASH"

// Arbitrary FatFs drive number for our external SPI flash chip -- "0:" in
// the f_mount/f_mkfs calls below refers to whichever pdrv is registered
// here.
#define EXT_FLASH_PDRV 0

// Same SPI flash wiring as esp32.ino: CS on GPIO 5, hardware VSPI bus
// (default VSPI pins SCK=18, MISO=19, MOSI=23 match the physical wiring).
#define FLASH_CS_GPIO 5

// 1 = loop forever printing the raw JEDEC ID for live wiring troubleshooting
// (see setup()); 0 = normal one-time format behavior. Flip to 0 once
// wiring is confirmed good, then re-upload to actually format the chip.
#define WIRING_DIAGNOSTIC_MODE 1

// 1 = loop forever writing/reading a test pattern to one sector (far from
// any real filesystem data) and reporting match/mismatch -- exercises the
// real bulk read/write path fast and repeatably, without needing a full
// erase+format cycle each time. Only one of this and
// WIRING_DIAGNOSTIC_MODE should be 1 at a time.
#define READ_WRITE_TEST_MODE 1

Adafruit_FlashTransport_SPI flashTransport(FLASH_CS_GPIO, &SPI);

Adafruit_SPIFlash flash(&flashTransport);
FatVolume fatfs;

// Forward declarations for the diskio callbacks (defined near the bottom
// of this file, after setup()/loop()). Arduino's IDE normally auto-generates
// these, but its parser is unreliable with multi-line signatures like these
// -- declaring them explicitly avoids depending on that.
extern "C" {
DSTATUS extFlashDiskInitialize(BYTE pdrv);
DSTATUS extFlashDiskStatus(BYTE pdrv);
DRESULT extFlashDiskRead(BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
DRESULT extFlashDiskWrite(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
DRESULT extFlashDiskIoctl(BYTE pdrv, BYTE cmd, void *buff);
}

// Bigger stack for the main Arduino task -- formatting an 8MB volume with
// f_mkfs() uses more stack (ours + FatFs's own internal frames) than the
// default budget allows, and overflowing it panics with "Stack canary
// watchpoint triggered" rather than a clean error. This must be at file
// scope (not inside a function).
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void format_fat12(void) {
// Working buffer for f_mkfs. static: keeps this 4KB (plus the FATFS struct
// below) off the stack entirely, rather than just relying on the bigger
// stack above -- belt and suspenders, since f_mkfs()'s own internal stack
// usage for an 8MB volume isn't something we control.
#ifdef __AVR__
  static uint8_t workbuf[512];
#else
  static uint8_t workbuf[4096];
#endif

  // Elm Cham's fatfs objects
  static FATFS elmchamFatfs;

  // Full chip erase first: earlier format attempts on this chip got
  // interrupted mid-operation by crashes (stack overflow, a stuck
  // write-in-progress status bit), which can leave sectors partially
  // written/erased in a way f_mkfs()'s optimizations don't expect even
  // though f_mkfs() itself reports success. Erasing everything back to a
  // known-good blank (all 0xFF) state removes that as a variable. Can take
  // up to a couple minutes on an 8MB chip -- this call blocks until done.
  Serial.println(F("Erasing entire chip first (can take up to ~2 minutes)..."));
  if (!flash.eraseChip()) {
    Serial.println(F("Error, chip erase failed!"));
    while (1)
      yield();
  }
  flash.waitUntilReady();
  Serial.println(F("Erase complete."));

  // f_mkfs()'s signature changed in the FatFs version bundled with newer
  // ESP32 cores: it now takes an MKFS_PARM struct instead of separate
  // fmt/au_size arguments (Adafruit's original example predates this).
  // Zero-value fields mean "auto-select", per FatFs's own convention.
  //
  // FM_SFD ("Super Floppy Disk") skips the MBR/partition-table layout
  // entirely, putting the FAT boot sector directly at sector 0 -- the
  // right choice for a single small flash chip like this (partition
  // tables are a hard-disk convention). Without it, f_mkfs() built an MBR
  // at sector 0 pointing to a partition starting at sector 63, and
  // f_mount() couldn't validate that round-trip even though every
  // individual read/write succeeded (confirmed via per-call diskio logs).
  MKFS_PARM opt = {};
  opt.fmt = FM_FAT | FM_SFD;

  // Make filesystem.
  FRESULT r = f_mkfs("", &opt, workbuf, sizeof(workbuf));
  if (r != FR_OK) {
    Serial.print(F("Error, f_mkfs failed with error code: "));
    Serial.println(r, DEC);
    while (1)
      yield();
  }

  // Adafruit_SPIFlash caches writes in RAM per erase-block and only
  // physically commits them on syncBlocks() -- without this, the mount
  // right below can fail with FR_NO_FILESYSTEM because what f_mkfs() just
  // wrote hasn't actually reached the chip yet.
  flash.syncBlocks();

  // Diagnostic: inspect the actual bytes physically read back from sector
  // 0, independent of f_mount()'s validation logic. A real FAT boot sector
  // ends with signature 0x55 0xAA at offset 510-511; the very first bytes
  // are normally a jump instruction (0xEB or 0xE9) followed by an OEM name.
  {
    static uint8_t sector0[512];
    bool ok = flash.readBlocks(0, sector0, 1);
    Serial.printf("Sector 0 read: %s\n", ok ? "OK" : "FAIL");
    Serial.print(F("First 16 bytes: "));
    for (int i = 0; i < 16; i++) Serial.printf("%02X ", sector0[i]);
    Serial.println();
    Serial.printf("Boot signature (should be 55 AA): %02X %02X\n", sector0[510], sector0[511]);
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

#if WIRING_DIAGNOSTIC_MODE
  // Live wiring check: keeps reading and printing the raw JEDEC ID forever
  // instead of just once, so you can physically wiggle/reseat each wire
  // (VCC, GND, CLK, DI, DO, CS) one at a time WHILE WATCHING Serial Monitor
  // for a change. All 0x00 or all 0xFF means nothing's really connected on
  // that read. A real Winbond chip's ID starts with manufacturer byte
  // 0xEF and should settle on the SAME 3 distinct-looking bytes every
  // time once the connection is solid. Set WIRING_DIAGNOSTIC_MODE to 0
  // below and re-upload once you've confirmed a stable, believable ID.
  while (true) {
    uint8_t jedec[4] = {0, 0, 0, 0};
    flashTransport.begin();
    flashTransport.readCommand(0x9F /* JEDEC READ ID, standard across SPI NOR flash */, jedec, 4);
    Serial.printf("Raw JEDEC ID bytes: %02X %02X %02X %02X\n", jedec[0], jedec[1], jedec[2], jedec[3]);
    delay(500);
  }
#endif

  // Initialize flash library and check its chip ID.
  if (!flash.begin()) {
    Serial.println(F("Error, failed to initialize flash chip!"));
    while (1)
      yield();
  }

  // flash.begin() auto-speeds the SPI clock up to this chip's rated
  // maximum (133MHz for the W25Q64JV) -- fine on a proper PCB, but far
  // beyond what breadboard jumper wires can reliably carry. Every bulk
  // read/write after begin() was silently returning garbage (all zeros)
  // at that speed, even though it reported "OK" -- confirmed by tracing
  // the cache's actual buffer contents. The earlier raw JEDEC ID reads
  // worked fine only because those happened before begin() sped things up
  // (still at the transport's conservative default, 4MHz). Forcing it
  // back down to a speed breadboard wiring can sustain reliably.
  flashTransport.setClockSpeed(1000000, 1000000);

  Serial.print(F("Flash chip JEDEC ID: 0x"));
  Serial.println(flash.getJEDECID(), HEX);
  Serial.print(F("Flash size: "));
  Serial.print(flash.size() / 1024);
  Serial.println(F(" KB"));

#if READ_WRITE_TEST_MODE
  // Fast, repeatable read/write reliability test -- exercises the exact
  // same bulk read/write path as real formatting (through the cache, at
  // the real 1MHz operating speed), but on ONE sector far from any
  // filesystem structures, so it's safe to run repeatedly without
  // reformatting anything. Loops forever so you can wiggle wires and
  // watch for consistency, same as the wiring diagnostic above.
  {
    const uint32_t TEST_SECTOR = 2000; // arbitrary, well clear of anything f_mkfs touches
    uint32_t pass = 0, fail = 0;
    while (true) {
      uint8_t writeBuf[512];
      uint8_t readBuf[512];
      uint8_t pattern = (uint8_t)(millis() & 0xFF); // changes each iteration
      for (int i = 0; i < 512; i++) writeBuf[i] = (uint8_t)(pattern + i);

      flash.eraseSector(TEST_SECTOR * 512 / 4096);
      // Read status IMMEDIATELY (before waitUntilReady would block/clear
      // it) -- a real erase takes tens of ms, so WIP (bit 0) should still
      // read as SET right now. If it's already 0, the erase command was
      // never actually accepted by the chip (e.g. Write Enable didn't
      // really take), and eraseCommand()'s "success" was meaningless --
      // it only confirms the SPI bytes were sent, not that the chip acted
      // on them.
      uint8_t statusRightAfterErase = flash.readStatus();
      Serial.printf("[rw-test] status right after eraseSector(): 0x%02X (WIP bit %s)\n",
                    statusRightAfterErase, (statusRightAfterErase & 0x01) ? "SET - erase really in progress" : "CLEAR - erase was likely ignored");
      flash.waitUntilReady();
      bool wOk = flash.writeBlocks(TEST_SECTOR, writeBuf, 1);
      flash.syncBlocks();
      bool rOk = flash.readBlocks(TEST_SECTOR, readBuf, 1);
      bool match = (memcmp(writeBuf, readBuf, 512) == 0);

      if (match) pass++; else fail++;
      Serial.printf("[rw-test] pattern=0x%02X write=%s read=%s match=%s  (pass=%lu fail=%lu)\n",
                    pattern, wOk ? "OK" : "FAIL", rOk ? "OK" : "FAIL", match ? "YES" : "NO",
                    (unsigned long)pass, (unsigned long)fail);
      if (!match) {
        Serial.print(F("  wrote: "));
        for (int i = 0; i < 16; i++) Serial.printf("%02X ", writeBuf[i]);
        Serial.println();
        Serial.print(F("  read:  "));
        for (int i = 0; i < 16; i++) Serial.printf("%02X ", readBuf[i]);
        Serial.println();
      }
      delay(500);
    }
  }
#endif

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

  // Hook our flash chip's read/write/etc. into FatFs's dispatcher under
  // drive number EXT_FLASH_PDRV -- required before f_mkfs()/f_mount() can
  // talk to it (see the diskio_impl.h comment above).
  ff_diskio_impl_t diskioImpl = {
    .init = extFlashDiskInitialize,
    .status = extFlashDiskStatus,
    .read = extFlashDiskRead,
    .write = extFlashDiskWrite,
    .ioctl = extFlashDiskIoctl,
  };
  ff_diskio_register(EXT_FLASH_PDRV, &diskioImpl);

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
// fatfs diskio -- registered with ff_diskio_register() in setup(), not
// picked up by fixed name (see the diskio_impl.h comment near the top).
// Named distinctly from disk_read/disk_write/etc. on purpose: those names
// are the generic dispatcher already compiled into every ESP32 sketch via
// libfatfs.a: redefining them causes a "multiple definition" link error.
//--------------------------------------------------------------------+
extern "C" {

DSTATUS extFlashDiskInitialize(BYTE pdrv) {
  Serial.printf("[diskio] init pdrv=%u\n", pdrv);
  return 0;
}

DSTATUS extFlashDiskStatus(BYTE pdrv) {
  Serial.printf("[diskio] status pdrv=%u\n", pdrv);
  return 0;
}

DRESULT extFlashDiskRead(BYTE pdrv,  /* Physical drive nmuber to identify the drive */
                         BYTE *buff, /* Data buffer to store read data */
                         DWORD sector, /* Start sector in LBA */
                         UINT count    /* Number of sectors to read */
) {
  (void)pdrv;
  bool ok = flash.readBlocks(sector, buff, count);
  Serial.printf("[diskio] read  sector=%lu count=%u -> %s\n", (unsigned long)sector, count, ok ? "OK" : "FAIL");
  return ok ? RES_OK : RES_ERROR;
}

DRESULT extFlashDiskWrite(BYTE pdrv, /* Physical drive nmuber to identify the drive */
                          const BYTE *buff, /* Data to be written */
                          DWORD sector,     /* Start sector in LBA */
                          UINT count        /* Number of sectors to write */
) {
  (void)pdrv;
  if (sector == 0) {
    Serial.print(F("[diskio] write sector=0, buff's first 16 bytes as given to us: "));
    for (int i = 0; i < 16; i++) Serial.printf("%02X ", buff[i]);
    Serial.println();
  }
  bool ok = flash.writeBlocks(sector, buff, count);
  Serial.printf("[diskio] write sector=%lu count=%u -> %s\n", (unsigned long)sector, count, ok ? "OK" : "FAIL");
  return ok ? RES_OK : RES_ERROR;
}

DRESULT extFlashDiskIoctl(BYTE pdrv, /* Physical drive nmuber (0..) */
                          BYTE cmd,  /* Control code */
                          void *buff /* Buffer to send/receive control data */
) {
  (void)pdrv;
  Serial.printf("[diskio] ioctl cmd=%u\n", cmd);

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
