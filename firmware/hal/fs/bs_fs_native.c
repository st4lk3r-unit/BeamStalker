/*
 * bs_fs_native.c - bs_fs backend for native Linux.
 *
 * Stores files under ./BeamStalker/ relative to the working directory
 * where the binary is launched.
 */
#ifdef BS_FS_NATIVE

#include "bs/bs_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#define BS_FS_ROOT "./BeamStalker"

/* ---- Path helper ------------------------------------------------------- */

static void full_path(char* dst, size_t sz, const char* path) {
    /* Strip leading '/' from path before appending */
    if (path && path[0] == '/') path++;
    if (path && path[0] != '\0')
        snprintf(dst, sz, BS_FS_ROOT "/%s", path);
    else
        snprintf(dst, sz, "%s", BS_FS_ROOT);
}

/* ---- Init -------------------------------------------------------------- */

static bool s_available = false;

int bs_fs_init(void) {
    if (mkdir(BS_FS_ROOT, 0755) < 0 && errno != EEXIST) return -1;
    s_available = true;
    return 0;
}

bool bs_fs_available(void) { return s_available; }
const char* bs_fs_init_error(void) { return NULL; }
int bs_fs_format(void) { return -1; }  /* not applicable on native */

/* ---- File I/O ---------------------------------------------------------- */

bs_file_t bs_fs_open(const char* path, const char* mode) {
    char fp[512];
    full_path(fp, sizeof fp, path);
    FILE* f = fopen(fp, mode);
    return (bs_file_t)f;
}

int bs_fs_read(bs_file_t f, void* buf, size_t len) {
    if (!f) return -1;
    return (int)fread(buf, 1, len, (FILE*)f);
}

int bs_fs_write(bs_file_t f, const void* buf, size_t len) {
    if (!f) return -1;
    return (int)fwrite(buf, 1, len, (FILE*)f);
}

int bs_fs_seek(bs_file_t f, long offset, int whence) {
    if (!f) return -1;
    return fseek((FILE*)f, offset, whence);
}

long bs_fs_tell(bs_file_t f) {
    if (!f) return -1;
    return ftell((FILE*)f);
}

void bs_fs_close(bs_file_t f) {
    if (!f) return;
    fclose((FILE*)f);
}

/* ---- Directory / existence -------------------------------------------- */

bool bs_fs_exists(const char* path) {
    char fp[512];
    full_path(fp, sizeof fp, path);
    return access(fp, F_OK) == 0;
}

int bs_fs_mkdir_p(const char* path) {
    char fp[512];
    full_path(fp, sizeof fp, path);

    /* Walk through and create each component */
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", fp);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

int bs_fs_remove(const char* path) {
    char fp[512];
    full_path(fp, sizeof fp, path);
    return remove(fp);
}

/* ---- Convenience: read/write entire file ------------------------------ */

long bs_fs_file_size(const char* path) {
    char fp[512]; full_path(fp, sizeof fp, path);
    FILE* f = fopen(fp, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
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
    bs_file_t f = bs_fs_open(path, "w");
    if (!f) return -1;
    int n = bs_fs_write(f, buf, len);
    bs_fs_close(f);
    return n;
}

#endif /* BS_FS_NATIVE */
