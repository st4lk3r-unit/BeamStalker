/*
 * bs_fs_sdcard.c - bs_fs backend for Arduino SD card.
 *
 * Stores normal BeamStalker files under /BeamStalker/ on the SD card.
 * Paths beginning with BS_FS_RAW_PREFIX ("sd:") address the SD root directly.
 */
#ifdef BS_FS_SDCARD
#include "bs/bs_fs.h"
#include "bs/bs_board.h"
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

static bool is_raw_path(const char* path) {
    return path && strncmp(path, BS_FS_RAW_PREFIX, strlen(BS_FS_RAW_PREFIX)) == 0;
}

static void raw_full_path(char* dst, size_t sz, const char* path) {
    const char* p = path ? path + strlen(BS_FS_RAW_PREFIX) : "/";
    if (!p || !p[0]) p = "/";
    if (p[0] == '/') snprintf(dst, sz, "%s", p);
    else snprintf(dst, sz, "/%s", p);
}

static void app_full_path(char* dst, size_t sz, const char* path) {
    if (path && path[0] == '/') snprintf(dst, sz, BS_FS_ROOT "%s", path);
    else snprintf(dst, sz, BS_FS_ROOT "/%s", path ? path : "");
}

static void full_path(char* dst, size_t sz, const char* path) {
    if (is_raw_path(path)) raw_full_path(dst, sz, path);
    else app_full_path(dst, sz, path);
}

static int mkdir_p_full(const char* full) {
    if (!full || !full[0]) return -1;

    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", full);

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    if (!strcmp(tmp, "/")) return 0;

    for (char* p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (tmp[0] && !SD.exists(tmp)) {
            if (!SD.mkdir(tmp) && !SD.exists(tmp)) {
                *p = '/';
                return -1;
            }
        }
        *p = '/';
    }

    if (!SD.exists(tmp)) {
        if (!SD.mkdir(tmp) && !SD.exists(tmp)) return -1;
    }
    return 0;
}

static void ensure_parent_dir_full(const char* full) {
    if (!full || !full[0]) return;

    char dir[256];
    snprintf(dir, sizeof dir, "%s", full);

    char* slash = strrchr(dir, '/');
    if (!slash) return;
    if (slash == dir) return;   /* parent is SD root */

    *slash = '\0';
    mkdir_p_full(dir);
}

static bool mode_writes(const char* mode) {
    return mode && (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+') != NULL);
}

static bool        s_available   = false;
static const char* s_init_error  = NULL;

int bs_fs_init(void) {
    /* Let the board layer deselect any shared SPI peers before SD.begin(). */
    bs_board_prepare_fs_mount();

    /* Check board-specific SD detect before attempting SPI init. */
    {
        int det = bs_board_sd_detect();
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

    bs_board_prepare_fs_mount();
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
    if (mode && mode[0] == 'w') sdmode = "w";
    else if (mode && mode[0] == 'a') sdmode = "a";
    else if (mode && mode[0] == 'r' && mode[1] == '+') sdmode = "r+";

    /* Arduino SD.open() will not create missing parent directories.  Make
     * write/append paths robust so callers can open nested files such as
     * wifi/sniff/sniff-<seq>-<timestamp>.pcap after a fresh format/card. */
    if (mode_writes(mode)) ensure_parent_dir_full(fp);

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

bool bs_fs_is_dir(const char* path) {
    char fp[256]; full_path(fp, sizeof fp, path);
    File f = SD.open(fp, "r");
    if (!f) return false;
    bool is_dir = f.isDirectory();
    f.close();
    return is_dir;
}

int bs_fs_mkdir_p(const char* path) {
    char fp[256]; full_path(fp, sizeof fp, path);
    return mkdir_p_full(fp);
}

static bool is_protected_remove_target(const char* full) {
    return !full || !full[0] || strcmp(full, "/") == 0;
}

static int remove_full_recursive(const char* full) {
    if (is_protected_remove_target(full)) return -1;

    File f = SD.open(full, "r");
    if (!f) {
        return SD.remove(full) ? 0 : -1;
    }

    bool is_dir = f.isDirectory();
    if (!is_dir) {
        f.close();
        return SD.remove(full) ? 0 : -1;
    }

    for (;;) {
        File child = f.openNextFile();
        if (!child) break;

        char child_path[256];
        const char* child_name = child.name();
        if (child_name && child_name[0] == '/') {
            snprintf(child_path, sizeof child_path, "%s", child_name);
        } else {
            snprintf(child_path, sizeof child_path, "%s/%s", full, child_name ? child_name : "");
        }
        child.close();

        if (child_path[0] == '\0' || remove_full_recursive(child_path) != 0) {
            f.close();
            return -1;
        }
    }

    f.close();
    return SD.rmdir(full) ? 0 : -1;
}

int bs_fs_remove(const char* path) {
    char fp[256]; full_path(fp, sizeof fp, path);
    return remove_full_recursive(fp);
}

int bs_fs_rename(const char* old_path, const char* new_path) {
    char old_fp[256], new_fp[256];
    full_path(old_fp, sizeof old_fp, old_path);
    full_path(new_fp, sizeof new_fp, new_path);
    return SD.rename(old_fp, new_fp) ? 0 : -1;
}

int bs_fs_list_dir(const char* path, bs_fs_list_cb cb, void* user) {
    if (!cb) return -1;
    char fp[256]; full_path(fp, sizeof fp, path);
    File dir = SD.open(fp, "r");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return -1; }

    File f = dir.openNextFile();
    while (f) {
        bs_dir_entry_t ent;
        memset(&ent, 0, sizeof ent);
        const char* nm = f.name();
        const char* slash = strrchr(nm, '/');
        if (slash) nm = slash + 1;
        snprintf(ent.name, sizeof ent.name, "%s", nm ? nm : "?");
        ent.is_dir = f.isDirectory();
        ent.size = ent.is_dir ? 0 : (long)f.size();
        f.close();
        if (ent.name[0] != '\0' && cb(&ent, user) != 0) {
            dir.close();
            return 1;
        }
        f = dir.openNextFile();
    }
    dir.close();
    return 0;
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
