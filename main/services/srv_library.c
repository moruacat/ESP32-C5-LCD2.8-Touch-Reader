#include "srv_library.h"

#include "srv_sd.h"
#include "srv_config.h"
#include "srv_recent.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "srv_library";

/* data.ebfl 是 XML 元数据，只读前 4KB 足够取书名/作者/简介/封面尺寸 */
#define EBF_META_MAX       4096
/* 看简介时整份元数据都读进来（简介可能排在前 4KB 之后，也可能比 192 字节长） */
#define EBF_META_FULL_MAX  32768
/* 封面读取上限：超过这个大小直接放弃，别把 PSRAM 吃光 */
#define EBF_COVER_MAX      (1024 * 1024)
#define EBF_CHAPTER_PREFIX "chapter."
#define EBF_CHAPTER_SUFFIX ".ebfl"
#define EBF_DIR_SUFFIX     ".ebf"
#define COVER_FILE         "cover.raw"
#define DATA_FILE          "data.ebfl"
#define OTHERS_FILE        "others.dat"

static char        s_root[64];
static char        s_others[80];
static srv_book_t *s_books = NULL;
static int         s_count = 0;

/* ===================== 路径 ===================== */

const char *srv_library_root(void)
{
    if (s_root[0] == '\0') {
        snprintf(s_root, sizeof(s_root), "%s/books", srv_sd_mount_point());
    }
    return s_root;
}

const char *srv_library_others_path(void)
{
    if (s_others[0] == '\0') {
        snprintf(s_others, sizeof(s_others), "%s/%s", srv_library_root(), OTHERS_FILE);
    }
    return s_others;
}

static esp_err_t alloc_books(void)
{
    if (s_books != NULL) {
        return ESP_OK;
    }

    const size_t bytes = sizeof(srv_book_t) * SRV_LIBRARY_MAX;
    /* 书架常驻内存约 37KB，优先放 PSRAM，失败再退回内部 RAM */
    s_books = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_books == NULL) {
        ESP_LOGW(TAG, "PSRAM 分配失败，改用内部 RAM");
        s_books = malloc(bytes);
    }
    if (s_books == NULL) {
        ESP_LOGE(TAG, "书架内存分配失败（%u 字节）", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ===================== 小工具 ===================== */

/* 大小写不敏感地判断是否以 suffix 结尾 */
static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (ls < lf) {
        return false;
    }
    return strcasecmp(s + ls - lf, suffix) == 0;
}

/* 去掉结尾的 .ext（大小写不敏感），原地修改 */
static void strip_ext(char *name, const char *ext)
{
    size_t ln = strlen(name);
    size_t le = strlen(ext);
    if (ln > le && ends_with_ci(name, ext)) {
        name[ln - le] = '\0';
    }
}

/* 路径是否位于书架根目录之下（含根目录本身） */
static bool is_under_root(const char *path)
{
    const char *root = srv_library_root();
    size_t rl = strlen(root);
    return strncmp(path, root, rl) == 0 && (path[rl] == '\0' || path[rl] == '/');
}

/* UTF-8 首字节 → 这个字符占几个字节（非法首字节按 1 算，别让解析卡住） */
static int utf8_seq_len(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

/**
 * @brief 把「按字节截断」的长度收回到 UTF-8 字符边界
 *
 * 直接按字节砍会把最后一个汉字砍成半个，界面上就是乱码。所以先退到最后一个
 * 字符的首字节，再看这个字符是不是完整落在 len 之内；不完整就整个丢掉。
 */
static size_t utf8_trim_partial(const char *s, size_t len)
{
    size_t cut = len;
    while (cut > 0 && ((unsigned char)s[cut - 1] & 0xC0) == 0x80) {
        cut--;                          /* 落在续字节上 → 退到首字节之后 */
    }
    if (cut == 0) {
        return len;                     /* 整段都是续字节，异常数据，原样返回 */
    }

    size_t start = cut - 1;
    int    need  = utf8_seq_len((unsigned char)s[start]);
    return (need > 1 && start + (size_t)need > len) ? start : len;
}

/* 取 XML 里 <tag>...</tag> 的文本内容，去掉首尾空白 */
static bool xml_tag_text(const char *xml, const char *tag, char *out, size_t out_len)
{
    char open[32];
    char close[32];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *b = strstr(xml, open);
    if (b == NULL) {
        return false;
    }
    b += strlen(open);

    const char *e = strstr(b, close);
    if (e == NULL) {
        return false;
    }

    size_t n = (size_t)(e - b);
    while (n > 0 && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')) {
        b++;
        n--;
    }
    while (n > 0 && (b[n - 1] == ' ' || b[n - 1] == '\t' ||
                     b[n - 1] == '\r' || b[n - 1] == '\n')) {
        n--;
    }
    if (n >= out_len) {
        n = utf8_trim_partial(b, out_len - 1);   /* 别切出半个汉字 */
    }
    memcpy(out, b, n);
    out[n] = '\0';
    return true;
}

/* 取 <tag attr="123"> 的整数属性值；找不到返回 0 */
static int32_t xml_attr_int(const char *xml, const char *tag, const char *attr)
{
    char key[48];
    snprintf(key, sizeof(key), "<%s", tag);
    const char *p = strstr(xml, key);
    if (p == NULL) {
        return 0;
    }
    const char *tag_end = strchr(p, '>');
    if (tag_end == NULL) {
        return 0;
    }

    snprintf(key, sizeof(key), "%s=\"", attr);
    const char *a = strstr(p, key);
    if (a == NULL || a > tag_end) {
        return 0;
    }
    return (int32_t)strtol(a + strlen(key), NULL, 10);
}

/* ===================== EBF 解析 ===================== */

static bool chapter_count_cb(const srv_fs_entry_t *e, void *arg)
{
    int *cnt = (int *)arg;

    if (e->is_dir || strncmp(e->name, EBF_CHAPTER_PREFIX, 8) != 0) {
        return true;
    }
    if (!ends_with_ci(e->name, EBF_CHAPTER_SUFFIX)) {
        return true;
    }
    (*cnt)++;
    return true;
}

/* 读取 data.ebfl（书名/作者/简介/封面尺寸）与 cover.raw 大小 */
static void load_ebf_meta(const char *dir, srv_book_t *b)
{
    char path[SRV_FS_PATH_MAX];

    /* 章节数 */
    int chapters = 0;
    srv_fs_list(dir, chapter_count_cb, &chapters);
    b->chapter_count = chapters;

    /* 封面 */
    snprintf(path, sizeof(path), "%s/%s", dir, COVER_FILE);
    srv_fs_entry_t ce;
    if (srv_fs_stat(path, &ce) == ESP_OK && !ce.is_dir) {
        b->cover_bytes = (int32_t)ce.size;
    }

    /* 元数据 XML */
    snprintf(path, sizeof(path), "%s/%s", dir, DATA_FILE);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return;                         /* 没有 data.ebfl 不算错，用文件夹名当书名 */
    }

    char *buf = malloc(EBF_META_MAX + 1);
    if (buf == NULL) {
        fclose(fp);
        return;
    }
    size_t n = fread(buf, 1, EBF_META_MAX, fp);
    buf[n] = '\0';
    fclose(fp);

    char title[SRV_BOOK_NAME_MAX];
    if (xml_tag_text(buf, "title", title, sizeof(title)) && title[0] != '\0') {
        snprintf(b->name, sizeof(b->name), "%s", title);
    }
    xml_tag_text(buf, "author", b->author, sizeof(b->author));
    xml_tag_text(buf, "intro", b->intro, sizeof(b->intro));

    b->cover_w = xml_attr_int(buf, "cover", "width");
    b->cover_h = xml_attr_int(buf, "cover", "height");

    /* 封面尺寸没写就按文件大小反推（RGB565 = 2 字节/像素），默认按 3:4 比例 */
    if (b->cover_bytes > 0 && (b->cover_w <= 0 || b->cover_h <= 0)) {
        int32_t px = b->cover_bytes / 2;
        b->cover_w = 96;
        b->cover_h = (px > 0) ? (px / b->cover_w) : 0;
    }

    free(buf);
}

/* ===================== 条目管理 ===================== */

static srv_book_t *find_slot(const char *path)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_books[i].path, path) == 0) {
            return &s_books[i];
        }
    }
    return NULL;
}

/* 把一次目录扫描到的条目转成书籍；返回 true 表示已加入 */
static bool add_from_entry(const char *path, const srv_fs_entry_t *e, bool external)
{
    bool is_ebf_dir = e->is_dir && ends_with_ci(e->name, EBF_DIR_SUFFIX);
    bool is_txt     = !e->is_dir && ends_with_ci(e->name, ".txt");

    if (!is_ebf_dir && !is_txt) {
        return false;
    }
    if (find_slot(path) != NULL) {
        return false;                   /* 已在书架里（/books 扫描与 others.dat 可能重叠） */
    }
    if (s_count >= SRV_LIBRARY_MAX) {
        ESP_LOGW(TAG, "书架已满（上限 %d），忽略 %s", SRV_LIBRARY_MAX, path);
        return false;
    }

    srv_book_t *b = &s_books[s_count];
    memset(b, 0, sizeof(*b));
    b->external = external;
    snprintf(b->path, sizeof(b->path), "%s", path);
    /* 用 %.*s 显式限长：srv_fs_entry_t.name 比书名缓冲长，直接 %s 会触发 -Wformat-truncation */
    snprintf(b->name, sizeof(b->name), "%.*s", SRV_BOOK_NAME_MAX - 1, e->name);

    if (is_ebf_dir) {
        b->type = BOOK_TYPE_EBF;
        strip_ext(b->name, EBF_DIR_SUFFIX);
        load_ebf_meta(path, b);
    } else {
        b->type = BOOK_TYPE_TXT;
        strip_ext(b->name, ".txt");
    }

    s_count++;
    return true;
}

static bool add_from_dir_cb(const srv_fs_entry_t *e, void *arg)
{
    char path[SRV_FS_PATH_MAX];
    if (srv_fs_join(srv_library_root(), e->name, path, sizeof(path)) != ESP_OK) {
        return true;
    }
    add_from_entry(path, e, false);
    return true;
}

/* ===================== 外部书籍清单 ===================== */

static esp_err_t others_save(void)
{
    FILE *fp = fopen(srv_library_others_path(), "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法写入 %s", srv_library_others_path());
        return ESP_FAIL;
    }

    fprintf(fp, "# 书架外部书籍清单，每行一个绝对路径（不移动源文件）\n");
    for (int i = 0; i < s_count; i++) {
        if (s_books[i].external) {
            fprintf(fp, "%s\n", s_books[i].path);
        }
    }
    fclose(fp);
    return ESP_OK;
}

static void others_load(void)
{
    FILE *fp = fopen(srv_library_others_path(), "r");
    if (fp == NULL) {
        return;                         /* 首次使用没有该文件是正常的 */
    }

    char line[SRV_FS_PATH_MAX + 8];
    int  added = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        if (find_slot(line) != NULL) {
            continue;
        }

        srv_fs_entry_t e;
        if (srv_fs_stat(line, &e) != ESP_OK) {
            ESP_LOGW(TAG, "外部书籍不存在，跳过: %s", line);
            continue;
        }
        if (add_from_entry(line, &e, true)) {
            added++;
        }
    }
    fclose(fp);

    if (added > 0) {
        ESP_LOGI(TAG, "载入外部书籍 %d 本", added);
    }
}

/* ===================== 对外接口 ===================== */

/* 最近阅读优先：读过的按时间倒序排前面，没读过的保持扫描顺序。
 * 只在开关打开时排；书架最多 48 本，扫描时排一次足够。 */
static int recent_rank_cmp(const srv_book_t *a, const srv_book_t *b)
{
    const int64_t ta = srv_recent_time(a->path);
    const int64_t tb = srv_recent_time(b->path);
    if (ta > 0 && tb > 0) {
        return (ta > tb) ? -1 : ((ta < tb) ? 1 : 0);
    }
    if (ta > 0) {
        return -1;
    }
    if (tb > 0) {
        return 1;
    }
    return 0;                           /* 都没读过：保持原顺序 */
}

static void library_sort_recent(void)
{
    if (s_count < 2 || srv_config_get_int(CFG_SHELF_RECENT, CFG_DEF_SHELF_RECENT) == 0) {
        return;
    }
    for (int i = 0; i < s_count - 1; i++) {
        for (int j = i + 1; j < s_count; j++) {
            if (recent_rank_cmp(&s_books[j], &s_books[i]) < 0) {
                srv_book_t tmp = s_books[i];
                s_books[i] = s_books[j];
                s_books[j] = tmp;
            }
        }
    }
}

esp_err_t srv_library_scan(void)
{
    if (alloc_books() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    s_count = 0;

    if (!srv_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD 未挂载，书架为空");
        return ESP_ERR_INVALID_STATE;
    }

    /* /books 不存在就建一个 */
    srv_fs_mkdir(srv_library_root());

    int before = s_count;
    srv_fs_list(srv_library_root(), add_from_dir_cb, NULL);
    int from_books = s_count - before;

    /* 合并外部书籍清单 */
    others_load();

    /* 最近阅读记录先加载，再按开关把读过的书排到前面 */
    (void)srv_recent_init();
    library_sort_recent();

    ESP_LOGI(TAG, "书架扫描完成：%s 下 %d 本，外部 %d 本，共 %d 本",
             srv_library_root(), from_books, s_count - from_books, s_count);
    return ESP_OK;
}

esp_err_t srv_library_init(void)
{
    /* 提前把路径字符串准备好，后面各处依赖它 */
    (void)srv_library_root();
    (void)srv_library_others_path();

    return srv_library_scan();
}

int srv_library_count(void)
{
    return s_count;
}

esp_err_t srv_library_get(int idx, const srv_book_t **out)
{
    if (out == NULL || idx < 0 || idx >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = &s_books[idx];
    return ESP_OK;
}

esp_err_t srv_library_find(const char *path, const srv_book_t **out)
{
    if (path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    srv_book_t *b = find_slot(path);
    if (b == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = b;
    return ESP_OK;
}

esp_err_t srv_library_add_external(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!srv_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (is_under_root(path)) {
        ESP_LOGW(TAG, "%s 已在书架目录内，无需手动添加", path);
        return ESP_ERR_INVALID_ARG;
    }

    srv_fs_entry_t e;
    if (srv_fs_stat(path, &e) != ESP_OK) {
        ESP_LOGE(TAG, "路径不存在: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (alloc_books() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    if (find_slot(path) != NULL) {
        return ESP_OK;                  /* 已经在书架里，幂等返回 */
    }
    if (!add_from_entry(path, &e, true)) {
        ESP_LOGE(TAG, "不是受支持的书籍（需 .txt 或 .ebf 文件夹）: %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    /* 外部书籍不能是路径本身就不存在的占位，落盘 */
    return others_save();
}

esp_err_t srv_library_remove(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    srv_book_t *b = find_slot(path);
    if (b == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    int idx = (int)(b - s_books);
    for (int i = idx; i < s_count - 1; i++) {
        s_books[i] = s_books[i + 1];
    }
    s_count--;

    ESP_LOGI(TAG, "已移出书架: %s", path);
    return others_save();               /* 更新 others.dat（非外部书籍不受影响） */
}

esp_err_t srv_library_read_cover(const srv_book_t *b, uint8_t **out_data,
                                 size_t *out_bytes, int32_t *out_w, int32_t *out_h)
{
    if (b == NULL || out_data == NULL || out_bytes == NULL ||
        out_w == NULL || out_h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (b->type != BOOK_TYPE_EBF) {
        return ESP_ERR_INVALID_ARG;      /* TXT 没有封面文件 */
    }
    if (b->cover_bytes <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)b->cover_bytes > EBF_COVER_MAX) {
        ESP_LOGW(TAG, "封面过大（%d 字节），不读取", (int)b->cover_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    char path[SRV_FS_PATH_MAX];
    if (srv_fs_join(b->path, COVER_FILE, path, sizeof(path)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "封面打开失败: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    const size_t bytes = (size_t)b->cover_bytes;
    /* 封面图给 UI 渲染用，放 PSRAM 免得挤内部 RAM */
    uint8_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(bytes);
    }
    if (buf == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t rd = fread(buf, 1, bytes, fp);
    fclose(fp);
    if (rd != bytes) {
        ESP_LOGW(TAG, "封面读取不完整（%u/%u）: %s", (unsigned)rd, (unsigned)bytes, path);
        free(buf);
        return ESP_FAIL;
    }

    /* 尺寸：以 data.ebfl 为准，但元数据写的宽高和实际字节数对不上时，
     * 说明是随便填的，按「宽 × 字节数」重算高（RGB565 = 2 字节/像素） */
    int32_t w = b->cover_w;
    int32_t h = b->cover_h;
    if (w > 0 && h > 0 && (int64_t)w * h * 2 != (int64_t)bytes) {
        h = (int32_t)(bytes / 2 / (size_t)w);
    }
    if (w <= 0 || h <= 0) {
        free(buf);
        ESP_LOGW(TAG, "封面尺寸异常: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_data  = buf;
    *out_bytes = bytes;
    *out_w     = w;
    *out_h     = h;
    return ESP_OK;
}

esp_err_t srv_library_read_intro(const srv_book_t *b, char *out, size_t out_len)
{
    if (b == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (b->type != BOOK_TYPE_EBF) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[SRV_FS_PATH_MAX];
    if (srv_fs_join(b->path, DATA_FILE, path, sizeof(path)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char *buf = malloc(EBF_META_FULL_MAX + 1);
    if (buf == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, EBF_META_FULL_MAX, fp);
    buf[n] = '\0';
    fclose(fp);

    bool ok = xml_tag_text(buf, "intro", out, out_len) && out[0] != '\0';
    free(buf);

    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}
