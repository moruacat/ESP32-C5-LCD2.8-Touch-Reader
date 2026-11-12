#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单词学习：词书扫描、随机抽词、进度/错词本
 *
 * 数据放在 SD 卡 `/words/<词书名>/unit-01.txt`，每行一个单词（由
 * tools/build_wordbook.py 从课文/词表生成）。
 *
 * 学习进度存 `/words/progress.dat`（全局，按单词记录）：
 *     word<TAB>score<TAB>wrong
 *   - score 0~5：认识一次 +1，不认识 -1
 *   - wrong：累计答错次数（错词本据此挑词）
 *
 * 抽词权重：没学过 3、score<=1 为 2、score>=4 为 1；答错过的再 +2。
 * 每次会话从选中单元的并集里随机抽 count 个词，不要求按顺序学完。
 */

#define WB_MAX_BOOKS      16
#define WB_MAX_UNITS      128
#define WB_UNIT_WORD_MAX  48      /*!< 单次会话最多抽多少词 */
#define WB_NAME_MAX       40

/* ===================== 词书 ===================== */

esp_err_t srv_wordbook_init(void);
void srv_wordbook_reload(void);
bool srv_wordbook_ready(void);

int srv_wordbook_book_count(void);
const char *srv_wordbook_book_name(int book);

int srv_wordbook_unit_count(int book);
int srv_wordbook_unit_words(int book, int unit);
const char *srv_wordbook_unit_name(int book, int unit);

/*! 错词本里的词数（wrong > 0 的去重词） */
int srv_wordbook_wrong_count(void);

/* ===================== 学习会话 ===================== */

/**
 * @brief 从选中的单元随机抽词开始学习
 * @param book     词书下标
 * @param unit_sel 每个单元一个 bool（长度 = srv_wordbook_unit_count）
 * @param count    本次抽多少个词
 */
esp_err_t srv_wordbook_start_units(int book, const bool *unit_sel, int count);

/**
 * @brief 错词本复习
 */
esp_err_t srv_wordbook_start_wrong(int count);

bool srv_wordbook_session_active(void);
int  srv_wordbook_session_total(void);
int  srv_wordbook_session_done(void);
int  srv_wordbook_session_known(void);

/**
 * @brief 当前卡片上的单词；会话结束时返回空串
 */
const char *srv_wordbook_current(void);

/**
 * @brief 记录本次判断并推进到下一个词
 * @param known true=认识（左滑），false=不认识（右滑）
 */
void srv_wordbook_answer(bool known);

void srv_wordbook_stop(void);

#ifdef __cplusplus
}
#endif
