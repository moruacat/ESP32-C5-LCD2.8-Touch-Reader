#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 词典数据层（新格式：dict.dat + dict.idx，由 tools/build_dict.py 生成）
 *
 * 词库文件：`<SD 挂载点>/dict/dict.dat`，UTF-8 纯文本，每行 **5 字段 / 4 个 TAB**：
 *
 *     word<TAB>音标<TAB>释义<TAB>徽章码<TAB>原形
 *
 *   - word 全小写，允许 `a-z ' - 空格`（`give up` 这类短语也在表里）
 *   - 释义用 `；` 分词性段落，词性前缀如 `n.` / `v.`；行内不含 TAB/CR/LF
 *   - 徽章码：`4`CET4 `6`CET6 `K`考研 `G`高考 `Z`中考 `T`TOEFL `I`IELTS
 *     `R`GRE `O`Oxford3000 `C<1-5>`Collins 星级
 *   - 原形：本词的原形（可空），用于显示「running → run」
 *
 * 索引文件：`dict.idx`（每 64 行一个锚点，块首词前 8 字节 + 文件偏移）。
 * 进页面时 `srv_dict_init()` 同步读一遍索引（约 30ms、常驻 ~40KB），之后每次查词
 * 只需 1 次定位 + 顺序读一个 64 行的块，**不需要后台建索引**。
 */

/* ===================== 徽章 ===================== */
#define SRV_DICT_TAG_CET4     (1u << 0)
#define SRV_DICT_TAG_CET6     (1u << 1)
#define SRV_DICT_TAG_KY       (1u << 2)     /*!< 考研 */
#define SRV_DICT_TAG_GK       (1u << 3)     /*!< 高考 */
#define SRV_DICT_TAG_ZK       (1u << 4)     /*!< 中考 */
#define SRV_DICT_TAG_TOEFL    (1u << 5)
#define SRV_DICT_TAG_IELTS    (1u << 6)
#define SRV_DICT_TAG_GRE      (1u << 7)
#define SRV_DICT_TAG_OXFORD   (1u << 8)

/*! 一条词条（字段长度按生成器的上限留足） */
typedef struct {
    char     word[40];          /*!< 命中的词（小写原样） */
    char     phonetic[48];      /*!< 音标，可能为空 */
    char     lemma[40];         /*!< 原形；空 = 无（UI 决定是否显示「→ xxx」） */
    char     def[512];          /*!< 释义（`；` 分词性段落） */
    uint16_t tags;              /*!< SRV_DICT_TAG_* 位或 */
    uint8_t  collins;           /*!< Collins 星级 0~5 */
} srv_dict_entry_t;

/* ===================== 生命周期 ===================== */

/**
 * @brief 打开词库与索引（同步，幂等）。SD 未挂载或文件不在时返回错误
 */
esp_err_t srv_dict_init(void);

/**
 * @brief 关掉重新加载（SD 重挂、换词库文件后调用）
 */
void srv_dict_reload(void);

bool srv_dict_ready(void);

/**
 * @brief 主表文件路径（日志/界面提示用）
 */
const char *srv_dict_path(void);

/* ===================== 查询 ===================== */

/**
 * @brief 精确查词（不区分大小写）
 * @param out 命中时填好；可为 NULL 只判断有没有
 * @return ESP_OK 命中 / ESP_ERR_NOT_FOUND 未收录 / 其它为错误
 */
esp_err_t srv_dict_lookup(const char *word, srv_dict_entry_t *out);

/**
 * @brief 查词 + 词形还原
 *
 * 先精确查；没命中再按规则还原（-s/-es/-ed/-ing/-ly/-er/-est、辅音双写、
 * -ies→-y…）最多试几个候选。
 *
 * 命中后 `out->lemma` 给出「输入词对应的原形」：
 *   - 精确命中 `running` → 表里自带 lemma=run
 *   - 还原命中 `runnning` → 记成命中的那个词（供 UI 显示「runnning → run」）
 */
esp_err_t srv_dict_lookup_lemma(const char *word, srv_dict_entry_t *out);

/**
 * @brief 前缀联想：列出以 prefix 开头的词
 * @param cb  每条回调一次，返回 false 提前结束
 * @param max 最多返回条数（<=0 表示不限）
 * @return 命中条数
 */
typedef bool (*srv_dict_list_cb_t)(const srv_dict_entry_t *e, void *arg);

int srv_dict_prefix(const char *prefix, srv_dict_list_cb_t cb, void *arg, int max);

/* ===================== 词形变化 ===================== */

/*! 一个词的全部变形（字段内多个形式用逗号分隔，如 `roofs,rooves`） */
typedef struct {
    char base[40];          /*!< 原形 */
    char past[64];          /*!< 过去式 */
    char past_part[64];     /*!< 过去分词 */
    char ing[64];           /*!< 现在分词 */
    char third[64];         /*!< 第三人称单数 */
    char plural[64];        /*!< 复数 */
    char comp[64];          /*!< 比较级 */
    char sup[64];           /*!< 最高级 */
    bool found;             /*!< variants 表里查到该原形的变形 */
    bool changed;           /*!< 输入词 ≠ 原形（是变形词） */
} srv_dict_forms_t;

/**
 * @brief 查一个词的全部变形
 *
 * 输入可以是任意形式（ran / better / studies）：
 *   1. 先按词形还原找到原形（复用 srv_dict_lookup_lemma 的规则 + 表内 lemma）
 *   2. 再用原形查 dict/variants.dat（由 tools/build_variants.py 生成）
 *
 * 数据文件不在时返回 ESP_ERR_NOT_FOUND，`out->base` 仍会填好。
 */
esp_err_t srv_dict_forms(const char *word, srv_dict_forms_t *out);

/**
 * @brief 变形表（dict/variants.dat）是否已成功加载
 *
 * UI 用它区分「表没装」和「这个词确实没有变形记录」两种提示。
 */
bool srv_dict_variants_ready(void);

/**
 * @brief 拼写容错候选（编辑距离 ≤1，按「长度接近 + 字母序」排）
 *
 * 只在「前两个字母相同」的索引桶里扫，累计读满 64KB 就放弃 —— 桶再大也不会卡住。
 */
int srv_dict_suggest(const char *word, srv_dict_list_cb_t cb, void *arg, int max);

/* ===================== 逐词直译 ===================== */

/**
 * @brief 逐词直译：按 `[A-Za-z][A-Za-z'-]*` 切 token，每个 token 查词形还原
 * @param cb  每个 token 回调一次；e == NULL 表示没查到（原样保留），返回 false 提前结束
 */
typedef bool (*srv_dict_word_cb_t)(const char *token, const srv_dict_entry_t *e, void *arg);

void srv_dict_translate(const char *text, srv_dict_word_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif