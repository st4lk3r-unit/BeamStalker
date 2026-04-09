/*
 * bs_fs_none.c - stub bs_fs backend for targets without storage wiring.
 *
 * Activated when neither BS_FS_NATIVE nor BS_FS_SDCARD is defined.
 * This keeps the firmware linkable on display-only / button-only boards
 * while reporting storage as unavailable to the UI and apps.
 */
#if !defined(BS_FS_NATIVE) && !defined(BS_FS_SDCARD)

#include "bs/bs_fs.h"

static const char* s_init_error = "filesystem backend not configured for this target";

int bs_fs_init(void) { return -1; }
bool bs_fs_available(void) { return false; }
const char* bs_fs_init_error(void) { return s_init_error; }
int bs_fs_format(void) { return -1; }

bs_file_t bs_fs_open(const char* path, const char* mode) {
    (void)path; (void)mode;
    return NULL;
}

int bs_fs_read(bs_file_t f, void* buf, size_t len) {
    (void)f; (void)buf; (void)len;
    return -1;
}

int bs_fs_write(bs_file_t f, const void* buf, size_t len) {
    (void)f; (void)buf; (void)len;
    return -1;
}

int bs_fs_seek(bs_file_t f, long offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -1;
}

long bs_fs_tell(bs_file_t f) {
    (void)f;
    return -1;
}

void bs_fs_close(bs_file_t f) {
    (void)f;
}

bool bs_fs_exists(const char* path) {
    (void)path;
    return false;
}

int bs_fs_mkdir_p(const char* path) {
    (void)path;
    return -1;
}

int bs_fs_remove(const char* path) {
    (void)path;
    return -1;
}

long bs_fs_file_size(const char* path) {
    (void)path;
    return -1;
}

int bs_fs_read_file(const char* path, void* buf, size_t max_len, size_t* out_len) {
    (void)path; (void)buf; (void)max_len;
    if (out_len) *out_len = 0;
    return -1;
}

int bs_fs_write_file(const char* path, const void* buf, size_t len) {
    (void)path; (void)buf; (void)len;
    return -1;
}

#endif /* !BS_FS_NATIVE && !BS_FS_SDCARD */
