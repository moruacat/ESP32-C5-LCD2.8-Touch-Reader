#include "ui_photos.h"
#include "ui_common.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_photo.h"
#include "srv_sd.h"

/* 全屏看图时要把缓存条目丢掉，见 fs_free() */
#include "misc/cache/instance/lv_image_cache.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_photos";

/* ===================== 布局（横屏 320x240） ===================== */

/* 列表用滚轮承载（同阅读器的章节列表）：几百张照片只建一个 LVGL 对象，
 * 逐行建对象的话 500 张就是 1500+ 个，内部 RAM 扛不住。 */
#define ROLLER_X    8
#define ROLLER_W    304
#define INDEX_Y     (UI_TITLEBAR_H + 2)
#define INDEX_H     22
#define ROLLER_Y    (INDEX_Y + INDEX_H + 4)
/*! 滚轮可见行数：高度由它和行距算出，不写死 */
#define ROLLER_ROWS 5

#define BTN_W       110
#define BTN_H       30

/* ===== 缩略图网格：一页 4x2 = 8 张，翻页切换 =====
 * 逐张缩略图从原图（320x240 RGB565）最近邻采样到 72x54，一页 8 张约 62KB PSRAM。
 * 只建 8 个图片对象，几百张照片也不会把 LVGL 对象数撑爆。 */
#define GRID_COLS   4
#define GRID_ROWS   2
#define GRID_PAGE   (GRID_COLS * GRID_ROWS)
#define CELL_W      76
#define CELL_H      66
#define THUMB_W     72
#define THUMB_H     54
#define GRID_Y      (ROLLER_Y)

/*! 预读任务的栈：里面有 fopen/fread（SD 卡驱动也要用栈） */
#define PRE_TASK_STACK  4096

/* ===================== 页面状态 ===================== */

static lv_obj_t  *s_root;           /*!< 页面根容器 */
static lv_obj_t  *s_roller;         /*!< 列表滚轮 */
static lv_obj_t  *s_index_btn;      /*!< 顶部序号按钮（点它快速跳转） */
static lv_obj_t  *s_index_lb;       /*!< 序号文字 "12 / 345" */
static lv_obj_t  *s_empty_lb;       /*!< 一张都没有时的提示 */
static lv_obj_t  *s_open_btn;       /*!< 「打开」按钮（没照片时隐藏） */
static lv_obj_t  *s_fs;             /*!< 全屏视图（盖住列表与标题栏） */
static lv_obj_t  *s_img;            /*!< 全屏里的图片 */
static lv_obj_t  *s_fs_name;        /*!< 全屏底部的文件名 + 序号 */

static uint8_t   *s_data;           /*!< 当前全屏照片的像素缓冲 */
static lv_image_dsc_t s_dsc;        /*!< 复用同一个 dsc，关掉时必须丢缓存 */
static int        s_idx;            /*!< 当前全屏的是第几张 */

/* 网格 */
static lv_obj_t      *s_grid;
static lv_obj_t      *s_grid_img[GRID_PAGE];
static lv_obj_t      *s_grid_no[GRID_PAGE];
static lv_image_dsc_t s_grid_dsc[GRID_PAGE];
static uint8_t       *s_grid_buf[GRID_PAGE];
static int            s_page;
static int            s_page_count;
static lv_obj_t      *s_page_lb;

static void grid_show_page(int page);   /* 全屏退出时要把网格翻到当前页 */

/* 滑动判定 */
static lv_point_t s_press_pt;
static bool       s_pressing;

/* ===================== 全屏视图 ===================== */

/* 丢缓存再还缓冲：缓存以 dsc 地址为键，存的是指向这块内存的指针，
 * 留在那儿下次同地址命中就成了野指针 */
static void fs_free(void)
{
    if (s_data != NULL) {
        lv_image_cache_drop(&s_dsc);
        free(s_data);
        s_data = NULL;
    }
}

/* ===================== 下一张预读 =====================
 *
 * SD 上读一张 150KB 要几十毫秒，同步读会让左右滑一顿一顿的。
 * 做法：当前这张显示完后起个任务把下一张读进内存，滑过去时直接拿来用；
 * 没命中（滑太快、或往回滑）就退回同步读，行为不变。
 */

static uint8_t      *s_pre_data = NULL;     /*!< 预读好的像素缓冲 */
static int           s_pre_idx  = -1;       /*!< 它是第几张 */
static volatile bool s_pre_busy = false;    /*!< 任务正在读 */
static volatile bool s_pre_alive = false;   /*!< 页面是否还在（销毁时置 false） */
/*! 页面代号：快速「退出→再进」时旧的预读任务还在跑，靠它把过期结果丢掉 */
static volatile uint32_t s_pre_gen = 0;

static void pre_task(void *arg)
{
    const uint32_t packed = (uint32_t)(intptr_t)arg;
    const uint32_t gen = packed >> 16;
    const int      idx = (int)(packed & 0xFFFFu);

    uint8_t *data  = NULL;
    size_t   bytes = 0;
    bool     ok    = (srv_photo_load(idx, &data, &bytes) == ESP_OK);

    if (ok && s_pre_alive && gen == s_pre_gen) {
        free(s_pre_data);               /* 旧结果没人要了；free(NULL) 是安全的 */
        s_pre_data = data;
        s_pre_idx  = idx;
    } else if (ok) {
        free(data);                     /* 页面早切走了（或已换了新页面），直接丢 */
    }

    s_pre_busy = false;
    vTaskDelete(NULL);
}

static void pre_start(int idx)
{
    if (!s_pre_alive || s_pre_busy || idx < 0 || idx >= srv_photo_count()) {
        return;
    }

    free(s_pre_data);                   /* 目标变了，旧的作废 */
    s_pre_data = NULL;
    s_pre_idx  = -1;

    s_pre_busy = true;
    const uint32_t packed = ((uint32_t)s_pre_gen << 16) | (uint32_t)(idx & 0xFFFF);
    if (xTaskCreate(pre_task, "photo_pre", PRE_TASK_STACK,
                    (void *)(intptr_t)packed, 3, NULL) != pdPASS) {
        s_pre_busy = false;
    }
}

/* 取预读结果：命中就把缓冲交给调用方（谁用谁 free），否则返回 false */
static bool pre_take(int idx, uint8_t **out)
{
    if (s_pre_data == NULL || s_pre_idx != idx) {
        return false;
    }
    *out       = s_pre_data;
    s_pre_data = NULL;
    s_pre_idx  = -1;
    return true;
}

/* 页面销毁：任务还在读就让它自己收尾（它看到 alive=false 会释放结果） */
static void pre_stop(void)
{
    s_pre_alive = false;
    if (!s_pre_busy) {
        free(s_pre_data);
        s_pre_data = NULL;
        s_pre_idx  = -1;
    }
}

/* 读第 idx 张并显示。idx 会被夹进合法范围，返回 false 表示没得显示 */
static bool fs_show(int idx)
{
    int total = srv_photo_count();
    if (total <= 0) {
        return false;
    }
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= total) {
        idx = total - 1;
    }

    uint8_t *data  = NULL;
    size_t   bytes = 0;
    if (pre_take(idx, &data)) {
        bytes = SRV_PHOTO_BYTES;            /* 预读的一定是整张 */
    } else if (srv_photo_load(idx, &data, &bytes) != ESP_OK) {
        return false;
    }

    fs_free();
    s_data = data;
    s_idx  = idx;

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w      = SRV_PHOTO_W;
    s_dsc.header.h      = SRV_PHOTO_H;
    s_dsc.header.stride = SRV_PHOTO_W * 2;      /* RGB565：每像素 2 字节 */
    s_dsc.data_size     = (uint32_t)bytes;
    s_dsc.data          = data;

    /* 转换脚本已把图缩到 320x240 并居中留黑边，这里 1:1 贴上去即可 */
    lv_image_set_src(s_img, &s_dsc);

    const srv_photo_t *p = NULL;
    if (srv_photo_get(idx, &p) == ESP_OK) {
        /* 文件名用 %.*s 限长：p->name 是 256 字节缓冲，直接 %s 拼数字会被
         * -Werror=format-truncation 拦下（编译器按最坏情况算长度） */
        char buf[SRV_PHOTO_NAME_MAX + 16];
        snprintf(buf, sizeof(buf), "%.*s   %d/%d",
                 SRV_PHOTO_NAME_MAX - 24, p->name, idx + 1, total);
        lv_label_set_text(s_fs_name, buf);
    }

    /* 显示完这张，顺手把下一张读进内存（最后一页就没有下一张了） */
    pre_start(idx + 1);
    return true;
}

static void fs_exit(void)
{
    lv_obj_add_flag(s_fs, LV_OBJ_FLAG_HIDDEN);
    fs_free();

    /* 全屏里左右滑过之后，网格要翻到当前这张所在的页 */
    if (srv_photo_count() > 0) {
        grid_show_page(s_idx / GRID_PAGE);
    }
}

/* 全屏下的手势：左右滑切换照片，轻点退回列表 */
static void fs_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_indev_get_point(indev, &s_press_pt);
    s_pressing = true;
}

static void fs_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pressing) {
        return;
    }
    s_pressing = false;

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int dx = p.x - s_press_pt.x;
    int dy = p.y - s_press_pt.y;

    /* 横向为主且够远 → 切换照片 */
    if (abs(dx) >= UI_SWIPE_MIN_PX && abs(dx) >= abs(dy) * 2) {
        if (dx < 0) {
            if (s_idx + 1 < srv_photo_count()) {
                fs_show(s_idx + 1);
            }
        } else {
            if (s_idx > 0) {
                fs_show(s_idx - 1);
            }
        }
        return;
    }

    /* 没滑动就是轻点：退回列表 */
    fs_exit();
}

/* ===================== 列表视图 ===================== */

/* 滚轮的选项文本缓冲：几百张照片的名字拼起来（每行一个），
 * 关掉页面时释放。滚轮内部会自己存一份。 */
static char *s_opts;

static void opts_free(void)
{
    free(s_opts);
    s_opts = NULL;
}

/* 滚轮里显示第几项 */
static int roller_sel(void)
{
    if (s_roller == NULL || !lv_obj_is_valid(s_roller)) {
        return 0;
    }
    uint32_t sel = lv_roller_get_selected(s_roller);
    int total = srv_photo_count();
    if ((int)sel >= total) {
        return total > 0 ? total - 1 : 0;
    }
    return (int)sel;
}

/* 按 UTF-8 码点截断，超出加省略号（roller 行不换行，按字节硬截会切出半个汉字） */
/* 把第 idx 张的像素读出来显示（列表侧的入口） */
static void open_index(int idx)
{
    if (!fs_show(idx)) {
        ui_alert_show("打不开照片", UI_COLOR_WARN, "文件损坏或不是本工具转出来的 .raw",
                      "知道了", NULL, NULL, 2400);
        return;
    }
    lv_obj_remove_flag(s_fs, LV_OBJ_FLAG_HIDDEN);
}

/* 滚轮定位到哪一张，点「打开」就开哪张 */
static void on_open_clicked(lv_event_t *e)
{
    (void)e;
    open_index(roller_sel());
}

/* ===================== 缩略图网格 ===================== */

static void open_index(int idx);        /* 实现在全屏视图一节后面 */

static void grid_free(void)
{
    for (int i = 0; i < GRID_PAGE; i++) {
        if (s_grid_buf[i] != NULL) {
            lv_image_cache_drop(&s_grid_dsc[i]);
            free(s_grid_buf[i]);
            s_grid_buf[i] = NULL;
        }
    }
}

/* 最近邻采样：320x240 RGB565 -> 72x54 */
static void thumb_make(const uint8_t *src, uint8_t *dst)
{
    for (int y = 0; y < THUMB_H; y++) {
        const int sy = y * SRV_PHOTO_H / THUMB_H;
        const uint8_t *row = src + (size_t)sy * SRV_PHOTO_W * 2;
        for (int x = 0; x < THUMB_W; x++) {
            const int sx = x * SRV_PHOTO_W / THUMB_W;
            const uint8_t *p = row + sx * 2;
            *dst++ = p[0];
            *dst++ = p[1];
        }
    }
}

static void grid_fill_page(int page)
{
    const int total = srv_photo_count();
    const int base = page * GRID_PAGE;

    for (int i = 0; i < GRID_PAGE; i++) {
        const int idx = base + i;
        if (idx >= total) {
            lv_obj_add_flag(s_grid_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_grid_no[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        if (s_grid_buf[i] == NULL) {
            s_grid_buf[i] = heap_caps_malloc(THUMB_W * THUMB_H * 2,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_grid_buf[i] == NULL) {
                s_grid_buf[i] = malloc(THUMB_W * THUMB_H * 2);
            }
        }
        if (s_grid_buf[i] == NULL) {
            lv_obj_add_flag(s_grid_img[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        uint8_t *src = NULL;
        size_t   bytes = 0;
        if (srv_photo_load(idx, &src, &bytes) != ESP_OK) {
            lv_obj_add_flag(s_grid_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_grid_no[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        thumb_make(src, s_grid_buf[i]);
        free(src);

        lv_image_cache_drop(&s_grid_dsc[i]);
        memset(&s_grid_dsc[i], 0, sizeof(s_grid_dsc[i]));
        s_grid_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_grid_dsc[i].header.cf     = LV_COLOR_FORMAT_RGB565;
        s_grid_dsc[i].header.w      = THUMB_W;
        s_grid_dsc[i].header.h      = THUMB_H;
        s_grid_dsc[i].header.stride = THUMB_W * 2;
        s_grid_dsc[i].data_size     = THUMB_W * THUMB_H * 2;
        s_grid_dsc[i].data          = s_grid_buf[i];
        lv_image_set_src(s_grid_img[i], &s_grid_dsc[i]);
        lv_obj_remove_flag(s_grid_img[i], LV_OBJ_FLAG_HIDDEN);

        char buf[16];
        snprintf(buf, sizeof(buf), "%d", idx + 1);
        lv_label_set_text(s_grid_no[i], buf);
        lv_obj_remove_flag(s_grid_no[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void grid_show_page(int page)
{
    const int total = srv_photo_count();
    int pages = (total + GRID_PAGE - 1) / GRID_PAGE;
    if (pages < 1) {
        pages = 1;
    }
    if (page < 0) {
        page = 0;
    }
    if (page >= pages) {
        page = pages - 1;
    }

    s_page = page;
    s_page_count = pages;
    grid_fill_page(page);

    if (s_page_lb != NULL && lv_obj_is_valid(s_page_lb)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "第 %d / %d 页", page + 1, pages);
        lv_label_set_text(s_page_lb, buf);
    }
}

static void on_grid_cell(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    const int idx = s_page * GRID_PAGE + i;
    if (idx >= 0 && idx < srv_photo_count()) {
        open_index(idx);
    }
}

static void on_page_prev(lv_event_t *e)
{
    (void)e;
    grid_show_page(s_page - 1);
}

static void on_page_next(lv_event_t *e)
{
    (void)e;
    grid_show_page(s_page + 1);
}

/* 顶部序号：滚轮选中项一变就更新 */
static void update_index(void)
{
    if (s_index_lb == NULL || !lv_obj_is_valid(s_index_lb)) {
        return;
    }
    int total = srv_photo_count();
    char buf[32];
    if (total <= 0) {
        snprintf(buf, sizeof(buf), "0 / 0");
    } else {
        snprintf(buf, sizeof(buf), "%d / %d", roller_sel() + 1, total);
    }
    lv_label_set_text(s_index_lb, buf);
}

static void on_roller_changed(lv_event_t *e)
{
    (void)e;
    update_index();
}

/* 点滚轮里的选中项直接打开。滚动拖动时 LVGL 不会发 SHORT_CLICKED，
 * 所以不会出现「一滑就自动打开照片」；下方「打开」按钮仍保留。 */
static void on_roller_short_clicked(lv_event_t *e)
{
    (void)e;
    if (srv_photo_count() > 0) {
        open_index(roller_sel());
    }
}

/* 点顶部序号：输入数字直接跳过去（几百张时比一直滑快得多） */
static char s_jump_title[48];

static void on_jump_done(const char *text, void *arg)
{
    (void)arg;
    if (text == NULL || srv_photo_count() <= 0) {
        return;                         /* 键盘还开着时页面被切走了 */
    }
    grid_show_page(atoi(text) - 1);
}

static void on_index_clicked(lv_event_t *e)
{
    (void)e;
    const int total = srv_photo_count();
    if (total <= 0) {
        return;
    }
    const int pages = (total + GRID_PAGE - 1) / GRID_PAGE;
    snprintf(s_jump_title, sizeof(s_jump_title), "跳到第几页（1-%d）", pages);
    ui_text_input_show(s_jump_title, 4, on_jump_done, NULL);
}

static void list_build(void)
{
    opts_free();

    const int total = srv_photo_count();

    /* 一张都没有：网格、序号、翻页全藏起来，显示提示 */
    if (total <= 0) {
        lv_obj_add_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_index_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_open_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_page_lb != NULL) {
            lv_obj_add_flag(s_page_lb, LV_OBJ_FLAG_HIDDEN);
        }

        lv_label_set_text(s_empty_lb, srv_sd_is_mounted()
                                          ? "没有照片\n把转换好的 .raw 拷进 SD 卡的 /photos"
                                          : "SD 卡未挂载");
        lv_obj_remove_flag(s_empty_lb, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(s_empty_lb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_roller, LV_OBJ_FLAG_HIDDEN);   /* 滚轮退役，网格接管 */
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_index_btn, LV_OBJ_FLAG_HIDDEN);
    /* 「打开」按钮不显示：网格点格子直接打开 */
    lv_obj_add_flag(s_open_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_page_lb != NULL) {
        lv_obj_remove_flag(s_page_lb, LV_OBJ_FLAG_HIDDEN);
    }

    grid_show_page(s_page);
}

/* ===================== 页面 ===================== */

static void photos_rescan_apply(void *unused)
{
    (void)unused;
    /* 页面已经切走就不管了。delete_async 还没执行时 s_root 仍然有效，
     * lv_obj_is_valid() 能兜住那一段，而且 LVGL 是单线程执行，不会打架。 */
    if (s_root == NULL || !lv_obj_is_valid(s_root) || srv_photo_busy()) {
        return;
    }
    list_build();
}

/* 后台扫描扫完了（这个回调跑在扫描任务里，别在这儿碰 LVGL）：
 * 转给 LVGL 线程去按新表重建列表 */
static void photos_rescan_done(void)
{
    lv_async_call(photos_rescan_apply, NULL);
}

static void photos_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 手快「切走又切回」时新页面已经建好，旧回调别把新指针清掉 */
    if (lv_event_get_target(e) != s_root) {
        return;
    }

    fs_free();
    grid_free();
    opts_free();
    pre_stop();
    s_root     = NULL;
    s_roller   = NULL;
    s_grid     = NULL;
    s_page_lb  = NULL;
    for (int i = 0; i < GRID_PAGE; i++) {
        s_grid_img[i] = NULL;
        s_grid_no[i]  = NULL;
    }
    s_index_btn = NULL;
    s_index_lb = NULL;
    s_empty_lb = NULL;
    s_open_btn = NULL;
    s_fs       = NULL;
    s_img      = NULL;
    s_fs_name  = NULL;
}

void ui_photos_create(lv_obj_t *root)
{
    s_root = root;

    ui_titlebar_create(root, "照片", app_navigator_back_smart_cb, NULL);

    /* ===== 顶部当前序号：点它输入数字直接跳转 ===== */
    s_index_btn = lv_button_create(root);
    lv_obj_set_size(s_index_btn, 132, INDEX_H);
    lv_obj_align(s_index_btn, LV_ALIGN_TOP_MID, 0, INDEX_Y);
    lv_obj_set_style_radius(s_index_btn, 6, 0);
    lv_obj_set_style_bg_color(s_index_btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(s_index_btn, 0, 0);
    lv_obj_set_style_pad_all(s_index_btn, 0, 0);
    ui_add_click_feedback(s_index_btn);
    lv_obj_add_event_cb(s_index_btn, on_index_clicked, LV_EVENT_CLICKED, NULL);

    s_index_lb = lv_label_create(s_index_btn);
    lv_obj_set_style_text_font(s_index_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_index_lb, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_index_lb, "0 / 0");
    lv_obj_center(s_index_lb);

    /* ===== 列表视图：一个滚轮承载全部照片（同阅读器章节列表的思路） ===== */
    s_roller = lv_roller_create(root);
    lv_obj_set_width(s_roller, ROLLER_W);
    lv_obj_set_pos(s_roller, ROLLER_X, ROLLER_Y);
    lv_obj_set_style_bg_color(s_roller, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(s_roller, 0, 0);
    lv_obj_set_style_radius(s_roller, 8, 0);
    lv_obj_set_style_pad_all(s_roller, 0, 0);
    lv_obj_set_style_text_font(s_roller, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_roller, UI_COLOR_TEXT_DIM, 0);
    /* 行距要在设可见行数之前给好：那个函数靠它算高度 */
    lv_obj_set_style_text_line_space(s_roller, 10, 0);
    lv_roller_set_visible_row_count(s_roller, ROLLER_ROWS);
    lv_obj_set_style_bg_color(s_roller, UI_COLOR_PRIMARY, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_radius(s_roller, 6, LV_PART_SELECTED);
    lv_obj_add_event_cb(s_roller, on_roller_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_roller, on_roller_short_clicked, LV_EVENT_SHORT_CLICKED, NULL);

    /* ===== 缩略图网格（滚轮隐藏，网格接管） ===== */
    s_grid = lv_obj_create(root);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_grid, UI_SCREEN_W, GRID_ROWS * CELL_H + 4);
    lv_obj_set_pos(s_grid, 0, GRID_Y);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < GRID_PAGE; i++) {
        const int col = i % GRID_COLS;
        const int row = i / GRID_COLS;

        lv_obj_t *cell = lv_button_create(s_grid);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_pos(cell, 4 + col * (CELL_W + 4), 2 + row * (CELL_H + 4));
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_bg_color(cell, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        ui_add_click_feedback(cell);
        lv_obj_add_event_cb(cell, on_grid_cell, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        s_grid_img[i] = lv_image_create(cell);
        lv_obj_center(s_grid_img[i]);
        lv_obj_add_flag(s_grid_img[i], LV_OBJ_FLAG_HIDDEN);

        s_grid_no[i] = lv_label_create(cell);
        lv_obj_set_style_text_font(s_grid_no[i], &font_ui_14, 0);
        lv_obj_set_style_text_color(s_grid_no[i], UI_COLOR_TEXT, 0);
        lv_obj_set_style_bg_color(s_grid_no[i], UI_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(s_grid_no[i], LV_OPA_60, 0);
        lv_obj_set_style_pad_hor(s_grid_no[i], 3, 0);
        lv_obj_set_style_radius(s_grid_no[i], 3, 0);
        lv_obj_align(s_grid_no[i], LV_ALIGN_BOTTOM_RIGHT, -2, -2);
        lv_label_set_text(s_grid_no[i], "");
        lv_obj_add_flag(s_grid_no[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 底部翻页栏：上一页 / 页码 / 下一页 */
    lv_obj_t *prev = lv_button_create(root);
    lv_obj_set_size(prev, 56, 24);
    lv_obj_align(prev, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_obj_set_style_radius(prev, 6, 0);
    lv_obj_set_style_bg_color(prev, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_set_style_pad_all(prev, 0, 0);
    ui_add_click_feedback(prev);
    lv_obj_add_event_cb(prev, on_page_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_lb = lv_label_create(prev);
    lv_obj_set_style_text_font(prev_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(prev_lb, UI_COLOR_TEXT, 0);
    lv_label_set_text(prev_lb, "上一页");
    lv_obj_center(prev_lb);

    lv_obj_t *next = lv_button_create(root);
    lv_obj_set_size(next, 56, 24);
    lv_obj_align(next, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
    lv_obj_set_style_radius(next, 6, 0);
    lv_obj_set_style_bg_color(next, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_set_style_pad_all(next, 0, 0);
    ui_add_click_feedback(next);
    lv_obj_add_event_cb(next, on_page_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_lb = lv_label_create(next);
    lv_obj_set_style_text_font(next_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(next_lb, UI_COLOR_TEXT, 0);
    lv_label_set_text(next_lb, "下一页");
    lv_obj_center(next_lb);

    s_page_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_page_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_page_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_align(s_page_lb, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_label_set_text(s_page_lb, "第 1 / 1 页");
    lv_obj_add_flag(s_page_lb, LV_OBJ_FLAG_HIDDEN);

    /* 「打开」按钮：网格直接点格子打开，这个按钮保留但隐藏（兼容老逻辑） */
    s_open_btn = lv_button_create(root);
    lv_obj_set_size(s_open_btn, BTN_W, BTN_H);
    lv_obj_align(s_open_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(s_open_btn, 8, 0);
    lv_obj_set_style_shadow_width(s_open_btn, 0, 0);
    lv_obj_set_style_bg_color(s_open_btn, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_pad_all(s_open_btn, 0, 0);
    ui_add_click_feedback(s_open_btn);
    lv_obj_add_event_cb(s_open_btn, on_open_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lb = lv_label_create(s_open_btn);
    lv_obj_set_style_text_font(btn_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(btn_lb, lv_color_white(), 0);
    lv_label_set_text(btn_lb, "打开");
    lv_obj_center(btn_lb);

    /* 空列表提示：建好放着，list_build() 按情况显示/隐藏。
     * 建在全屏层之前，这样全屏看图时它不会盖在照片上面。 */
    s_empty_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_empty_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_empty_lb, &font_ui_14, 0);
    lv_obj_set_style_text_align(s_empty_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty_lb, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_empty_lb, LV_OBJ_FLAG_HIDDEN);

    /* ===== 全屏视图：默认隐藏，打开照片才盖上来 ===== */
    s_fs = lv_obj_create(root);
    lv_obj_remove_flag(s_fs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_fs, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_fs, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_fs, 0, 0);
    lv_obj_set_style_bg_color(s_fs, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_fs, LV_OPA_COVER, 0);   /* 全屏看图，底下一点都不露 */
    lv_obj_set_style_border_width(s_fs, 0, 0);
    lv_obj_set_style_radius(s_fs, 0, 0);
    lv_obj_set_style_pad_all(s_fs, 0, 0);
    lv_obj_add_flag(s_fs, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_fs, fs_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_fs, fs_released_cb, LV_EVENT_RELEASED, NULL);

    s_img = lv_image_create(s_fs);
    lv_obj_center(s_img);                             /* 320x240 正好铺满，不用缩放 */

    /* 底部文件名 + 序号：叠在图上，加半透明底免得白底照片上看不清 */
    s_fs_name = lv_label_create(s_fs);
    lv_obj_set_style_text_font(s_fs_name, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_fs_name, UI_COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(s_fs_name, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_fs_name, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_fs_name, 3, 0);
    lv_obj_set_style_radius(s_fs_name, 4, 0);
    lv_obj_set_width(s_fs_name, UI_SCREEN_W - 16);
    lv_label_set_long_mode(s_fs_name, LV_LABEL_LONG_DOT);
    lv_obj_align(s_fs_name, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_add_event_cb(root, photos_root_del_cb, LV_EVENT_DELETE, NULL);

    /* 预读标志要在建完 UI 之后打开：任务起来得先确认页面还在。
     * 代号先自增：上一次退出时可能还有预读任务在跑，它的结果按代号丢掉。 */
    s_pre_gen++;
    s_pre_alive = true;

    /* 先用现有条目把列表显示出来（进页面不再等扫描） */
    list_build();

    /* SD 可能在别处被重挂过，再到后台重扫一遍；扫完回调里重建列表。
     * 扫描期间条目表在变，界面这边不会再读它（列表还是上面那份）。 */
    if (srv_photo_scan_async(photos_rescan_done) != ESP_OK) {
        ESP_LOGW(TAG, "后台扫描启动失败，列表可能不是最新的");
    }
}