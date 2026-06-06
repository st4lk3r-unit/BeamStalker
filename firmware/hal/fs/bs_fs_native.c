/*
 * bs_fs_native.c - bs_fs backend for native Linux.
 *
 * Stores normal BeamStalker files under ./BeamStalker/ relative to the
 * working directory where the binary is launched.  Paths beginning with
 * BS_FS_RAW_PREFIX ("sd:") are mapped to ./sdcard/ for native testing.
 */
#ifdef BS_FS_NATIVE

#include "bs/bs_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>

#define BS_FS_ROOT     "./BeamStalker"
#define BS_FS_RAW_ROOT_DIR "./sdcard"

/* ---- Path helper ------------------------------------------------------- */

static bool is_raw_path(const char* path) {
    return path && strncmp(path, BS_FS_RAW_PREFIX, strlen(BS_FS_RAW_PREFIX)) == 0;
}

static void full_path(char* dst, size_t sz, const char* path) {
    if (is_raw_path(path)) {
        const char* p = path + strlen(BS_FS_RAW_PREFIX);
        while (p && *p == '/') p++;
        if (p && p[0] != '\0')
            snprintf(dst, sz, BS_FS_RAW_ROOT_DIR "/%s", p);
        else
            snprintf(dst, sz, "%s", BS_FS_RAW_ROOT_DIR);
        return;
    }

    /* Strip leading '/' from app-relative path before appending. */
    if (path && path[0] == '/') path++;
    if (path && path[0] != '\0')
        snprintf(dst, sz, BS_FS_ROOT "/%s", path);
    else
        snprintf(dst, sz, "%s", BS_FS_ROOT);
}

static int mkdir_p_full(const char* full) {
    if (!full || !full[0]) return -1;

    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", full);
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

static void ensure_parent_dir_full(const char* full) {
    if (!full || !full[0]) return;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", full);
    char* slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    if (dir[0]) (void)mkdir_p_full(dir);
}

static int mode_writes(const char* mode) {
    return mode && (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+') != NULL);
}

/* ---- Init -------------------------------------------------------------- */

static bool s_available = false;

int bs_fs_init(void) {
    if (mkdir(BS_FS_ROOT, 0755) < 0 && errno != EEXIST) return -1;
    if (mkdir(BS_FS_RAW_ROOT_DIR, 0755) < 0 && errno != EEXIST) return -1;
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
    if (mode_writes(mode)) ensure_parent_dir_full(fp);
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

bool bs_fs_is_dir(const char* path) {
    char fp[512];
    struct stat st;
    full_path(fp, sizeof fp, path);
    return stat(fp, &st) == 0 && S_ISDIR(st.st_mode);
}

int bs_fs_mkdir_p(const char* path) {
    char fp[512];
    full_path(fp, sizeof fp, path);
    return mkdir_p_full(fp);
}

static int remove_full_recursive(const char* full) {
    if (!full || !full[0]) return -1;

    struct stat st;
    if (stat(full, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(full);
        if (!d) return -1;

        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[512];
            snprintf(child, sizeof child, "%s/%s", full, de->d_name);
            if (remove_full_recursive(child) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        return rmdir(full);
    }

    return remove(full);
}

int bs_fs_remove(const char* path) {
    char fp[512];
    full_path(fp, sizeof fp, path);
    if (!strcmp(fp, BS_FS_ROOT) || !strcmp(fp, BS_FS_RAW_ROOT_DIR)) return -1;
    return remove_full_recursive(fp);
}

int bs_fs_rename(const char* old_path, const char* new_path) {
    char old_fp[512], new_fp[512];
    full_path(old_fp, sizeof old_fp, old_path);
    full_path(new_fp, sizeof new_fp, new_path);
    return rename(old_fp, new_fp);
}

int bs_fs_list_dir(const char* path, bs_fs_list_cb cb, void* user) {
    if (!cb) return -1;
    char fp[512];
    full_path(fp, sizeof fp, path);
    DIR* d = opendir(fp);
    if (!d) return -1;

    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        bs_dir_entry_t ent;
        memset(&ent, 0, sizeof ent);
        snprintf(ent.name, sizeof ent.name, "%s", de->d_name);

        char child[512];
        snprintf(child, sizeof child, "%s/%s", fp, de->d_name);
        struct stat st;
        if (stat(child, &st) == 0) {
            ent.is_dir = S_ISDIR(st.st_mode);
            ent.size   = ent.is_dir ? 0 : (long)st.st_size;
        } else {
            ent.size = -1;
        }
        if (cb(&ent, user) != 0) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
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
