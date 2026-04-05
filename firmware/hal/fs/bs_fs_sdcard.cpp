/*
 * bs_fs_sdcard.c - bs_fs backend for Arduino SD card.
 *
 * Stores files under /BeamStalker/ on the SD card.
 */
#ifdef BS_FS_SDCARD
#include "bs/bs_fs.h"
#include "bs/bs_hw.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <string.h>
#include <stdio.h>

/*
 * When the SD card uses different SPI pins from the display (e.g. Cardputer:
 * display=SCK36/MOSI35, SD=SCK40/MOSI14) we must use a dedicated SPIClass
 * instance so the two buses don't interfere.  On targets where display and SD
 * share the same physical SPI bus (e.g. T-Pager SCK35/MOSI34 for both) the
 * global SPI object is reused as before.
 */
#if defined(BS_SD_SCK_PIN) && defined(SGFX_PIN_SCK) && (BS_SD_SCK_PIN != SGFX_PIN_SCK)
#  define BS_SD_USE_DEDICATED_SPI 1
static SPIClass s_sd_spi(HSPI);
static bool     s_sd_spi_begun = false;
#  define SD_BUS s_sd_spi
#else
#  define SD_BUS SPI
#endif

#ifndef BS_SD_CS_PIN
#  define BS_SD_CS_PIN 10
#endif
#ifndef BS_SD_FREQ
#  define BS_SD_FREQ 20000000UL
#endif

#define BS_FS_ROOT "/BeamStalker"

static void full_path(char* dst, size_t sz, const char* path) {
    if (path && path[0] == '/') snprintf(dst, sz, BS_FS_ROOT "%s", path);
    else snprintf(dst, sz, BS_FS_ROOT "/%s", path ? path : "");
}

static bool        s_available   = false;
static const char* s_init_error  = NULL;

int bs_fs_init(void) {
    /*
     * SPI bus is already initialised by SGFX (SGFX_PIN_MISO=33 is set).
     * Do NOT call SPI.end() - that leaves the display CS pin (SGFX_PIN_CS=38)
     * in an undefined / LOW state, which selects the ST7796 simultaneously
     * and causes SPI bus contention during SD card init.
     *
     * Instead: explicitly pull the display CS HIGH so the display is deselected,
     * then talk to the SD card on the already-configured SPI bus.
     */
#if defined(SGFX_PIN_CS) && SGFX_PIN_CS >= 0
    pinMode(SGFX_PIN_CS, OUTPUT);
    digitalWrite(SGFX_PIN_CS, HIGH);
#endif
    /* Deselect ALL SPI bus peers before touching the bus.
     * NFC and LoRa CS pins float or default LOW at boot, which selects those
     * chips simultaneously and causes SPI bus contention → sdSelectCard fails. */
#if defined(BS_LORA_CS_PIN) && BS_LORA_CS_PIN >= 0
    pinMode(BS_LORA_CS_PIN, OUTPUT);
    digitalWrite(BS_LORA_CS_PIN, HIGH);
#endif
#if defined(BS_NFC_CS_PIN) && BS_NFC_CS_PIN >= 0
    pinMode(BS_NFC_CS_PIN, OUTPUT);
    digitalWrite(BS_NFC_CS_PIN, HIGH);
#endif

    delay(50);   /* short settle after display CS deselect */

    /* Check SD card detect pin (XL9555 P12) before attempting SPI init */
    {
        int det = bs_hw_sd_detect();
        if (det == 0) {
            s_init_error = "no card detected (SD_DET/P12 high)";
            return -1;
        }
        /* det == -1 means XL9555 not readable; proceed anyway and let SD.begin() fail */
    }

#ifdef BS_SD_USE_DEDICATED_SPI
    if (!s_sd_spi_begun) {
        s_sd_spi.begin(BS_SD_SCK_PIN,
                       (int)BS_SD_MISO_PIN,
                       BS_SD_MOSI_PIN,
                       BS_SD_CS_PIN);
        s_sd_spi_begun = true;
    }
#endif
    if (!SD.begin(BS_SD_CS_PIN, SD_BUS, BS_SD_FREQ)) {
        /* Fallback: try at a lower, more compatible speed */
        if (!SD.begin(BS_SD_CS_PIN, SD_BUS, 4000000)) {
            s_init_error = "SD.begin() failed (tried 20 MHz + 4 MHz)";
            return -1;
        }
    }
    if (!SD.exists(BS_FS_ROOT)) SD.mkdir(BS_FS_ROOT);
    s_available = true;
    return 0;
}

const char* bs_fs_init_error(void) { return s_init_error; }

int bs_fs_format(void) {
    SD.end();
    s_available  = false;
    s_init_error = NULL;

#if defined(SGFX_PIN_CS) && SGFX_PIN_CS >= 0
    pinMode(SGFX_PIN_CS, OUTPUT);
    digitalWrite(SGFX_PIN_CS, HIGH);
#endif
    delay(200);

    /*
     * format_if_empty=true  →  ESP-IDF formats as FAT32 when mount fails.
     * Use 4 MHz for the format pass (more compatible with unconfigured cards).
     * mountpoint="/sd", max_files=5 are the Arduino SD library defaults.
     */
    if (!SD.begin(BS_SD_CS_PIN, SD_BUS, 4000000, "/sd", 5, /*format_if_empty=*/true)) {
        s_init_error = "format+mount failed";
        return -1;
    }
    if (!SD.exists(BS_FS_ROOT)) SD.mkdir(BS_FS_ROOT);
    s_available  = true;
    s_init_error = NULL;
    return 0;
}

bool bs_fs_available(void) { return s_available; }

bs_file_t bs_fs_open(const char* path, const char* mode) {
    char fp[256]; full_path(fp, sizeof fp, path);
    const char* sdmode = "r";
    if (mode[0] == 'w') sdmode = "w";
    else if (mode[0] == 'a') sdmode = "a";
    else if (mode[0] == 'r' && mode[1] == '+') sdmode = "r+";
    File f = SD.open(fp, sdmode);
    if (!f) return NULL;
    File* fh = new File(f);
    return (bs_file_t)fh;
}

int bs_fs_read(bs_file_t f, void* buf, size_t len) {
    if (!f) return -1;
    return ((File*)f)->read((uint8_t*)buf, len);
}

int bs_fs_write(bs_file_t f, const void* buf, size_t len) {
    if (!f) return -1;
    return ((File*)f)->write((const uint8_t*)buf, len);
}

int bs_fs_seek(bs_file_t f, long offset, int whence) {
    if (!f) return -1;
    // SD.h only supports seek from start
    (void)whence;
    return ((File*)f)->seek((uint32_t)offset) ? 0 : -1;
}

long bs_fs_tell(bs_file_t f) {
    if (!f) return -1;
    return (long)((File*)f)->position();
}

void bs_fs_close(bs_file_t f) {
    if (!f) return;
    ((File*)f)->close();
    delete (File*)f;
}

bool bs_fs_exists(const char* path) {
    char fp[256]; full_path(fp, sizeof fp, path);
    return SD.exists(fp);
}

int bs_fs_mkdir_p(const char* path) {
    char fp[256]; full_path(fp, sizeof fp, path);
    return SD.mkdir(fp) ? 0 : -1;
}

int bs_fs_remove(const char* path) {
    char fp[256]; full_path(fp, sizeof fp, path);
    return SD.remove(fp) ? 0 : -1;
}

long bs_fs_file_size(const char* path) {
    char fp[256]; full_path(fp, sizeof fp, path);
    File f = SD.open(fp, "r");
    if (!f) return -1;
    long sz = (long)f.size();
    f.close();
    return sz;
}

int bs_fs_read_file(const char* path, void* buf, size_t max_len, size_t* out_len) {
    bs_file_t f = bs_fs_open(path, "r");
    if (!f) return -1;
    int n = bs_fs_read(f, buf, max_len);
    bs_fs_close(f);
    if (out_len) *out_len = (n > 0) ? (size_t)n : 0;
    return n;
}

int bs_fs_write_file(const char* path, const void* buf, size_t len) {
    bs_fs_mkdir_p(".");  // ensure root exists
    bs_file_t f = bs_fs_open(path, "w");
    if (!f) return -1;
    int n = bs_fs_write(f, buf, len);
    bs_fs_close(f);
    return n;
}

#endif /* BS_FS_SDCARD */
