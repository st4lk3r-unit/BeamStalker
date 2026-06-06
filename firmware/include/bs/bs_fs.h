#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* bs_file_t;

/*
 * Normal bs_fs paths are rooted under BeamStalker's app directory
 * (for example, "wifi/sniff" maps to /BeamStalker/wifi/sniff on SD).
 * Paths starting with BS_FS_RAW_PREFIX bypass that app directory and address
 * the mounted media root directly.  This is intended for tools such as the
 * on-device File Manager that need to browse the whole SD card.
 *
 * Example raw root path: BS_FS_RAW_ROOT -> SD card root.
 */
#define BS_FS_RAW_PREFIX "sd:"
#define BS_FS_RAW_ROOT   "sd:/"

typedef struct {
    char name[64];
    bool is_dir;
    long size;
} bs_dir_entry_t;

typedef int (*bs_fs_list_cb)(const bs_dir_entry_t* ent, void* user);

/* Call once at startup (after arch init). Returns 0 on success, <0 on error. */
int  bs_fs_init(void);

/* Returns true if bs_fs_init() succeeded and the filesystem is usable. */
bool bs_fs_available(void);

/* fopen-style. Returns NULL on failure. mode: "r","w","a","r+","w+" */
bs_file_t bs_fs_open(const char* path, const char* mode);
int       bs_fs_read(bs_file_t f, void* buf, size_t len);
int       bs_fs_write(bs_file_t f, const void* buf, size_t len);
int       bs_fs_seek(bs_file_t f, long offset, int whence);
long      bs_fs_tell(bs_file_t f);
void      bs_fs_close(bs_file_t f);

bool bs_fs_exists(const char* path);
bool bs_fs_is_dir(const char* path);
int  bs_fs_mkdir_p(const char* path);   /* creates all intermediate dirs */
int  bs_fs_remove(const char* path);
int  bs_fs_rename(const char* old_path, const char* new_path);
int  bs_fs_list_dir(const char* path, bs_fs_list_cb cb, void* user);

/* Returns file size in bytes, or -1 if file does not exist / error. */
long bs_fs_file_size(const char* path);

/* Returns a static string describing the last bs_fs_init() failure, or NULL. */
const char* bs_fs_init_error(void);

/*
 * Format the SD card as FAT32 and reinitialise the filesystem.
 * DESTRUCTIVE - erases all card data.
 * Uses SD.begin(format_if_empty=true) which triggers ESP-IDF's FAT formatter
 * when the existing filesystem cannot be mounted (e.g. NTFS, exFAT).
 * Returns 0 on success, -1 on failure.
 */
int bs_fs_format(void);

/* Convenience: read/write entire file. Returns bytes read/written or <0 */
int bs_fs_read_file(const char* path, void* buf, size_t max_len, size_t* out_len);
int bs_fs_write_file(const char* path, const void* buf, size_t len);

#ifdef __cplusplus
}
#endif
