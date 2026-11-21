#include "ui_font.h"

#include "srv_sd.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#ifdef ESP_PLATFORM
#include "esp_spiffs.h"     /* IDF 6.x：esp_vfs_spiffs.h 已并入 esp_spiffs.h */
#endif

#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ui_font";

/* 字号档位：14 内嵌，其余从字库加载。新增档位时同步运行
 * tools/gen_read_fonts.py 并把 font/font_ui_<px>.bin 打进 font 分区
 * （或拷到 SD 卡 /fonts）。 */
static const int s_sizes[] = { 14, 16, 18, 20, 22 };
#define SIZE_COUNT  (int)(sizeof(s_sizes) / sizeof(s_sizes[0]))

/*! Flash 里的字库分区（由 main/CMakeLists.txt 打包） */
#define FONT_FLASH_BASE     "/fontflash"
#define FONT_FLASH_PART     "font"

extern const lv_font_t font_ui_14;

static lv_font_t *s_loaded = NULL;      /*!< 动态加载的字体（NULL = 只用 14px） */
static int        s_loaded_px = 0;

#ifdef ESP_PLATFORM
static bool s_flash_mounted = false;
static bool s_flash_tried = false;

static bool font_flash_mount(void)
{
    if (s_flash_mounted) {
        return true;
    }
    if (s_flash_tried) {
        return false;
    }
    s_flash_tried = true;

    esp_vfs_spiffs_conf_t conf = {
        .base_path              = FONT_FLASH_BASE,
        .partition_label        = FONT_FLASH_PART,
        .max_files              = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "字库分区不可用（%s），只用 SD 卡 /fonts", esp_err_to_name(ret));
        return false;
    }
    s_flash_mounted = true;
    ESP_LOGI(TAG, "字库分区已挂载到 %s", FONT_FLASH_BASE);
    return true;
}
#endif

static void make_sd_path(int px, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/fonts/font_ui_%d.bin", srv_sd_mount_point(), px);
}

static void make_flash_path(int px, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/font_ui_%d.bin", FONT_FLASH_BASE, px);
}

/* 找一个实际存在的字库文件：SD 卡优先，其次 Flash 分区 */
static bool find_font_path(int px, char *out, size_t out_len)
{
    if (px <= 14) {
        out[0] = '\0';
        return false;
    }

    make_sd_path(px, out, out_len);
    FILE *fp = fopen(out, "rb");
    if (fp != NULL) {
        fclose(fp);
        return true;
    }

#ifdef ESP_PLATFORM
    if (font_flash_mount()) {
        make_flash_path(px, out, out_len);
        fp = fopen(out, "rb");
        if (fp != NULL) {
            fclose(fp);
            return true;
        }
    }
#endif
    return false;
}

bool ui_read_font_available(int px)
{
    if (px <= 14) {
        return true;
    }
    if (s_loaded != NULL && s_loaded_px == px) {
        return true;
    }

    char path[96];
    return find_font_path(px, path, sizeof(path));
}

int ui_read_font_next(int px)
{
    int idx = 0;
    for (int i = 0; i < SIZE_COUNT; i++) {
        if (s_sizes[i] == px) {
            idx = i;
            break;
        }
    }
    for (int i = 1; i <= SIZE_COUNT; i++) {
        const int cand = s_sizes[(idx + i) % SIZE_COUNT];
        if (ui_read_font_available(cand)) {
            return cand;
        }
    }
    return 14;
}

int ui_read_font_prev(int px)
{
    int idx = 0;
    for (int i = 0; i < SIZE_COUNT; i++) {
        if (s_sizes[i] == px) {
            idx = i;
            break;
        }
    }
    for (int i = 1; i <= SIZE_COUNT; i++) {
        const int pos = (idx - i + SIZE_COUNT * 2) % SIZE_COUNT;
        const int cand = s_sizes[pos];
        if (ui_read_font_available(cand)) {
            return cand;
        }
    }
    return 14;
}

void ui_read_font_release(void)
{
    if (s_loaded != NULL) {
        ESP_LOGI(TAG, "释放 %dpx 阅读字体", s_loaded_px);
        lv_binfont_destroy(s_loaded);
        s_loaded = NULL;
        s_loaded_px = 0;
    }
}

/**
 * @brief 从指定文件加载一档字库；失败返回 NULL（调用方可换来源重试）
 *
 * 真机直接从文件加载（LVGL POSIX fs），PSRAM 里只留一份字形数据：
 * 8MB PSRAM 里代码 XIP 占了 4.4MB，只剩 ~2.5MB；2bpp 的 20px 字库约 1.3MB，
 * 能装下，4bpp 的 2.6MB 装不下（那种情况会换 Flash 里的 2bpp 再试）。
 */
static lv_font_t *load_font_file(int px, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }

    fseek(fp, 0L, SEEK_END);
    const long sz = ftell(fp);
    fseek(fp, 0L, SEEK_SET);
    if (sz <= 1024 || sz > 8L * 1024 * 1024) {
        ESP_LOGE(TAG, "%dpx 字库大小异常（%ld 字节，%s）", px, sz, path);
        fclose(fp);
        return NULL;
    }

#ifdef ESP_PLATFORM
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram < (size_t)sz + 256 * 1024) {
        ESP_LOGW(TAG, "PSRAM 余量不足（%u KB），%dpx 字库换来源（%s 需要 %ld KB）",
                 (unsigned)(free_psram / 1024), px, path, sz / 1024);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

#if LV_USE_FS_POSIX
    /* POSIX 驱动的盘符来自 sdkconfig 的 LV_FS_POSIX_LETTER（'S'），路径前缀为空 */
    char fs_path[112];
    snprintf(fs_path, sizeof(fs_path), "S:%s", path);
    lv_font_t *f = lv_binfont_create(fs_path);
#else
    lv_font_t *f = NULL;
    ESP_LOGE(TAG, "LVGL 没有启用 POSIX fs，无法加载字库（检查 sdkconfig）");
#endif

#else
    /* 模拟器：内存充足，沿用 buffer 方式（sim lv_conf 开了 MEMFS） */
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc((size_t)sz);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "字库缓冲分配失败（%ld 字节）", sz);
        fclose(fp);
        return NULL;
    }

    const size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        ESP_LOGE(TAG, "%dpx 字库读取不完整", px);
        free(buf);
        return NULL;
    }

#if LV_USE_FS_MEMFS
    lv_font_t *f = lv_binfont_create_from_buffer(buf, (uint32_t)sz);
#else
    lv_font_t *f = NULL;
    ESP_LOGE(TAG, "LVGL 没有启用 MEMFS，无法从内存加载字库（检查 sdkconfig）");
#endif
    free(buf);      /* 创建时已经把数据读进 LVGL 自己分配的内存 */
#endif  /* ESP_PLATFORM */

    if (f != NULL) {
        ESP_LOGI(TAG, "加载 %dpx 阅读字体（%ld KB，%s）", px, sz / 1024, path);
    }
    return f;
}

lv_font_t *ui_read_font_get(int px)
{
    if (px <= 14) {
        return (lv_font_t *)&font_ui_14;
    }
    if (s_loaded != NULL && s_loaded_px == px) {
        return s_loaded;
    }

    ui_read_font_release();

    char path[96];
    make_sd_path(px, path, sizeof(path));
    lv_font_t *f = load_font_file(px, path);

#ifdef ESP_PLATFORM
    /* SD 卡上有旧字库但太大（例如换 2bpp 前拷进去的 4bpp），或者文件坏了：
     * 再试 Flash 里的字库。这样不用手动删 SD /fonts。 */
    if (f == NULL && font_flash_mount()) {
        make_flash_path(px, path, sizeof(path));
        f = load_font_file(px, path);
    }
#endif

    if (f == NULL) {
        ESP_LOGW(TAG, "没有可用的 %dpx 字库，回退 14px", px);
        return (lv_font_t *)&font_ui_14;
    }

    s_loaded = f;
    s_loaded_px = px;
    return f;
}
