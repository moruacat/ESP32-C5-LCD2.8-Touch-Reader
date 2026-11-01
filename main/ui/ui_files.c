/**
 * @file ui_files.c
 * @brief 文件管理：浏览 SD 卡、把外部书/音乐收进书架与媒体库、删除文件
 *
 * 布局（横屏 320x240）：
 *   标题栏 / 路径栏（含「上级」「重载」）/ 可滚动的文件列表 / 底部 SD 卡占用
 *
 * 交互：
 *   点目录   进去（.ebf 目录当成书，直接打开阅读）
 *   点 .txt  打开阅读
 *   点音频   收进媒体库
 *   长按     菜单：打开 / 加入书架 / 加入音乐库 / 删除
 *
 * 浏览位置放在 static 里，但每次进页面都重置到挂载点根目录 —— 不记住上次
 * 走到哪，免得换卡之后停在一个已经不存在的路径上。
 */
#include "ui_files.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include "srv_fs.h"
#include "srv_sd.h"
#include "srv_library.h"
#include "srv_musiclib.h"
#include "srv_reader.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_files";

/* 列表最多记多少条：屏上只看得见 4 行，多出来的靠滚动；上限只是防内存爆 */
#define FILE_LIST_MAX   64

#define ROW_W           304
#define ROW_H           32
#define ROW_GAP         4

#define PATH_Y          30
#define PATH_H          32
#define LIST_Y          64
#define LIST_H          140
#define SD_BAR_Y        208
#define SD_TEXT_Y       218

typedef struct {
    char     name[SRV_FS_NAME_MAX];
    bool     is_dir;
    bool     is_ebf;        /*!< .ebf 目录：当书看 */
    uint32_t size;
} file_item_t;

static file_item_t s_items[FILE_LIST_MAX];
static int         s_count = 0;
static char        s_cur[SRV_FS_PATH_MAX];

/* 页面上的静态控件。切页会整棵树删掉，销毁回调里置空（见 on_list_delete）。 */
static lv_obj_t *s_list;
static lv_obj_t *s_path_lb;
static lv_obj_t *s_up_btn;      /*!< 「上级」按钮本身 */
static lv_obj_t *s_up_lb;       /*!< 「上级」按钮上的文字 */
static lv_obj_t *s_sd_lb;
static lv_obj_t *s_sd_bar;

/* 长按之后松手，LVGL 还会补一个 CLICKED，用它吞掉（用时刻比对而不是布尔标志：
 * 长按后把手指滑出该行再松开就不会有 CLICKED，标志会一直留着）。 */
static uint32_t s_menu_tick = 0;

/* 菜单动作针对的那一条：长按时快照下来，之后不管列表怎么重建都用它 */
static char s_act_path[SRV_FS_PATH_MAX];
static char s_act_name[SRV_FS_NAME_MAX];
static bool s_act_is_dir;
static bool s_act_is_ebf;
static int  s_menu_open  = -1;      /*!< 各动作在菜单里的下标（-1 = 本次没有这项） */
static int  s_menu_book  = -1;
static int  s_menu_music = -1;
static int  s_menu_del   = -1;

static void files_reload(void);

/* ===================== 小工具 ===================== */

static bool name_less(const char *a, const char *b)
{
    /* 大小写不敏感比较（不引 strcasecmp：模拟器按 -std=c11 编，POSIX 名字可能不声明） */
    for (;; a++, b++) {
        int ca = (unsigned char)*a;
        int cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) {
            return ca < cb;
        }
        if (ca == '\0') {
            return false;
        }
    }
}

static bool is_audio_ext(const char *ext)
{
    static const char *const k_ext[] = {
        "mp3", "wav", "aac", "m4a", "flac", "ogg", "amr", "ts",
    };
    for (size_t i = 0; i < sizeof(k_ext) / sizeof(k_ext[0]); i++) {
        if (strcmp(ext, k_ext[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void ext_of(const char *path, char *out, size_t n)
{
    srv_fs_ext(path, out, n);
}

static bool has_ext(const char *path, const char *ext)
{
    char e[16];
    ext_of(path, e, sizeof(e));
    return strcmp(e, ext) == 0;
}

/*! 能不能当书打开：TXT 文件，或 .ebf 文件夹 */
static bool is_book(bool is_dir, bool is_ebf, const char *path)
{
    return is_ebf || (!is_dir && has_ext(path, "txt"));
}

static void fmt_size(uint32_t bytes, char *out, size_t n)
{
    if (bytes >= 1024u * 1024u) {
        snprintf(out, n, "%.1fM", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024u) {
        snprintf(out, n, "%.1fK", (double)bytes / 1024.0);
    } else {
        snprintf(out, n, "%uB", (unsigned)bytes);
    }
}

static const lv_image_dsc_t *icon_for(const file_item_t *it)
{
    if (it->is_ebf) {
        return &ic_file_eb;
    }
    if (it->is_dir) {
        return &ic_folder;
    }

    char ext[16];
    ext_of(it->name, ext, sizeof(ext));
    if (strcmp(ext, "txt") == 0) {
        return &ic_file_txt;
    }
    if (is_audio_ext(ext)) {
        return &ic_music;
    }
    return &ic_page;
}

/* ===================== 目录扫描 ===================== */

typedef struct {
    file_item_t *buf;
    int          cap;
    int          n;
} scan_ctx_t;

static bool scan_cb(const srv_fs_entry_t *e, void *arg)
{
    scan_ctx_t *ctx = (scan_ctx_t *)arg;
    if (ctx->n >= ctx->cap) {
        return false;                   /* 满了就停 */
    }

    file_item_t *it = &ctx->buf[ctx->n];
    snprintf(it->name, sizeof(it->name), "%s", e->name);
    it->is_dir = e->is_dir;
    it->is_ebf = false;
    it->size   = e->size;
    if (e->is_dir) {
        it->is_ebf = has_ext(e->name, "ebf");
    }
    ctx->n++;
    return true;
}

/*! 目录在前、名字升序；条目少（上限 64）用插入排序足够 */
static void sort_items(void)
{
    for (int i = 1; i < s_count; i++) {
        file_item_t tmp = s_items[i];
        int j = i - 1;

        while (j >= 0) {
            const file_item_t *a = &s_items[j];
            if (a->is_dir != tmp.is_dir) {
                if (a->is_dir) {
                    break;              /* 目录一律排在文件前面 */
                }
            } else if (!name_less(tmp.name, a->name)) {
                break;
            }
            s_items[j + 1] = s_items[j];
            j--;
        }
        s_items[j + 1] = tmp;
    }
}

static void scan_dir(void)
{
    scan_ctx_t ctx = { .buf = s_items, .cap = FILE_LIST_MAX, .n = 0 };
    s_count = 0;

    if (!srv_sd_is_mounted()) {
        return;
    }
    if (srv_fs_list(s_cur, scan_cb, &ctx) == ESP_OK) {
        s_count = ctx.n;
        sort_items();
    }
}

/* ===================== 刷新 ===================== */

static void update_path(void)
{
    const char *root = srv_sd_mount_point();
    const char *rel  = s_cur;

    if (strncmp(s_cur, root, strlen(root)) == 0) {
        rel = s_cur + strlen(root);     /* "" 或 "/books" */
    }
    lv_label_set_text(s_path_lb, (rel[0] == '\0') ? "/" : rel);

    /* 已经在挂载点根上了就没什么可「上级」的：按钮禁用 + 文字变暗，
     * 两者都要（ui_row_btn_create 返回的是文字标签，禁用按钮得拿父对象） */
    const bool at_root = (strcmp(s_cur, root) == 0);
    if (at_root) {
        lv_obj_add_state(s_up_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_up_btn, LV_STATE_DISABLED);
    }
    lv_obj_set_style_text_color(s_up_lb, at_root ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT, 0);
}

static void update_sd(void)
{
    uint32_t total_kb = 0;
    uint32_t free_kb  = 0;

    if (!srv_sd_is_mounted() ||
        srv_sd_get_info(&total_kb, &free_kb) != ESP_OK || total_kb == 0) {
        lv_label_set_text(s_sd_lb, "SD 卡未挂载 · 插入后点「重载」");
        lv_bar_set_value(s_sd_bar, 0, LV_ANIM_OFF);
        return;
    }

    const uint32_t used_kb = (total_kb > free_kb) ? (total_kb - free_kb) : 0;
    const int pct = (int)((uint64_t)used_kb * 100u / total_kb);

    char buf[96];
    snprintf(buf, sizeof(buf), "FAT32 · 已用 %.1f GB / 共 %.1f GB · %d%%",
             (double)used_kb / (1024.0 * 1024.0),
             (double)total_kb / (1024.0 * 1024.0), pct);
    lv_label_set_text(s_sd_lb, buf);
    lv_bar_set_value(s_sd_bar, pct, LV_ANIM_OFF);
}

/* ===================== 动作 ===================== */

static void open_book(const char *path)
{
    /* 打开要把整本书读进来，是 SD 上的慢活，丢后台；这里立刻切页 */
    esp_err_t ret = srv_reader_open_async(path);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "打开失败 %s: %s", path, esp_err_to_name(ret));
        ui_alert_show("打开失败", UI_COLOR_WARN,
                      (ret == ESP_ERR_INVALID_STATE) ? "上一本书还在打开，稍等一下"
                                                     : esp_err_to_name(ret),
                      "知道了", NULL, NULL, 2200);
        return;
    }
    app_navigator_switch(PAGE_READER, true);
}

static void act_add_book(const char *path)
{
    esp_err_t ret = srv_library_add_external(path);
    if (ret == ESP_ERR_INVALID_ARG) {
        ui_alert_show("加不进去", UI_COLOR_WARN,
                      (strncmp(path, srv_library_root(), strlen(srv_library_root())) == 0)
                          ? "已经在书架目录里了"
                          : "只支持 .txt 文件或 .ebf 文件夹",
                      "知道了", NULL, NULL, 2400);
        return;
    }
    if (ret != ESP_OK) {
        ui_alert_show("加入失败", UI_COLOR_WARN, esp_err_to_name(ret),
                      "知道了", NULL, NULL, 2200);
        return;
    }

    (void)srv_library_scan();
    ui_alert_show("已加入书架", UI_COLOR_OK, s_act_name, "知道了", NULL, NULL, 1800);
}

static void act_add_music(const char *path)
{
    esp_err_t ret = srv_musiclib_add_external(path);
    if (ret != ESP_OK) {
        ui_alert_show("加不进去", UI_COLOR_WARN,
                      (ret == ESP_ERR_INVALID_ARG) ? "不是受支持的音频文件"
                                                   : esp_err_to_name(ret),
                      "知道了", NULL, NULL, 2400);
        return;
    }

    (void)srv_musiclib_scan();
    ui_alert_show("已加入媒体库", UI_COLOR_OK, s_act_name, "知道了", NULL, NULL, 1800);
}

static void on_del_ok(void *arg)
{
    (void)arg;

    esp_err_t ret = srv_fs_remove_tree(s_act_path);
    if (ret != ESP_OK) {
        ui_alert_show("删除失败", UI_COLOR_WARN, esp_err_to_name(ret),
                      "知道了", NULL, NULL, 2200);
        return;
    }

    ESP_LOGI(TAG, "已删除 %s", s_act_path);

    /* 删掉的可能是书架/媒体库里那一条，两个都重扫（找不到就自己掉出去） */
    (void)srv_library_scan();
    (void)srv_musiclib_scan();

    scan_dir();
    files_reload();

    ui_alert_show("已删除", UI_COLOR_OK, s_act_name, "知道了", NULL, NULL, 1500);
}

static void act_delete(const char *path, const char *name, bool is_dir)
{
    /* ★ 前缀「文件夹连同里面的内容一起删除：」是 45 字节，加上文件名的 255 字节
     *   上限就是 300 字节，原先的 288（SRV_FS_NAME_MAX + 32）装不下：-O2 下
     *   GCC 的 -Wformat-truncation
     *   能算出来，而 IDF 开着 -Werror，于是直接变成编译错误。留到 +64 有富余，
     *   以后再改文案也不用重算。 */
    char note[SRV_FS_NAME_MAX + 64];
    snprintf(note, sizeof(note), "%s%s",
             is_dir ? "文件夹连同里面的内容一起删除：" : "", name);

    ui_dialog_confirm("删除后就找不回来了", note, "删除", on_del_ok, NULL);
    (void)path;                         /* 实际路径在 s_act_path 里（对话框回调只带一个 void*） */
}

/* ===================== 长按菜单 ===================== */

static void on_menu_cb(int idx, void *arg)
{
    (void)arg;

    if (idx < 0) {
        return;
    }
    if (idx == s_menu_open) {
        open_book(s_act_path);
    } else if (idx == s_menu_book) {
        act_add_book(s_act_path);
    } else if (idx == s_menu_music) {
        act_add_music(s_act_path);
    } else if (idx == s_menu_del) {
        act_delete(s_act_path, s_act_name, s_act_is_dir);
    }
}

static void show_menu(int idx)
{
    const file_item_t *it = &s_items[idx];

    snprintf(s_act_name, sizeof(s_act_name), "%s", it->name);
    s_act_is_dir = it->is_dir;
    s_act_is_ebf = it->is_ebf;
    if (srv_fs_join(s_cur, it->name, s_act_path, sizeof(s_act_path)) != ESP_OK) {
        return;
    }

    char ext[16];
    ext_of(s_act_path, ext, sizeof(ext));

    ui_menu_item_t items[UI_MENU_MAX_ITEMS];
    int n = 0;
    s_menu_open = s_menu_book = s_menu_music = s_menu_del = -1;

    if (is_book(s_act_is_dir, s_act_is_ebf, s_act_path)) {
        items[n] = (ui_menu_item_t){ "打开", &ic_book_open };
        s_menu_open = n++;
    }
    if (is_book(s_act_is_dir, s_act_is_ebf, s_act_path)) {
        items[n] = (ui_menu_item_t){ "加入书架", &ic_bookshelf };
        s_menu_book = n++;
    }
    if (!s_act_is_dir && is_audio_ext(ext)) {
        items[n] = (ui_menu_item_t){ "加入音乐库", &ic_music };
        s_menu_music = n++;
    }

    items[n] = (ui_menu_item_t){ "删除", &ic_delete };
    s_menu_del = n++;

    ui_menu_show(s_act_name, items, n, on_menu_cb, NULL);
}

static void on_row_long(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_count) {
        return;
    }
    s_menu_tick = lv_tick_get();
    show_menu(idx);
}

/* ===================== 行交互 ===================== */

static void on_row_click(lv_event_t *e)
{
    /* 长按松手补发的那个 CLICKED 要吞掉，否则菜单刚弹出来就执行了主动作 */
    if (s_menu_tick != 0 && lv_tick_elaps(s_menu_tick) < 800) {
        s_menu_tick = 0;
        return;
    }
    s_menu_tick = 0;

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_count) {
        return;
    }

    const file_item_t *it = &s_items[idx];
    char path[SRV_FS_PATH_MAX];
    if (srv_fs_join(s_cur, it->name, path, sizeof(path)) != ESP_OK) {
        return;
    }

    if (it->is_ebf) {
        open_book(path);                /* .ebf 目录本身就是一本书 */
        return;
    }
    if (it->is_dir) {
        snprintf(s_cur, sizeof(s_cur), "%s", path);
        scan_dir();
        files_reload();
        return;
    }

    char ext[16];
    ext_of(it->name, ext, sizeof(ext));
    if (strcmp(ext, "txt") == 0) {
        open_book(path);
        return;
    }
    if (is_audio_ext(ext)) {
        snprintf(s_act_name, sizeof(s_act_name), "%s", it->name);   /* 提示里要显示 */
        act_add_music(path);
        return;
    }

    ui_alert_show("打不开", UI_COLOR_WARN, "这个格式不能打开也不能播放",
                  "知道了", NULL, NULL, 1800);
}

/* ===================== 路径栏按钮 ===================== */

static void on_up(lv_event_t *e)
{
    (void)e;

    const char *root = srv_sd_mount_point();
    if (strcmp(s_cur, root) == 0) {
        return;
    }

    char parent[SRV_FS_PATH_MAX];
    srv_fs_parent(s_cur, parent, sizeof(parent));

    /* 别越过挂载点（挂载点是短相对路径时，父路径可能变成 "."） */
    if (strncmp(parent, root, strlen(root)) != 0) {
        snprintf(parent, sizeof(parent), "%s", root);
    }
    snprintf(s_cur, sizeof(s_cur), "%s", parent);

    scan_dir();
    files_reload();
}

static void on_reload(lv_event_t *e)
{
    (void)e;

    if (srv_sd_remount() != ESP_OK) {
        ui_alert_show("重载失败", UI_COLOR_WARN, "换张卡再试，或检查卡座",
                      "知道了", NULL, NULL, 2200);
    }

    /* 卡可能换过了，回到根目录最稳 */
    snprintf(s_cur, sizeof(s_cur), "%s", srv_sd_mount_point());
    (void)srv_library_scan();
    (void)srv_musiclib_scan();

    scan_dir();
    files_reload();
}

/* ===================== 构建 ===================== */

static void build_row(lv_obj_t *parent, int idx, lv_coord_t y)
{
    const file_item_t *it = &s_items[idx];

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_pos(row, 8, y);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(row, on_row_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);

    lv_obj_t *ic = lv_image_create(row);
    lv_image_set_src(ic, icon_for(it));
    lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT_DIM, 0);
    lv_image_set_scale(ic, 22 * 256 / 32);      /* 32px 图标缩到 22px */
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_style_text_color(nm, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(nm, &font_ui_14, 0);
    lv_obj_set_width(nm, 200);
    /* 高度必须限成一行：只设宽 + LONG_DOT 时，label 会按整段文字自己撑高，
     * 结果折成两行被行高裁掉（书名一长就露馅） */
    lv_obj_set_height(nm, font_ui_14.line_height);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_label_set_text(nm, it->name);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 36, 0);

    if (!it->is_dir) {
        char sz[16];
        fmt_size(it->size, sz, sizeof(sz));

        lv_obj_t *sl = lv_label_create(row);
        lv_obj_set_style_text_color(sl, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(sl, &font_ui_14, 0);
        lv_obj_set_width(sl, 56);
        lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(sl, sz);
        lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

/*! 清空列表并按当前目录重建所有行（页面内所有"内容变了"的地方都走它） */
static void files_reload(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list)) {
        return;
    }

    lv_obj_clean(s_list);

    if (!srv_sd_is_mounted()) {
        lv_obj_t *tip = lv_label_create(s_list);
        lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(tip, &font_ui_14, 0);
        lv_label_set_text(tip, "SD 卡未挂载");
        lv_obj_align(tip, LV_ALIGN_TOP_LEFT, 12, 8);
    } else if (s_count == 0) {
        lv_obj_t *tip = lv_label_create(s_list);
        lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(tip, &font_ui_14, 0);
        lv_label_set_text(tip, "空目录");
        lv_obj_align(tip, LV_ALIGN_TOP_LEFT, 12, 8);
    } else {
        lv_coord_t y = 0;
        for (int i = 0; i < s_count; i++) {
            build_row(s_list, i, y);
            y += ROW_H + ROW_GAP;
        }
    }

    update_path();
    update_sd();
}

static void on_list_delete(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 手快「返回又进来」时新列表已经建好了，旧回调不能把新指针清掉 */
    if (lv_event_get_target(e) != s_list) {
        return;
    }
    s_list    = NULL;
    s_path_lb = NULL;
    s_up_btn  = NULL;
    s_up_lb   = NULL;
    s_sd_lb   = NULL;
    s_sd_bar  = NULL;
}

void ui_files_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "文件管理", app_navigator_back_cb, NULL);

    /* ---- 路径栏：当前路径 + 「上级」「重载」 ---- */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, 304, PATH_H);
    lv_obj_set_pos(bar, 8, PATH_Y);
    lv_obj_set_style_bg_color(bar, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    s_path_lb = lv_label_create(bar);
    lv_obj_set_style_text_color(s_path_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_path_lb, &font_ui_14, 0);
    lv_obj_set_width(s_path_lb, 150);
    lv_label_set_long_mode(s_path_lb, LV_LABEL_LONG_DOT);
    lv_obj_align(s_path_lb, LV_ALIGN_LEFT_MID, 8, 0);

    s_up_lb  = ui_row_btn_create(bar, -8, 60, "上级", on_up, NULL);
    s_up_btn = lv_obj_get_parent(s_up_lb);
    ui_row_btn_create(bar, -72, 60, "重载", on_reload, NULL);

    /* ---- 文件列表（纵向滚动；横向滑动仍交给页面手势返回） ---- */
    s_list = lv_obj_create(root);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(s_list, UI_SCREEN_W, LIST_H);
    lv_obj_set_pos(s_list, 0, LIST_Y);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_bottom(s_list, ROW_GAP, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list, 2, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(s_list, on_list_delete, LV_EVENT_DELETE, NULL);

    /* ---- 底部：SD 卡占用 ---- */
    s_sd_bar = lv_bar_create(root);
    lv_obj_set_size(s_sd_bar, 304, 8);
    lv_obj_set_pos(s_sd_bar, 8, SD_BAR_Y);
    lv_bar_set_range(s_sd_bar, 0, 100);
    lv_bar_set_value(s_sd_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_sd_bar, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sd_bar, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_sd_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sd_bar, 4, LV_PART_INDICATOR);

    s_sd_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_sd_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_sd_lb, &font_ui_14, 0);
    lv_obj_set_width(s_sd_lb, 304);
    lv_label_set_long_mode(s_sd_lb, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_sd_lb, "");
    lv_obj_set_pos(s_sd_lb, 8, SD_TEXT_Y);

    /* ---- 每次进来都从挂载点根开始 ---- */
    snprintf(s_cur, sizeof(s_cur), "%s", srv_sd_mount_point());
    scan_dir();
    files_reload();

    ESP_LOGI(TAG, "文件管理就绪：%s（%d 项）", s_cur, s_count);
}
