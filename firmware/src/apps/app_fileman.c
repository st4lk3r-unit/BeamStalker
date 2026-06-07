/*
 * app_fileman.c - On-device filesystem browser / file manager.
 *
 * Features:
 *   - Browse the real mounted SD-card root, not only /BeamStalker
 *   - Open/view small text files
 *   - Edit small text files in-place
 *   - Create files/folders
 *   - Copy/cut/paste files, move/rename entries
 *   - Delete files or folders recursively with confirmation
 */
#include "apps/app_fileman.h"
#include "bs/bs_assets.h"
#include "bs/bs_fs.h"
#include "bs/bs_gfx.h"
#include "bs/bs_keys.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_board.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define FM_MAX_PATH   160
#define FM_MAX_NAME    64
#define FM_MAX_ENTRIES 96
#define FM_EDIT_MAX  2048
#define FM_COPY_CHUNK 512
#define FM_TEXT_HSTEP 8
#define FM_HEX_HSTEP 8
#define FM_HEX_BYTES_PER_ROW 8
#define FM_HEX_LINE_MAX 64

typedef struct {
    char name[FM_MAX_NAME];
    bool is_dir;
    long size;
} fm_entry_t;

typedef enum {
    FM_BROWSE = 0,
    FM_ACTIONS,
    FM_INPUT,
    FM_VIEW,
    FM_HEX,
    FM_EDIT,
    FM_CONFIRM,
    FM_MESSAGE
} fm_mode_t;

typedef enum {
    FM_OP_NONE = 0,
    FM_OP_NEW_FILE,
    FM_OP_NEW_DIR,
    FM_OP_RENAME
} fm_input_op_t;

typedef enum {
    FM_ACT_OPEN = 0,
    FM_ACT_HEX,
    FM_ACT_EDIT,
    FM_ACT_COPY,
    FM_ACT_CUT,
    FM_ACT_PASTE,
    FM_ACT_RENAME,
    FM_ACT_DELETE,
    FM_ACT_NEW_FILE,
    FM_ACT_NEW_DIR,
    FM_ACT_COUNT
} fm_action_id_t;

typedef struct {
    fm_action_id_t id;
    const char* name;
    const char* desc;
} fm_action_t;

static fm_mode_t s_mode;
static char      s_cwd[FM_MAX_PATH];
static fm_entry_t s_entries[FM_MAX_ENTRIES];
static int       s_entry_count;
static int       s_cursor;
static int       s_scroll;
static bool      s_dirty;

static char      s_selected_path[FM_MAX_PATH];
static bool      s_selected_is_dir;
static char      s_selected_name[FM_MAX_NAME];
static char      s_op_dir[FM_MAX_PATH];

static char      s_clip_path[FM_MAX_PATH];
static char      s_clip_name[FM_MAX_NAME];
static bool      s_clip_valid;
static bool      s_clip_cut;

static fm_action_t s_actions[FM_ACT_COUNT];
static int       s_action_count;
static int       s_action_cursor;
static int       s_action_scroll;

static fm_input_op_t s_input_op;
static char      s_input_title[40];
static char      s_input_buf[FM_MAX_NAME];
static int       s_input_len;

static char      s_msg[96];
static uint32_t  s_msg_until;
static fm_mode_t s_msg_return;

static char      s_edit_path[FM_MAX_PATH];
static char      s_edit_buf[FM_EDIT_MAX + 1];
static size_t    s_edit_len;
static int       s_edit_scroll;
static int       s_edit_x_scroll;
static bool      s_edit_dirty;
static bool      s_edit_view_only;

static bool is_raw_root(const char* path) {
    return path && strcmp(path, BS_FS_RAW_ROOT) == 0;
}

static bool is_effective_root(const char* path) {
    return !path || !path[0] || is_raw_root(path);
}

static const char* cwd_label(void) {
    static char label[FM_MAX_PATH];
    if (is_raw_root(s_cwd)) return "/sd";
    if (strncmp(s_cwd, BS_FS_RAW_ROOT, strlen(BS_FS_RAW_ROOT)) == 0) {
        snprintf(label, sizeof label, "/sd/%s", s_cwd + strlen(BS_FS_RAW_ROOT));
        return label;
    }
    return s_cwd[0] ? s_cwd : "/";
}

static void set_message(const bs_arch_t* arch, const char* msg, fm_mode_t ret) {
    snprintf(s_msg, sizeof s_msg, "%s", msg ? msg : "");
    s_msg_until = arch->millis() + 900U;
    s_msg_return = ret;
    s_mode = FM_MESSAGE;
    s_dirty = true;
}

static bool valid_name(const char* name) {
    if (!name || !name[0]) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const char* p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    }
    return true;
}

static void path_join(char* dst, size_t n, const char* dir, const char* name) {
    if (!dst || n == 0) return;
    if (!dir || !dir[0]) {
        snprintf(dst, n, "%s", name ? name : "");
    } else if (!name || !name[0]) {
        snprintf(dst, n, "%s", dir);
    } else if (strcmp(dir, BS_FS_RAW_ROOT) == 0 || dir[strlen(dir) - 1] == '/') {
        snprintf(dst, n, "%s%s", dir, name);
    } else {
        snprintf(dst, n, "%s/%s", dir, name);
    }
}

static void path_parent(char* path) {
    if (!path || !path[0]) return;

    if (strncmp(path, BS_FS_RAW_ROOT, strlen(BS_FS_RAW_ROOT)) == 0) {
        if (strcmp(path, BS_FS_RAW_ROOT) == 0) return;
        char* slash = strrchr(path, '/');
        if (!slash || slash < path + (int)strlen(BS_FS_RAW_ROOT)) {
            snprintf(path, FM_MAX_PATH, "%s", BS_FS_RAW_ROOT);
            return;
        }
        if (slash == path + (int)strlen(BS_FS_RAW_ROOT) - 1) {
            snprintf(path, FM_MAX_PATH, "%s", BS_FS_RAW_ROOT);
            return;
        }
        *slash = '\0';
        return;
    }

    char* slash = strrchr(path, '/');
    if (!slash) { path[0] = '\0'; return; }
    *slash = '\0';
}

static const char* path_basename(const char* path) {
    if (!path) return "";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int list_cb(const bs_dir_entry_t* ent, void* user) {
    (void)user;
    if (!ent || s_entry_count >= FM_MAX_ENTRIES) return 0;
    snprintf(s_entries[s_entry_count].name, sizeof s_entries[s_entry_count].name, "%s", ent->name);
    s_entries[s_entry_count].is_dir = ent->is_dir;
    s_entries[s_entry_count].size   = ent->size;
    s_entry_count++;
    return 0;
}

static int entry_cmp(const fm_entry_t* a, const fm_entry_t* b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void sort_entries(void) {
    for (int i = 0; i < s_entry_count; i++) {
        for (int j = i + 1; j < s_entry_count; j++) {
            if (entry_cmp(&s_entries[i], &s_entries[j]) > 0) {
                fm_entry_t t = s_entries[i];
                s_entries[i] = s_entries[j];
                s_entries[j] = t;
            }
        }
    }
}

static void refresh_entries(void) {
    s_entry_count = 0;
    if (bs_fs_available()) bs_fs_list_dir(s_cwd, list_cb, NULL);
    sort_entries();
    if (s_entry_count <= 0) { s_cursor = 0; s_scroll = 0; }
    else if (s_cursor >= s_entry_count) s_cursor = s_entry_count - 1;
    s_dirty = true;
}

static void selected_from_cursor(void) {
    s_selected_path[0] = '\0';
    s_selected_name[0] = '\0';
    s_selected_is_dir = false;
    if (s_cursor < 0 || s_cursor >= s_entry_count) return;
    fm_entry_t* e = &s_entries[s_cursor];
    snprintf(s_selected_name, sizeof s_selected_name, "%s", e->name);
    s_selected_is_dir = e->is_dir;
    path_join(s_selected_path, sizeof s_selected_path, s_cwd, e->name);
}

static void unique_dest_path(char* out, size_t n, const char* dir, const char* name) {
    path_join(out, n, dir, name);
    if (!bs_fs_exists(out)) return;
    char stem[FM_MAX_NAME];
    char ext[24];
    snprintf(stem, sizeof stem, "%s", name ? name : "file");
    ext[0] = '\0';
    char* dot = strrchr(stem, '.');
    if (dot && dot != stem) {
        snprintf(ext, sizeof ext, "%s", dot);
        *dot = '\0';
    }
    for (int i = 1; i < 1000; i++) {
        char candidate[FM_MAX_NAME];
        snprintf(candidate, sizeof candidate, "%s-copy%d%s", stem, i, ext);
        path_join(out, n, dir, candidate);
        if (!bs_fs_exists(out)) return;
    }
}

static int copy_file(const char* src, const char* dst) {
    bs_file_t in = bs_fs_open(src, "r");
    if (!in) return -1;
    bs_file_t out = bs_fs_open(dst, "w");
    if (!out) { bs_fs_close(in); return -1; }

    uint8_t buf[FM_COPY_CHUNK];
    int rc = 0;
    for (;;) {
        int n = bs_fs_read(in, buf, sizeof buf);
        if (n < 0) { rc = -1; break; }
        if (n == 0) break;
        if (bs_fs_write(out, buf, (size_t)n) != n) { rc = -1; break; }
    }
    bs_fs_close(out);
    bs_fs_close(in);
    return rc;
}

static const char* current_action_dir(void) {
    if (s_selected_is_dir && s_selected_path[0]) return s_selected_path;
    return s_cwd;
}

static bool paste_clipboard_to(const char* dir) {
    if (!s_clip_valid) return false;
    char dst[FM_MAX_PATH];
    unique_dest_path(dst, sizeof dst, dir ? dir : s_cwd, s_clip_name);
    if (s_clip_cut) {
        if (bs_fs_rename(s_clip_path, dst) == 0) {
            s_clip_valid = false;
            return true;
        }
        return false;
    }
    return copy_file(s_clip_path, dst) == 0;
}

static void build_actions(bool for_dir, bool has_selection) {
    s_action_count = 0;
#define ADD_ACTION(ID, NAME, DESC) do { \
        s_actions[s_action_count].id = (ID); \
        s_actions[s_action_count].name = (NAME); \
        s_actions[s_action_count].desc = (DESC); \
        s_action_count++; \
    } while (0)
    if (has_selection) {
        ADD_ACTION(FM_ACT_OPEN, for_dir ? "Open Dir" : "View Text", for_dir ? "Enter folder" : "Read as text");
        if (!for_dir) ADD_ACTION(FM_ACT_HEX, "View Hex", "Raw byte inspector");
        if (!for_dir) ADD_ACTION(FM_ACT_EDIT, "Edit", "Small text files");
        if (!for_dir) ADD_ACTION(FM_ACT_COPY, "Copy", "Copy file to clipboard");
        ADD_ACTION(FM_ACT_CUT, "Cut/Move", for_dir ? "Move folder" : "Move file");
        ADD_ACTION(FM_ACT_RENAME, "Rename", "Change entry name");
        ADD_ACTION(FM_ACT_DELETE, "Delete", for_dir ? "Remove folder tree" : "Remove file");
    }
    if (s_clip_valid) ADD_ACTION(FM_ACT_PASTE, "Paste Here", for_dir && has_selection ? "Paste into folder" : "Paste in current dir");
    ADD_ACTION(FM_ACT_NEW_FILE, "New File", for_dir && has_selection ? "Create inside folder" : "Create in current dir");
    ADD_ACTION(FM_ACT_NEW_DIR, "New Folder", for_dir && has_selection ? "Create inside folder" : "Create in current dir");
#undef ADD_ACTION
    s_action_cursor = 0;
    s_action_scroll = 0;
}

static void begin_actions(bool directory_actions) {
    if (directory_actions || s_entry_count == 0) {
        s_selected_path[0] = '\0';
        s_selected_name[0] = '\0';
        s_selected_is_dir = true;
        build_actions(true, false);
    } else {
        selected_from_cursor();
        build_actions(s_selected_is_dir, true);
    }
    s_mode = FM_ACTIONS;
    s_dirty = true;
}

static void begin_input(fm_input_op_t op, const char* title, const char* initial) {
    s_input_op = op;
    snprintf(s_input_title, sizeof s_input_title, "%s", title ? title : "Name");
    snprintf(s_input_buf, sizeof s_input_buf, "%s", initial ? initial : "");
    s_input_len = (int)strlen(s_input_buf);
    s_mode = FM_INPUT;
    s_dirty = true;
}

static bool load_file_for_view(const char* path, bool edit) {
    size_t n = 0;
    memset(s_edit_buf, 0, sizeof s_edit_buf);
    int rc = bs_fs_read_file(path, s_edit_buf, FM_EDIT_MAX, &n);
    if (rc < 0) return false;
    s_edit_buf[n] = '\0';
    s_edit_len = n;
    s_edit_scroll = 0;
    s_edit_x_scroll = 0;
    s_edit_dirty = false;
    s_edit_view_only = !edit;
    snprintf(s_edit_path, sizeof s_edit_path, "%s", path);
    s_mode = edit ? FM_EDIT : FM_VIEW;
    s_dirty = true;
    return true;
}

static bool save_edit(void) {
    return bs_fs_write_file(s_edit_path, s_edit_buf, s_edit_len) == (int)s_edit_len;
}

static void execute_input(const bs_arch_t* arch) {
    if (!valid_name(s_input_buf)) { set_message(arch, "Invalid name", FM_BROWSE); return; }
    if (s_input_op == FM_OP_NEW_FILE) {
        char path[FM_MAX_PATH]; path_join(path, sizeof path, s_op_dir[0] ? s_op_dir : s_cwd, s_input_buf);
        if (bs_fs_exists(path)) { set_message(arch, "Already exists", FM_BROWSE); return; }
        if (bs_fs_write_file(path, "", 0) == 0) { refresh_entries(); set_message(arch, "File created", FM_BROWSE); }
        else set_message(arch, "Create failed", FM_BROWSE);
    } else if (s_input_op == FM_OP_NEW_DIR) {
        char path[FM_MAX_PATH]; path_join(path, sizeof path, s_op_dir[0] ? s_op_dir : s_cwd, s_input_buf);
        if (bs_fs_exists(path)) { set_message(arch, "Already exists", FM_BROWSE); return; }
        if (bs_fs_mkdir_p(path) == 0) { refresh_entries(); set_message(arch, "Folder created", FM_BROWSE); }
        else set_message(arch, "Mkdir failed", FM_BROWSE);
    } else if (s_input_op == FM_OP_RENAME) {
        char parent[FM_MAX_PATH]; snprintf(parent, sizeof parent, "%s", s_selected_path); path_parent(parent);
        char dst[FM_MAX_PATH]; path_join(dst, sizeof dst, parent, s_input_buf);
        if (bs_fs_exists(dst)) { set_message(arch, "Already exists", FM_BROWSE); return; }
        if (bs_fs_rename(s_selected_path, dst) == 0) { refresh_entries(); set_message(arch, "Renamed", FM_BROWSE); }
        else set_message(arch, "Rename failed", FM_BROWSE);
    }
}

static void execute_action(const bs_arch_t* arch) {
    if (s_action_cursor < 0 || s_action_cursor >= s_action_count) return;
    fm_action_id_t id = s_actions[s_action_cursor].id;
    switch (id) {
        case FM_ACT_OPEN:
            if (s_selected_is_dir) {
                snprintf(s_cwd, sizeof s_cwd, "%s", s_selected_path);
                s_cursor = s_scroll = 0;
                refresh_entries();
                s_mode = FM_BROWSE;
            } else if (!load_file_for_view(s_selected_path, false)) {
                set_message(arch, "Open failed", FM_BROWSE);
            }
            break;
        case FM_ACT_HEX:
            if (load_file_for_view(s_selected_path, false)) {
                s_mode = FM_HEX;
                s_dirty = true;
            } else {
                set_message(arch, "Hex open failed", FM_BROWSE);
            }
            break;
        case FM_ACT_EDIT:
            if (!load_file_for_view(s_selected_path, true)) set_message(arch, "Edit failed", FM_BROWSE);
            break;
        case FM_ACT_COPY:
            snprintf(s_clip_path, sizeof s_clip_path, "%s", s_selected_path);
            snprintf(s_clip_name, sizeof s_clip_name, "%s", s_selected_name);
            s_clip_valid = true; s_clip_cut = false;
            set_message(arch, "Copied", FM_BROWSE);
            break;
        case FM_ACT_CUT:
            snprintf(s_clip_path, sizeof s_clip_path, "%s", s_selected_path);
            snprintf(s_clip_name, sizeof s_clip_name, "%s", s_selected_name);
            s_clip_valid = true; s_clip_cut = true;
            set_message(arch, "Cut", FM_BROWSE);
            break;
        case FM_ACT_PASTE:
            if (paste_clipboard_to(current_action_dir())) { refresh_entries(); set_message(arch, "Pasted", FM_BROWSE); }
            else set_message(arch, "Paste failed", FM_BROWSE);
            break;
        case FM_ACT_RENAME:
            begin_input(FM_OP_RENAME, "Rename", s_selected_name);
            break;
        case FM_ACT_DELETE:
            s_mode = FM_CONFIRM;
            s_dirty = true;
            break;
        case FM_ACT_NEW_FILE:
            snprintf(s_op_dir, sizeof s_op_dir, "%s", current_action_dir());
            begin_input(FM_OP_NEW_FILE, "New file", "new.txt");
            break;
        case FM_ACT_NEW_DIR:
            snprintf(s_op_dir, sizeof s_op_dir, "%s", current_action_dir());
            begin_input(FM_OP_NEW_DIR, "New folder", "folder");
            break;
        default: break;
    }
}

static void draw_unavailable(void) {
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("File Manager");
    bs_gfx_text(8, bs_ui_content_y() + 6, "Storage unavailable", g_bs_theme.warn, bs_ui_text_scale());
    const char* e = bs_fs_init_error();
    if (e) bs_ui_draw_text_box(8, bs_ui_content_y() + 28, bs_gfx_width() - 16, e, g_bs_theme.dim, 1.0f, true);
    bs_ui_draw_hint("BACK=exit");
    bs_gfx_present();
}

static void draw_browser(void) {
    float ts = bs_ui_text_scale();
    int row_h = bs_ui_row_h(ts);
    int vis = bs_ui_list_visible(ts);
    bs_ui_list_clamp_scroll(s_cursor, &s_scroll, s_entry_count, vis);

    bs_gfx_clear(g_bs_theme.bg);
    char title[64]; snprintf(title, sizeof title, "Files %s", cwd_label());
    bs_ui_draw_header(title);

    if (s_entry_count == 0) {
        bs_gfx_text(8, bs_ui_content_y() + 6, "<empty>", g_bs_theme.dim, ts);
    }
    for (int i = 0; i < vis && (s_scroll + i) < s_entry_count; i++) {
        int idx = s_scroll + i;
        fm_entry_t* e = &s_entries[idx];
        char line[96];
        int y = bs_ui_content_y() + i * row_h;
        bool selected = (idx == s_cursor);
        if (selected) bs_gfx_fill_rect(0, y, bs_gfx_width(), row_h, g_bs_theme.dim);

        if (e->is_dir) {
            const char* mark = "D ";
            int mark_w = bs_gfx_text_w(mark, ts);
            bs_color_t mark_col = selected ? g_bs_theme.secondary : g_bs_theme.dim;
            bs_gfx_text(8, y + 3, mark, mark_col, ts);
            snprintf(line, sizeof line, "%s", e->name);
            bs_ui_draw_text_box(8 + mark_w, y + 3, bs_gfx_width() - 16 - mark_w,
                                line, selected ? g_bs_theme.accent : g_bs_theme.primary, ts, selected);
        } else {
            if (e->size >= 0) snprintf(line, sizeof line, "%s  %ldB", e->name, e->size);
            else snprintf(line, sizeof line, "%s", e->name);
            bs_ui_draw_text_box(8, y + 3, bs_gfx_width() - 16, line,
                                selected ? g_bs_theme.accent : g_bs_theme.primary, ts, selected);
        }
    }
    bs_ui_draw_scroll_arrows(s_scroll, s_entry_count, vis);
    bs_ui_draw_hint(s_clip_valid ? "SEL=open  RIGHT=ops/paste  BACK=up" : "SEL=open  RIGHT=ops  BACK=up");
    bs_gfx_present();
}

static void draw_actions(void) {
    float ts = bs_ui_text_scale();
    int row_h = bs_ui_menu_row_h(ts);
    int vis = bs_ui_menu_visible(ts);
    bs_ui_list_clamp_scroll(s_action_cursor, &s_action_scroll, s_action_count, vis);
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("File Actions");
    for (int i = 0; i < vis && (s_action_scroll + i) < s_action_count; i++) {
        int idx = s_action_scroll + i;
        bs_ui_draw_menu_row(bs_ui_content_y() + i * row_h,
                            s_actions[idx].name, s_actions[idx].desc,
                            idx == s_action_cursor, ts);
    }
    bs_ui_draw_scroll_arrows(s_action_scroll, s_action_count, vis);
    bs_ui_draw_hint("SELECT=run  BACK=files");
    bs_gfx_present();
}

static void draw_input(void) {
    float ts = bs_ui_text_scale();
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header(s_input_title);
    int y = bs_ui_content_y() + 8;
    bs_gfx_text(8, y, "Name:", g_bs_theme.dim, ts);
    bs_gfx_border(8, y + bs_gfx_text_h(ts) + 4, bs_gfx_width() - 16,
                  bs_gfx_text_h(ts) + 10, g_bs_theme.secondary, g_bs_theme.border);
    bs_ui_draw_text_box(12, y + bs_gfx_text_h(ts) + 9, bs_gfx_width() - 24,
                        s_input_buf, g_bs_theme.accent, ts, true);
    bs_ui_draw_hint("TYPE=name  ENTER=ok  BKSP=del  ESC=cancel");
    bs_gfx_present();
}

static int text_line_count(void) {
    if (s_edit_len == 0) return 0;
    int lines = 1;
    for (size_t i = 0; i < s_edit_len; i++) {
        if (s_edit_buf[i] == '\n') lines++;
    }
    return lines;
}

static bool text_has_col_after(int col) {
    if (col <= 0) return s_edit_len > 0;
    int line_len = 0;
    for (size_t i = 0; i < s_edit_len; i++) {
        if (s_edit_buf[i] == '\n') {
            if (line_len > col) return true;
            line_len = 0;
        } else {
            line_len++;
        }
    }
    return line_len > col;
}

static bool text_line_slice(int wanted_line, int xoff, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    if (wanted_line < 0) return false;

    int line = 0;
    size_t pos = 0;
    while (pos < s_edit_len && line < wanted_line) {
        if (s_edit_buf[pos++] == '\n') line++;
    }
    if (line != wanted_line || pos > s_edit_len) return false;

    while (pos < s_edit_len && xoff > 0 && s_edit_buf[pos] != '\n') {
        pos++;
        xoff--;
    }

    size_t n = 0;
    while (pos < s_edit_len && s_edit_buf[pos] != '\n' && n + 1 < out_sz) {
        unsigned char c = (unsigned char)s_edit_buf[pos++];
        if (c == '\t') out[n++] = ' ';
        else if (c < 32 || c > 126) out[n++] = '.';
        else out[n++] = (char)c;
    }
    out[n] = '\0';
    return true;
}

static void draw_text_page(bool edit) {
    float ts = bs_ui_text_scale();
    int row_h = bs_ui_row_h(ts);
    int max_rows = bs_ui_list_visible(ts);
    int lines = text_line_count();
    if (lines <= 0) s_edit_scroll = 0;
    else if (s_edit_scroll > lines - 1) s_edit_scroll = lines - 1;

    bs_gfx_clear(g_bs_theme.bg);
    char title[40];
    if (!edit && s_edit_x_scroll > 0) snprintf(title, sizeof title, "View File +%d", s_edit_x_scroll);
    else snprintf(title, sizeof title, "%s", edit ? "Edit File" : "View File");
    bs_ui_draw_header(title);

    int y0 = bs_ui_content_y();
    if (s_edit_len == 0) {
        bs_gfx_text(8, y0 + 3, "<empty>", g_bs_theme.dim, ts);
    } else {
        for (int row = 0; row < max_rows && (s_edit_scroll + row) < lines; row++) {
            char out[96];
            text_line_slice(s_edit_scroll + row, edit ? 0 : s_edit_x_scroll, out, sizeof out);
            bs_ui_draw_text_box(8, y0 + row * row_h + 3, bs_gfx_width() - 16,
                                out, g_bs_theme.primary, ts, false);
        }
    }
    bs_ui_draw_hint(edit ? "TYPE edit  ENTER=newline  LEFT=save  ESC=cancel" : "UP/DN lines  L/R horiz  BACK=files");
    bs_gfx_present();
}


static void hex_format_line(int off, char* line, size_t line_sz) {
    if (!line || line_sz == 0) return;
    const uint8_t* data = (const uint8_t*)s_edit_buf;
    char hex[FM_HEX_BYTES_PER_ROW * 3 + 1];
    char asc[FM_HEX_BYTES_PER_ROW + 1];
    int hn = 0;
    int an = 0;
    for (int i = 0; i < FM_HEX_BYTES_PER_ROW; i++) {
        int idx = off + i;
        if (idx < (int)s_edit_len) {
            hn += snprintf(hex + hn, sizeof hex - (size_t)hn, "%02X ", data[idx]);
            unsigned char c = data[idx];
            asc[an++] = (c >= 32 && c <= 126) ? (char)c : '.';
        } else {
            hn += snprintf(hex + hn, sizeof hex - (size_t)hn, "   ");
            asc[an++] = ' ';
        }
    }
    asc[an] = '\0';
    snprintf(line, line_sz, "%04X: %s %s", off, hex, asc);
}

static bool hex_has_col_after(int col) {
    if (s_edit_len == 0) return false;
    int rows = (int)((s_edit_len + FM_HEX_BYTES_PER_ROW - 1) / FM_HEX_BYTES_PER_ROW);
    if (rows <= 0) return false;
    char line[FM_HEX_LINE_MAX];
    hex_format_line(0, line, sizeof line);
    int len = (int)strlen(line);
    return len > col;
}

static void hex_line_slice(int off, int xoff, char* out, size_t out_sz) {
    char line[FM_HEX_LINE_MAX];
    hex_format_line(off, line, sizeof line);
    size_t len = strlen(line);
    if (!out || out_sz == 0) return;
    if (xoff < 0) xoff = 0;
    if ((size_t)xoff >= len) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_sz, "%s", line + xoff);
}

static void draw_hex_page(void) {
    float ts = bs_ui_text_scale();
    int row_h = bs_ui_row_h(ts);
    int max_rows = bs_ui_list_visible(ts);
    int rows = (int)((s_edit_len + FM_HEX_BYTES_PER_ROW - 1) / FM_HEX_BYTES_PER_ROW);
    if (rows <= 0) s_edit_scroll = 0;
    else if (s_edit_scroll > rows - 1) s_edit_scroll = rows - 1;

    bs_gfx_clear(g_bs_theme.bg);
    char title[40];
    if (s_edit_x_scroll > 0) snprintf(title, sizeof title, "Hex View +%d", s_edit_x_scroll);
    else snprintf(title, sizeof title, "Hex View");
    bs_ui_draw_header(title);

    int y0 = bs_ui_content_y();
    if (s_edit_len == 0) {
        bs_gfx_text(8, y0 + 3, "<empty>", g_bs_theme.dim, ts);
    } else {
        for (int row = 0; row < max_rows && (s_edit_scroll + row) < rows; row++) {
            int off = (s_edit_scroll + row) * FM_HEX_BYTES_PER_ROW;
            char line[FM_HEX_LINE_MAX];
            hex_line_slice(off, s_edit_x_scroll, line, sizeof line);
            bs_ui_draw_text_box(8, y0 + row * row_h + 3, bs_gfx_width() - 16,
                                line, g_bs_theme.primary, ts, false);
        }
    }
    bs_ui_draw_hint("UP/DN rows  L/R horiz  BACK=files");
    bs_gfx_present();
}

static void draw_confirm(void) {
    float ts = bs_ui_text_scale();
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Confirm Delete");
    bs_gfx_text(8, bs_ui_content_y() + 6, "Delete this entry?", g_bs_theme.warn, ts);
    bs_ui_draw_text_box(8, bs_ui_content_y() + 28, bs_gfx_width() - 16,
                        s_selected_name, g_bs_theme.primary, ts, true);
    bs_ui_draw_hint("SELECT=delete  BACK=cancel");
    bs_gfx_present();
}

static void draw_message(void) {
    float ts = bs_ui_text_scale();
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("File Manager");
    bs_ui_draw_text_box(8, bs_ui_content_y() + 20, bs_gfx_width() - 16,
                        s_msg, g_bs_theme.accent, ts, true);
    bs_gfx_present();
}

static void fileman_run(const bs_arch_t* arch) {
    s_mode = FM_BROWSE;
    snprintf(s_cwd, sizeof s_cwd, "%s", BS_FS_RAW_ROOT);
    s_cursor = s_scroll = 0;
    s_clip_valid = false;
    s_op_dir[0] = '\0';
    s_dirty = true;
    refresh_entries();

    uint32_t prev_ms = arch->millis();
    uint32_t last_anim_ms = prev_ms;
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        if (!bs_fs_available()) {
            draw_unavailable();
            bs_key_t key;
            while (bs_keys_poll(&key)) {
                bs_nav_id_t nav = bs_nav_from_key(key);
                if (nav == BS_NAV_BACK || nav == BS_NAV_SELECT) return;
            }
            arch->delay_ms((uint32_t)bs_board_ui_idle_delay_ms());
            continue;
        }

        if (s_mode == FM_MESSAGE && (int32_t)(now - s_msg_until) >= 0) {
            s_mode = s_msg_return;
            refresh_entries();
            s_dirty = true;
        }

        bool anim_due = bs_ui_carousel_enabled() && (uint32_t)(now - last_anim_ms) >= 100U;
        if (s_dirty || anim_due) {
            switch (s_mode) {
                case FM_BROWSE:  draw_browser(); break;
                case FM_ACTIONS: draw_actions(); break;
                case FM_INPUT:   draw_input(); break;
                case FM_VIEW:    draw_text_page(false); break;
                case FM_HEX:     draw_hex_page(); break;
                case FM_EDIT:    draw_text_page(true); break;
                case FM_CONFIRM: draw_confirm(); break;
                case FM_MESSAGE: draw_message(); break;
            }
            s_dirty = false;
            if (anim_due) last_anim_ms = now;
        }

        bs_key_t key;
        while (bs_keys_poll(&key)) {
            bs_nav_id_t nav = bs_nav_from_key(key);
            if (s_mode == FM_BROWSE) {
                switch (nav) {
                    case BS_NAV_UP: case BS_NAV_PREV:
                        if (s_entry_count > 0) s_cursor = (s_cursor + s_entry_count - 1) % s_entry_count;
                        s_dirty = true; break;
                    case BS_NAV_DOWN: case BS_NAV_NEXT:
                        if (s_entry_count > 0) s_cursor = (s_cursor + 1) % s_entry_count;
                        s_dirty = true; break;
                    case BS_NAV_SELECT:
                        if (s_entry_count > 0) {
                            selected_from_cursor();
                            if (s_selected_is_dir) {
                                snprintf(s_cwd, sizeof s_cwd, "%s", s_selected_path);
                                s_cursor = s_scroll = 0;
                                refresh_entries();
                            } else begin_actions(false);
                        } else begin_actions(true);
                        break;
                    case BS_NAV_RIGHT:
                        begin_actions(s_entry_count == 0);
                        break;
                    case BS_NAV_BACK:
                        if (!is_effective_root(s_cwd)) { path_parent(s_cwd); s_cursor = s_scroll = 0; refresh_entries(); }
                        else return;
                        break;
                    default: break;
                }
            } else if (s_mode == FM_ACTIONS) {
                switch (nav) {
                    case BS_NAV_UP: case BS_NAV_PREV:
                        s_action_cursor = (s_action_cursor + s_action_count - 1) % s_action_count; s_dirty = true; break;
                    case BS_NAV_DOWN: case BS_NAV_NEXT:
                        s_action_cursor = (s_action_cursor + 1) % s_action_count; s_dirty = true; break;
                    case BS_NAV_SELECT:
                        execute_action(arch); break;
                    case BS_NAV_BACK:
                        s_mode = FM_BROWSE; s_dirty = true; break;
                    default: break;
                }
            } else if (s_mode == FM_INPUT) {
                if (key.id == BS_KEY_CHAR && s_input_len < FM_MAX_NAME - 1) {
                    s_input_buf[s_input_len++] = key.ch;
                    s_input_buf[s_input_len] = '\0';
                    s_dirty = true;
                } else if (key.id == BS_KEY_BACK && s_input_len > 0) {
                    s_input_buf[--s_input_len] = '\0';
                    s_dirty = true;
                } else if (key.id == BS_KEY_ENTER) {
                    execute_input(arch);
                } else if (key.id == BS_KEY_ESC) {
                    s_mode = FM_BROWSE; s_dirty = true;
                }
            } else if (s_mode == FM_VIEW) {
                int lines = text_line_count();
                if (nav == BS_NAV_BACK || nav == BS_NAV_SELECT) { s_mode = FM_BROWSE; s_dirty = true; }
                else if (nav == BS_NAV_UP && s_edit_scroll > 0) { s_edit_scroll--; s_dirty = true; }
                else if (nav == BS_NAV_DOWN && s_edit_scroll + 1 < lines) { s_edit_scroll++; s_dirty = true; }
                else if (nav == BS_NAV_LEFT && s_edit_x_scroll > 0) {
                    s_edit_x_scroll = (s_edit_x_scroll > FM_TEXT_HSTEP) ? s_edit_x_scroll - FM_TEXT_HSTEP : 0;
                    s_dirty = true;
                } else if (nav == BS_NAV_RIGHT && text_has_col_after(s_edit_x_scroll + FM_TEXT_HSTEP)) {
                    s_edit_x_scroll += FM_TEXT_HSTEP;
                    s_dirty = true;
                }
            } else if (s_mode == FM_HEX) {
                int rows = (int)((s_edit_len + FM_HEX_BYTES_PER_ROW - 1) / FM_HEX_BYTES_PER_ROW);
                if (nav == BS_NAV_BACK || nav == BS_NAV_SELECT) { s_mode = FM_BROWSE; s_dirty = true; }
                else if (nav == BS_NAV_UP && s_edit_scroll > 0) { s_edit_scroll--; s_dirty = true; }
                else if (nav == BS_NAV_DOWN && s_edit_scroll + 1 < rows) { s_edit_scroll++; s_dirty = true; }
                else if (nav == BS_NAV_LEFT && s_edit_x_scroll > 0) {
                    s_edit_x_scroll = (s_edit_x_scroll > FM_HEX_HSTEP) ? s_edit_x_scroll - FM_HEX_HSTEP : 0;
                    s_dirty = true;
                } else if (nav == BS_NAV_RIGHT && hex_has_col_after(s_edit_x_scroll + FM_HEX_HSTEP)) {
                    s_edit_x_scroll += FM_HEX_HSTEP;
                    s_dirty = true;
                }
            } else if (s_mode == FM_EDIT) {
                if (key.id == BS_KEY_CHAR && s_edit_len < FM_EDIT_MAX) {
                    s_edit_buf[s_edit_len++] = key.ch;
                    s_edit_buf[s_edit_len] = '\0';
                    s_edit_dirty = true; s_dirty = true;
                } else if (key.id == BS_KEY_ENTER && s_edit_len < FM_EDIT_MAX) {
                    s_edit_buf[s_edit_len++] = '\n';
                    s_edit_buf[s_edit_len] = '\0';
                    s_edit_dirty = true; s_dirty = true;
                } else if (key.id == BS_KEY_BACK && s_edit_len > 0) {
                    s_edit_buf[--s_edit_len] = '\0';
                    s_edit_dirty = true; s_dirty = true;
                } else if (key.id == BS_KEY_LEFT) {
                    if (save_edit()) { refresh_entries(); set_message(arch, "Saved", FM_BROWSE); }
                    else set_message(arch, "Save failed", FM_BROWSE);
                } else if (key.id == BS_KEY_ESC) {
                    s_mode = FM_BROWSE; s_dirty = true;
                } else if (key.id == BS_KEY_UP && s_edit_scroll > 0) {
                    s_edit_scroll--; s_dirty = true;
                } else if (key.id == BS_KEY_DOWN) {
                    int lines = text_line_count();
                    if (s_edit_scroll + 1 < lines) { s_edit_scroll++; s_dirty = true; }
                }
            } else if (s_mode == FM_CONFIRM) {
                if (nav == BS_NAV_SELECT) {
                    if (bs_fs_remove(s_selected_path) == 0) { refresh_entries(); set_message(arch, "Deleted", FM_BROWSE); }
                    else set_message(arch, "Delete failed", FM_BROWSE);
                } else if (nav == BS_NAV_BACK) {
                    s_mode = FM_BROWSE; s_dirty = true;
                }
            } else if (s_mode == FM_MESSAGE) {
                if (nav == BS_NAV_BACK || nav == BS_NAV_SELECT) { s_mode = s_msg_return; refresh_entries(); s_dirty = true; }
            }
        }
        arch->delay_ms((uint32_t)bs_board_ui_idle_delay_ms());
    }
}

const bs_app_t app_fileman = {
    .name   = "Files",
    .icon   = bs_folder_32,
    .icon_w = 32,
    .icon_h = 32,
    .run    = fileman_run,
};
