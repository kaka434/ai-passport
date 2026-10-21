// main/app_detail.c
#include "app_detail.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- 游标式扫描
//
// 只做"够用"的 JSON:不建树、不分配、不递归(除 skip_value 的嵌套跳过)。
// 响应是固定的接口格式,遇到不认识的键就整体跳过,所以接口加字段不会崩。
//
// 游标底下是一个可以重填的小窗口,不是整份响应。原因很实在:实测详情响应
// 最大 36.7 KB,而板子跑起来空闲堆只有 45 KB,其中 20 KB 还要留给 TLS 握手
// ——整段缓冲在 C3 上根本放不下。解析器本来就是纯顺序扫描,给它一个 1 KB 的
// 窗口就够,内存占用与响应大小无关。

#define JCUR_WIN 1024

typedef struct {
    app_detail_read_fn read;
    void *ctx;
    char win[JCUR_WIN];
    size_t pos;
    size_t len;
    bool eof;
} jcur_t;

// 保证窗口里至少有 n 个字节可读(n 最大 4:j_is_null 要比 "null")。
static bool j_ensure(jcur_t *c, size_t n) {
    if (c->len - c->pos >= n) return true;
    if (c->eof) return false;

    if (c->pos > 0) {
        memmove(c->win, c->win + c->pos, c->len - c->pos);
        c->len -= c->pos;
        c->pos = 0;
    }
    while (c->len < n && !c->eof) {
        const int got = c->read(c->ctx, c->win + c->len, sizeof(c->win) - c->len);
        if (got <= 0) {
            c->eof = true;   // 0 = 正常结束,<0 = 读失败,都按"没有更多数据"处理
            break;
        }
        c->len += (size_t)got;
    }
    return c->len - c->pos >= n;
}

// 当前字节(不消耗);-1 表示数据到头。
static int j_cur(jcur_t *c) {
    if (!j_ensure(c, 1)) return -1;
    return (unsigned char)c->win[c->pos];
}

static void j_adv(jcur_t *c) {
    if (c->pos < c->len) c->pos++;
}

static void j_ws(jcur_t *c) {
    for (;;) {
        const int ch = j_cur(c);
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            j_adv(c);
            continue;
        }
        break;
    }
}

// 期望下一个非空白字符是 want;是就吃掉并返回 true。
static bool j_peek(jcur_t *c, char want) {
    j_ws(c);
    if (j_cur(c) != (unsigned char)want) return false;
    j_adv(c);
    return true;
}

static bool j_is_null(jcur_t *c) {
    j_ws(c);
    return j_ensure(c, 4) && memcmp(c->win + c->pos, "null", 4) == 0;
}

// 读一个字符串。buf 为 NULL 时只跳过。截断一定停在 UTF-8 字符边界上,
// 否则屏幕上会出现半个汉字的乱码。
static bool j_string(jcur_t *c, char *buf, size_t cap) {
    j_ws(c);
    if (j_cur(c) != '"') return false;
    j_adv(c);

    size_t n = 0;
    size_t safe = 0;   // 最后一个完整字符之后的位置
    bool truncated = false;

    for (;;) {
        int ch = j_cur(c);
        if (ch < 0) return false;
        j_adv(c);
        if (ch == '"') break;

        char out = (char)ch;
        if (ch == '\\') {
            ch = j_cur(c);
            if (ch < 0) return false;
            j_adv(c);
            switch (ch) {
                case 'n':
                case 't':
                case 'r':
                case 'b':
                case 'f':
                    out = ' ';
                    break;
                case 'u': {
                    // \uXXXX:接口给中文时是直接 UTF-8,这里只兜底处理 ASCII 转义,
                    // 其余统一降级成 '?',不去做代理对拼接。
                    unsigned code = 0;
                    for (int i = 0; i < 4; i++) {
                        const int h = j_cur(c);
                        if (h < 0) break;
                        j_adv(c);
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                    }
                    out = (code < 0x80) ? (char)code : '?';
                    break;
                }
                default:
                    out = (char)ch;   // \" \\ \/ 原样
                    break;
            }
        }

        if (buf == NULL) continue;
        if (n + 1 < cap) {
            // safe 记的是"这个字符开始之前"的位置:截断若发生在字符中间,
            // 回退到这里才能保证结尾是完整字符。
            const size_t before = n;
            buf[n++] = out;
            if (((unsigned char)out & 0xC0) != 0x80) safe = before;
        } else {
            truncated = true;
        }
    }

    if (buf != NULL && cap > 0) buf[truncated ? safe : n] = '\0';
    return true;
}

// 读一个整数。统计值都是整数,小数点后面直接丢掉。
static bool j_int(jcur_t *c, long *out) {
    j_ws(c);
    bool neg = false;
    int ch = j_cur(c);
    if (ch == '-' || ch == '+') {
        neg = (ch == '-');
        j_adv(c);
    }

    bool any = false;
    long value = 0;
    for (;;) {
        ch = j_cur(c);
        if (ch < '0' || ch > '9') break;
        // 夹一下,避免脏数据把 long 撑溢出(统计值不会超过几千)。
        if (value < 1000000) value = value * 10 + (ch - '0');
        j_adv(c);
        any = true;
    }
    if (!any) return false;

    if (j_cur(c) == '.') {
        j_adv(c);
        for (;;) {
            ch = j_cur(c);
            if (ch < '0' || ch > '9') break;
            j_adv(c);
        }
    }

    *out = neg ? -value : value;
    return true;
}

static bool j_skip_value(jcur_t *c);

// 同一个字段接口有时给字符串、有时给数字(minute 是"45"还是 45 取决于比赛),
// 有时给 null。统一取成文本:数字按十进制写进 buf,null 当空串。
static bool j_text(jcur_t *c, char *buf, size_t cap) {
    j_ws(c);
    if (j_cur(c) == '"') return j_string(c, buf, cap);
    if (j_is_null(c)) {
        if (!j_skip_value(c)) return false;
        if (buf != NULL && cap > 0) buf[0] = '\0';
        return true;
    }
    long value = 0;
    if (!j_int(c, &value)) return false;
    if (buf != NULL && cap > 0) {
        char tmp[16];
        size_t n = 0;
        unsigned long mag = (value < 0) ? (unsigned long)(-value) : (unsigned long)value;
        do {
            tmp[n++] = (char)('0' + (mag % 10));
            mag /= 10;
        } while (mag != 0 && n < sizeof(tmp) - 1);
        size_t out = 0;
        if (value < 0 && out + 1 < cap) buf[out++] = '-';
        while (n > 0 && out + 1 < cap) buf[out++] = tmp[--n];
        buf[out] = '\0';
    }
    return true;
}

// 跳过一个完整的 JSON 值(对象/数组/字符串/数字/字面量)。
static bool j_skip_value(jcur_t *c) {
    j_ws(c);
    const int ch = j_cur(c);
    if (ch < 0) return false;

    switch (ch) {
        case '"':
            return j_string(c, NULL, 0);
        case '{':
        case '[': {
            const char close = (ch == '{') ? '}' : ']';
            j_adv(c);
            j_ws(c);
            if (j_peek(c, close)) return true;
            for (;;) {
                if (close == '}') {
                    if (!j_string(c, NULL, 0)) return false;
                    if (!j_peek(c, ':')) return false;
                }
                if (!j_skip_value(c)) return false;
                j_ws(c);
                if (j_peek(c, ',')) continue;
                return j_peek(c, close);
            }
        }
        default: {
            // 数字 / true / false / null:一直吃到分隔符。
            bool any = false;
            for (;;) {
                const int d = j_cur(c);
                if (d < 0) break;
                if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t'
                    || d == '\n' || d == '\r') {
                    break;
                }
                j_adv(c);
                any = true;
            }
            return any;
        }
    }
}

// ---------------------------------------------------------------- 字段映射

// 把分钟字符串拆成"分钟 + 补时"。"35" → 35/0,"90+2" → 90/2。
static void parse_minute(const char *text, unsigned *minute, unsigned *extra) {
    unsigned m = 0;
    const char *p = text;
    while (*p >= '0' && *p <= '9') {
        if (m < 1000) m = m * 10 + (unsigned)(*p - '0');
        p++;
    }
    unsigned x = 0;
    if (*p == '+') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (x < 1000) x = x * 10 + (unsigned)(*p - '0');
            p++;
        }
    }
    *minute = m;
    *extra = x;
}

static uint8_t code_to_kind(const char *code) {
    if (strcmp(code, "G") == 0) return APP_GOAL_NORMAL;
    if (strcmp(code, "PG") == 0) return APP_GOAL_PENALTY;
    if (strcmp(code, "OG") == 0 || strcmp(code, "UG") == 0) return APP_GOAL_OWN;
    return APP_GOAL_NONE;
}

static uint8_t clamp_u8(unsigned value) {
    return (uint8_t)(value > 255 ? 255 : value);
}

// ---------------------------------------------------------------- events

// 一队的事件数组。side: 0 = 主队,1 = 客队。
static bool parse_event_array(jcur_t *c, app_detail_t *out, uint8_t side,
                              unsigned minute, unsigned extra) {
    if (!j_peek(c, '[')) return false;
    j_ws(c);
    if (j_peek(c, ']')) return true;

    for (;;) {
        if (!j_peek(c, '{')) return false;

        app_goal_t goal;
        memset(&goal, 0, sizeof(goal));
        goal.side = side;
        goal.minute = clamp_u8(minute);
        goal.extra = clamp_u8(extra);
        goal.kind = APP_GOAL_NONE;
        // reason 常常排在 code 前面,不能当场改 kind(code 随后会把它覆盖回 G)。
        bool penalty_hint = false;

        j_ws(c);
        if (!j_peek(c, '}')) {
            for (;;) {
                char key[24];
                if (!j_string(c, key, sizeof(key))) return false;
                if (!j_peek(c, ':')) return false;

                if (strcmp(key, "code") == 0) {
                    char code[8];
                    if (!j_text(c, code, sizeof(code))) return false;
                    goal.kind = code_to_kind(code);
                } else if (strcmp(key, "person") == 0) {
                    if (!j_text(c, goal.player, sizeof(goal.player))) return false;
                } else if (strcmp(key, "score") == 0) {
                    if (!j_text(c, goal.score, sizeof(goal.score))) return false;
                } else if (strcmp(key, "minute_extra") == 0) {
                    char text[8];
                    if (!j_text(c, text, sizeof(text))) return false;
                    unsigned m = 0;
                    unsigned x = 0;
                    parse_minute(text, &m, &x);
                    if (x > 0) goal.extra = clamp_u8(x);
                } else if (strcmp(key, "reason") == 0) {
                    char reason[24];
                    if (!j_text(c, reason, sizeof(reason))) return false;
                    // 有些比赛把点球写在 reason 里而不是给 PG 代码。
                    if (strstr(reason, "点球") != NULL) penalty_hint = true;
                } else if (!j_skip_value(c)) {
                    return false;
                }

                j_ws(c);
                if (j_peek(c, ',')) continue;
                if (!j_peek(c, '}')) return false;
                break;
            }
        }

        if (penalty_hint && goal.kind == APP_GOAL_NORMAL) goal.kind = APP_GOAL_PENALTY;

        if (goal.kind != APP_GOAL_NONE && out->goal_count < APP_GOAL_MAX) {
            out->goals[out->goal_count++] = goal;
        }

        j_ws(c);
        if (j_peek(c, ',')) continue;
        return j_peek(c, ']');
    }
}

// events 是一个"分钟 → {teamAEvents, teamBEvents}"的对象。分钟在键上。
static bool parse_events(jcur_t *c, app_detail_t *out) {
    j_ws(c);
    // 接口对"还没有事件"给的是空数组 [],不是空对象 {}:
    //   {"match_status":"Fixture","events":[],"statistics":[],…}   187 字节
    // 完场才是 {"19":{…},"45+2":{…}} 这种对象。只认对象会把未开赛的比赛整份
    // 判成解析失败,界面就变成"赛况加载失败"。数组一律当成"没有数据"跳过。
    if (j_cur(c) == '[') return j_skip_value(c);

    if (!j_peek(c, '{')) return false;
    j_ws(c);
    if (j_peek(c, '}')) return true;

    for (;;) {
        char group[16];
        if (!j_string(c, group, sizeof(group))) return false;
        if (!j_peek(c, ':')) return false;

        unsigned minute = 0;
        unsigned extra = 0;
        parse_minute(group, &minute, &extra);

        if (!j_peek(c, '{')) return false;
        j_ws(c);
        if (!j_peek(c, '}')) {
            for (;;) {
                char key[24];
                if (!j_string(c, key, sizeof(key))) return false;
                if (!j_peek(c, ':')) return false;

                if (strcmp(key, "teamAEvents") == 0) {
                    if (!parse_event_array(c, out, 0, minute, extra)) return false;
                } else if (strcmp(key, "teamBEvents") == 0) {
                    if (!parse_event_array(c, out, 1, minute, extra)) return false;
                } else if (strcmp(key, "minute") == 0) {
                    char text[8];
                    if (!j_text(c, text, sizeof(text))) return false;
                    unsigned m = 0;
                    unsigned x = 0;
                    parse_minute(text, &m, &x);
                    if (m > 0) minute = m;
                    if (x > 0) extra = x;
                } else if (!j_skip_value(c)) {
                    return false;
                }

                j_ws(c);
                if (j_peek(c, ',')) continue;
                if (!j_peek(c, '}')) return false;
                break;
            }
        }

        j_ws(c);
        if (j_peek(c, ',')) continue;
        return j_peek(c, '}');
    }
}

// ---------------------------------------------------------------- statistics

// {"id":"…","name":"…","formation":"4-3-3",…} —— 只取 formation。
static bool parse_team_object(jcur_t *c, char *formation, size_t cap) {
    if (!j_peek(c, '{')) return false;
    j_ws(c);
    if (j_peek(c, '}')) return true;

    for (;;) {
        char key[24];
        if (!j_string(c, key, sizeof(key))) return false;
        if (!j_peek(c, ':')) return false;

        if (strcmp(key, "formation") == 0) {
            if (!j_text(c, formation, cap)) return false;
        } else if (!j_skip_value(c)) {
            return false;
        }

        j_ws(c);
        if (j_peek(c, ',')) continue;
        return j_peek(c, '}');
    }
}

// {"value":39,"per":39,"class":""} —— 只取 value。
static bool parse_stat_value(jcur_t *c, int *out) {
    if (!j_peek(c, '{')) return false;
    j_ws(c);
    if (j_peek(c, '}')) return true;

    for (;;) {
        char key[16];
        if (!j_string(c, key, sizeof(key))) return false;
        if (!j_peek(c, ':')) return false;

        if (strcmp(key, "value") == 0) {
            long value = 0;
            if (!j_int(c, &value)) return false;
            *out = (int)value;
        } else if (!j_skip_value(c)) {
            return false;
        }

        j_ws(c);
        if (j_peek(c, ',')) continue;
        return j_peek(c, '}');
    }
}

// [{"type":"控球率","team_A":{"value":39,…},"team_B":{…}},…]
static bool parse_stat_list(jcur_t *c, app_detail_t *out) {
    if (!j_peek(c, '[')) return false;
    j_ws(c);
    if (j_peek(c, ']')) return true;

    for (;;) {
        if (!j_peek(c, '{')) return false;

        app_stat_t stat;
        memset(&stat, 0, sizeof(stat));

        j_ws(c);
        if (!j_peek(c, '}')) {
            for (;;) {
                char key[24];
                if (!j_string(c, key, sizeof(key))) return false;
                if (!j_peek(c, ':')) return false;

                if (strcmp(key, "type") == 0) {
                    if (!j_text(c, stat.label, sizeof(stat.label))) return false;
                } else if (strcmp(key, "team_A") == 0) {
                    int value = 0;
                    if (!parse_stat_value(c, &value)) return false;
                    stat.home = (int16_t)value;
                } else if (strcmp(key, "team_B") == 0) {
                    int value = 0;
                    if (!parse_stat_value(c, &value)) return false;
                    stat.away = (int16_t)value;
                } else if (!j_skip_value(c)) {
                    return false;
                }

                j_ws(c);
                if (j_peek(c, ',')) continue;
                if (!j_peek(c, '}')) return false;
                break;
            }
        }

        if (stat.label[0] != '\0' && out->stat_count < APP_STAT_MAX) {
            out->stats[out->stat_count++] = stat;
        }

        j_ws(c);
        if (j_peek(c, ',')) continue;
        return j_peek(c, ']');
    }
}

static bool parse_statistics(jcur_t *c, app_detail_t *out) {
    j_ws(c);
    // 同 events:统计还没出来时接口给 []。实测"中场暂停"的比赛就是
    // events 有对象、statistics 还是 [] —— 这种球看着有赛况,整份解析却会失败。
    if (j_cur(c) == '[') return j_skip_value(c);

    if (!j_peek(c, '{')) return false;
    j_ws(c);
    if (j_peek(c, '}')) return true;

    for (;;) {
        char key[24];
        if (!j_string(c, key, sizeof(key))) return false;
        if (!j_peek(c, ':')) return false;

        if (strcmp(key, "team_A") == 0) {
            if (!parse_team_object(c, out->formation_home, sizeof(out->formation_home))) {
                return false;
            }
        } else if (strcmp(key, "team_B") == 0) {
            if (!parse_team_object(c, out->formation_away, sizeof(out->formation_away))) {
                return false;
            }
        } else if (strcmp(key, "list") == 0) {
            if (!parse_stat_list(c, out)) return false;
        } else if (!j_skip_value(c)) {
            return false;
        }

        j_ws(c);
        if (j_peek(c, ',')) continue;
        return j_peek(c, '}');
    }
}

// ---------------------------------------------------------------- 对外接口

void app_detail_reset(app_detail_t *detail) {
    if (detail == NULL) return;
    memset(detail, 0, sizeof(*detail));
    detail->state = APP_STATE_UNKNOWN;
}

// JSON 对象的键顺序不保证按时间,所以解析完再按(分钟, 补时)排一次。
// 进球最多 16 条,插入排序足够,也不占栈。
static void sort_goals(app_detail_t *out) {
    for (size_t i = 1; i < out->goal_count; i++) {
        const app_goal_t key = out->goals[i];
        size_t j = i;
        while (j > 0) {
            const app_goal_t *prev = &out->goals[j - 1];
            const bool later = (prev->minute > key.minute)
                               || (prev->minute == key.minute && prev->extra > key.extra);
            if (!later) break;
            out->goals[j] = out->goals[j - 1];
            j--;
        }
        out->goals[j] = key;
    }
}

bool app_detail_parse_stream(app_detail_read_fn read, void *ctx, app_detail_t *out) {
    if (read == NULL || out == NULL) return false;
    app_detail_reset(out);

    jcur_t c;
    memset(&c, 0, sizeof(c));
    c.read = read;
    c.ctx = ctx;

    if (!j_peek(&c, '{')) return false;
    j_ws(&c);
    if (j_peek(&c, '}')) return false;   // 空对象:当成解析失败,界面会显示"拉取失败"

    for (;;) {
        char key[32];
        if (!j_string(&c, key, sizeof(key))) return false;
        if (!j_peek(&c, ':')) return false;

        if (strcmp(key, "match_status") == 0) {
            char status[16];
            if (!j_string(&c, status, sizeof(status))) return false;
            // overview 接口不给 period,只能折到"直播中"这一档;界面上另用
            // 列表快照里的状态覆盖,这里只作兜底。
            out->state = app_state_from_status(status, NULL);
        } else if (strcmp(key, "events") == 0) {
            // 没有赛况的比赛这一项给 null 或者 [] 都见过,parse_events 两种都收。
            if (j_is_null(&c)) {
                if (!j_skip_value(&c)) return false;
            } else if (!parse_events(&c, out)) {
                return false;
            }
        } else if (strcmp(key, "statistics") == 0) {
            if (j_is_null(&c)) {
                if (!j_skip_value(&c)) return false;
            } else if (!parse_statistics(&c, out)) {
                return false;
            }
        } else if (!j_skip_value(&c)) {
            return false;
        }

        j_ws(&c);
        if (j_peek(&c, ',')) continue;
        if (!j_peek(&c, '}')) return false;
        break;
    }

    sort_goals(out);
    return true;
}

// ---- 内存版入口(主机测试与"已经有一整段 JSON"的调用方用) ----

typedef struct {
    const char *p;
    size_t left;
} mem_src_t;

static int mem_read(void *ctx, char *buf, size_t cap) {
    mem_src_t *src = (mem_src_t *)ctx;
    const size_t n = (src->left < cap) ? src->left : cap;
    if (n == 0) return 0;
    memcpy(buf, src->p, n);
    src->p += n;
    src->left -= n;
    return (int)n;
}

bool app_detail_parse(const char *json, size_t len, app_detail_t *out) {
    if (json == NULL || out == NULL || len == 0) return false;
    mem_src_t src = { json, len };
    return app_detail_parse_stream(mem_read, &src, out);
}
