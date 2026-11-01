#include "srv_recent.h"
#include "srv_library.h"
#include "srv_sd.h"
#include "srv_fs.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "srv_recent";

typedef struct {
    int64_t ts;                        /*!< unix 时间戳（秒） */
    char    path[SRV_FS_PATH_MAX];
} rec_t;

static rec_t *s_recs = NULL;
static int    s_n = 0;
static bool   s_loaded = false;

const char *srv_recent_path(void)
{
    static char path[SRV_FS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.recent", srv_library_root());
    return path;
}

esp_err_t srv_recent_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }
    if (!srv_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_recs == NULL) {
        /* 64 * (8 + 320) ≈ 21KB，优先 PSRAM */
        s_recs = heap_caps_malloc(sizeof(rec_t) * SRV_RECENT_MAX,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_recs == NULL) {
            s_recs = malloc(sizeof(rec_t) * SRV_RECENT_MAX);
        }
        if (s_recs == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_n = 0;
    FILE *fp = fopen(srv_recent_path(), "r");
    if (fp == NULL) {
        s_loaded = true;               /* 没有记录文件：正常，第一次打开时创建 */
        return ESP_OK;
    }

    char line[SRV_FS_PATH_MAX + 32];
    while (s_n < SRV_RECENT_MAX && fgets(line, sizeof(line), fp) != NULL) {
        long long ts;
        if (sscanf(line, "%lld\t%319[^\n]", &ts, s_recs[s_n].path) != 2) {
            continue;                  /* 坏行跳过 */
        }
        s_recs[s_n].ts = (int64_t)ts;
        s_n++;
    }
    fclose(fp);

    s_loaded = true;
    ESP_LOGI(TAG, "最近阅读记录 %d 条", s_n);
    return ESP_OK;
}

/* 写回整个文件（最多 64 行，很小） */
static void recent_save(void)
{
    FILE *fp = fopen(srv_recent_path(), "w");
    if (fp == NULL) {
        ESP_LOGW(TAG, "无法写入 %s", srv_recent_path());
        return;
    }
    for (int i = 0; i < s_n; i++) {
        fprintf(fp, "%lld\t%s\n", (long long)s_recs[i].ts, s_recs[i].path);
    }
    fclose(fp);
}

void srv_recent_touch(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (!s_loaded) {
        (void)srv_recent_init();
    }
    if (s_recs == NULL) {
        return;
    }

    const int64_t now = (int64_t)time(NULL);

    int found = -1;
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_recs[i].path, path) == 0) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        /* 移到最前（数组保持时间倒序），后面顺序不变 */
        rec_t tmp = s_recs[found];
        memmove(&s_recs[1], &s_recs[0], sizeof(rec_t) * (size_t)found);
        s_recs[0] = tmp;
    } else {
        if (s_n < SRV_RECENT_MAX) {
            memmove(&s_recs[1], &s_recs[0], sizeof(rec_t) * (size_t)s_n);
            s_n++;
        } else {
            /* 满了：丢掉最旧的一条（最后一项），整体后移 */
            memmove(&s_recs[1], &s_recs[0], sizeof(rec_t) * (SRV_RECENT_MAX - 1));
        }
        snprintf(s_recs[0].path, sizeof(s_recs[0].path), "%s", path);
    }
    s_recs[0].ts = now;

    recent_save();
    ESP_LOGI(TAG, "记录阅读：%.80s", path);
}

int64_t srv_recent_time(const char *path)
{
    if (path == NULL || !s_loaded || s_recs == NULL) {
        return 0;
    }
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_recs[i].path, path) == 0) {
            return s_recs[i].ts;
        }
    }
    return 0;
}
