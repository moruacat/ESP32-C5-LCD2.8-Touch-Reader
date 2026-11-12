#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"
#include "srv_safe.h"
#include "srv_sd.h"
#include "srv_wifi.h"
#include "srv_library.h"
#include "srv_musiclib.h"
#include "srv_player.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"

static const char *TAG = "ui_about";

static lv_obj_t *s_sd_lb;
static lv_obj_t *s_pop_lb;      /*!< 「弹出」按钮文字 */

static void about_refresh(void)
{
    bool mounted = srv_sd_is_mounted();

    lv_label_set_text(s_sd_lb, mounted ? "已挂载" : "未挂载");
    lv_obj_set_style_text_color(s_sd_lb, mounted ? UI_COLOR_OK : UI_COLOR_TEXT_DIM, 0);

    /* 没挂载就没什么可弹的 */
    lv_obj_t *pop = lv_obj_get_parent(s_pop_lb);
    if (mounted) {
        lv_obj_remove_state(pop, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(pop, LV_STATE_DISABLED);
    }
}

/* ===================== 交互 ===================== */

static void on_pop_ok(void *arg)
{
    (void)arg;
    srv_sd_unmount();
    /* 卡拔了，书架与媒体库里的条目已经失效，直接清空重扫（会得到空表） */
    (void)srv_library_scan();
    (void)srv_musiclib_scan();
    about_refresh();
}

static void on_pop_click(lv_event_t *e)
{
    (void)e;
    ui_dialog_confirm("弹出 SD 卡？", "弹出后书籍与设置都无法读写",
                      "弹出", on_pop_ok, NULL);
}

static void on_remount_click(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = srv_sd_remount();

    /* 重新挂上的可能是另一张卡：书架与媒体库都要重扫，
     * 否则界面里还留着上一张卡的条目（点进去就是"文件不存在"）。 */
    if (ret == ESP_OK) {
        /* 换了卡，正在放的那首文件已经不在了，先停下来 */
        if (srv_player_state() != SRV_PLAYER_IDLE) {
            (void)srv_player_stop();
        }
        (void)srv_library_scan();
        (void)srv_musiclib_scan();

        /* 开机时 SD 就没插的话，上次的播放列表/曲目是这时候才恢复得回来的
         * （srv_player_restore 在 app_init 里跑的时候媒体库还是空的） */
        if (srv_player_playlist_id() == 0) {
            (void)srv_player_restore();
        }
    } else {
        ESP_LOGW(TAG, "重新挂载 SD 失败: %s", esp_err_to_name(ret));
    }
    about_refresh();
}

static void on_reset_ok(void *arg)
{
    (void)arg;

    srv_config_reset();
    /* 配置里的开关要同步到硬件，否则界面显示的和实际状态对不上 */
    srv_wifi_set_enabled(srv_config_get_int(CFG_WIFI_EN, 0) != 0);
    bsp_display_brightness_set((int)srv_config()->brightness);

    /* 恢复后本页显示的内容（SD 状态、设备名）以及上一级的各项状态都变了，
     * 直接退回设置页，让它重建一次 */
    app_navigator_back();
}

static void on_reset_click(lv_event_t *e)
{
    (void)e;
    ui_dialog_confirm("重置全部设置？", "网络、时间、闹钟、阅读等全部恢复默认值",
                      "重置", on_reset_ok, NULL);
}

/* ---- 快速返回主页 / 安全模式（从设置主页挪过来） ---- */

static void on_fast_home_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    srv_config_set_int(CFG_FAST_HOME, on ? 1 : 0, true);
}

static void on_safe_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    const bool want = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (want == srv_safe_on()) {
        return;                     /* 程序同步状态时触发的，不是用户改的 */
    }

    const bool on = srv_safe_toggle();
    ui_alert_show(on ? "安全模式已开启（重启后保持）" : "安全模式已关闭",
                  on ? UI_COLOR_OK : UI_COLOR_TEXT_DIM,
                  on ? "阅读 / 音乐 / 文件 / 照片密码\n快速返回主页 已收起"
                     : "普通设置恢复完整",
                  "知道了", NULL, NULL, on ? 3200 : 1800);
}

/* ===================== 页面 ===================== */

void ui_set_about_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "关于", app_navigator_back_cb, NULL);
    lv_obj_t *list = ui_settings_list_create(root);

    /* 只有「安全菜单 → 完整版设置 → 关于」这条路才显示安全模式开关，
     * 普通设置的关于页保持隐藏（安全模式入口不应该随便露出来） */
    const bool show_safe = (app_navigator_prev() == PAGE_SETTINGS_FULL);

    lv_obj_t *row = ui_row_create(list, UI_ROW_Y(0), &ic_SD, "SD 卡");
    s_sd_lb = ui_row_value_create(row, -12, 90);

    row = ui_row_create(list, UI_ROW_Y(1), &ic_info, "设备名");
    lv_obj_t *dev = ui_row_value_create(row, -12, 160);
    lv_obj_set_style_text_color(dev, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(dev, esp_app_get_description()->project_name);

    row = ui_row_create(list, UI_ROW_Y(2), NULL, "弹出 SD 卡");
    s_pop_lb = ui_row_btn_create(row, -8, 64, "弹出", on_pop_click, NULL);

    row = ui_row_create(list, UI_ROW_Y(3), NULL, "重载 SD 卡");
    ui_row_btn_create(row, -8, 64, "重载", on_remount_click, NULL);

    row = ui_row_create(list, UI_ROW_Y(4), &ic_home, "快速返回主页");
    (void)ui_row_switch_create(row, srv_config_get_int(CFG_FAST_HOME, CFG_DEF_FAST_HOME) != 0,
                              on_fast_home_switch, NULL);

    int next = 5;
    if (show_safe) {
        row = ui_row_create(list, UI_ROW_Y(next++), &ic_warning, "安全模式");
        (void)ui_row_switch_create(row, srv_safe_on(), on_safe_switch, NULL);
    }

    /* 危险操作：文字标红，点了还要再确认一次 */
    row = ui_row_create(list, UI_ROW_Y(next), &ic_warning, "重置全部设置");
    lv_obj_t *lb = ui_row_btn_create(row, -8, 64, "重置", on_reset_click, NULL);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEMP, 0);

    about_refresh();
}
