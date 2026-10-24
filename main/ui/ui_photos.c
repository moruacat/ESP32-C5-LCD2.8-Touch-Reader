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
#define ROLLER_Y    30
/*! 滚轮可见行数：高度由它和行距算出，不写死 */
#define ROLLER_ROWS 5

#define BTN_W       110
#define BTN_H       30

/*! 预读任务的栈：里面有 fopen/fread（SD 卡驱动也要用栈） */
#define PRE_TASK_STACK  4096

/* ===================== 页面状态 ===================== */

static lv_obj_t  *s_root;           /*!< 页面根容器 */
static lv_obj_t  *s_roller;         /*!< 列表滚轮 */
static lv_obj_t  *s_empty_lb;       /*!< 一张都没有时的提示 */
static lv_obj_t  *s_open_btn;       /*!< 「打开」按钮（没照片时隐藏） */
static lv_obj_t  *s_fs;             /*!< 全屏视图（盖住列表与标题栏） */
static lv_obj_t  *s_img;            /*!< 全屏里的图片 */
static lv_obj_t  *s_fs_name;        /*!< 全屏底部的文件名 + 序号 */

static uint8_t   *s_data;           /*!< 当前全屏照片的像素缓冲 */
static lv_image_dsc_t s_dsc;        /*!< 复用同一个 dsc，关掉时必须丢缓存 */
static int        s_idx;            /*!< 当前全屏的是第几张 */

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

static void pre_task(void *arg)
{
    int idx = (int)(intptr_t)arg;

    uint8_t *data  = NULL;
    size_t   bytes = 0;
    bool     ok    = (srv_photo_load(idx, &data, &bytes) == ESP_OK);

    if (ok && s_pre_alive) {
        free(s_pre_data);               /* 旧结果没人要了；free(NULL) 是安全的 */
        s_pre_data = data;
        s_pre_idx  = idx;
    } else if (ok) {
        free(data);                     /* 页面早切走了，直接丢 */
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
    if (xTaskCreate(pre_task, "photo_pre", PRE_TASK_STACK,
                    (void *)(intptr_t)idx, 3, NULL) != pdPASS) {
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

    /* 全屏里左右滑过之后，列表滚轮要跟着停在当前这张上 */
    if (s_roller != NULL && lv_obj_is_valid(s_roller) && srv_photo_count() > 0) {
        lv_roller_set_selected(s_roller, (uint32_t)s_idx, LV_ANIM_OFF);
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

static void list_build(void)
{
    opts_free();

    int total = srv_photo_count();

    /* 一张都没有：把滚轮和按钮藏起来，显示提示 */
    if (total <= 0) {
        lv_obj_add_flag(s_roller, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_open_btn, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(s_empty_lb, srv_sd_is_mounted()
                                          ? "没有照片\n把转换好的 .raw 拷进 SD 卡的 /photos"
                                          : "SD 卡未挂载");
        lv_obj_remove_flag(s_empty_lb, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(s_empty_lb, LV_OBJ_FLAG_HIDDEN);

    /* 两趟走：先量总长再分配，比"按每项预留 N 字节"准 */
    size_t cap = 8;
    for (int i = 0; i < total; i++) {
        const srv_photo_t *p = NULL;
        if (srv_photo_get(i, &p) == ESP_OK) {
            cap += strlen(p->name) + 16;    /* 名字 + "123/456  " + 换行 */
        }
    }

    /* 选项文本可能上百 KB → 先试 PSRAM */
    s_opts = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_opts == NULL) {
        s_opts = malloc(cap);
    }
    if (s_opts == NULL) {
        ESP_LOGW(TAG, "滚轮选项分配失败（%u 字节）", (unsigned)cap);
        return;
    }

    size_t pos = 0;
    for (int i = 0; i < total; i++) {
        const srv_photo_t *p = NULL;
        if (srv_photo_get(i, &p) != ESP_OK) {
            continue;
        }
        /* 序号放前面，滚动时一眼能看出是第几张 */
        pos += (size_t)snprintf(s_opts + pos, cap - pos, "%d/%d  %s\n",
                                i + 1, total, p->name);
    }
    if (pos > 0) {
        s_opts[pos - 1] = '\0';             /* 去掉末尾换行 */
    }

    /* 注意：不能用 lv_obj_clean(s_roller) 重建 —— 滚轮自身就是那个对象，
     * 清子对象会把它一起弄坏。换选项走 lv_roller_set_options 即可。 */
    lv_roller_set_options(s_roller, s_opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_roller, 0, LV_ANIM_OFF);

    lv_obj_remove_flag(s_roller, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_open_btn, LV_OBJ_FLAG_HIDDEN);
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
    opts_free();
    pre_stop();
    s_root     = NULL;
    s_roller   = NULL;
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

    /* 「打开」按钮：滚轮定位到哪一张就开哪张 */
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

    /* 预读标志要在建完 UI 之后打开：任务起来得先确认页面还在 */
    s_pre_alive = true;

    /* 先用现有条目把列表显示出来（进页面不再等扫描） */
    list_build();

    /* SD 可能在别处被重挂过，再到后台重扫一遍；扫完回调里重建列表。
     * 扫描期间条目表在变，界面这边不会再读它（列表还是上面那份）。 */
    if (srv_photo_scan_async(photos_rescan_done) != ESP_OK) {
        ESP_LOGW(TAG, "后台扫描启动失败，列表可能不是最新的");
    }
}