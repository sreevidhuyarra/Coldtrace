#include "twin.h"

#include <string.h>

/* A cursor over [p, end). Every helper returns NULL on malformed input, and
 * never reads past end. */

static const char *ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

/* p at '"'; returns just past the closing quote. */
static const char *skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') {
        return NULL;
    }
    for (p++; p < end; p++) {
        if (*p == '\\') {
            p++;                      /* skip the escaped character */
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Skip any value: string, object, array or a bare token (number, true,
 * false, null). Objects and arrays are skipped by depth counting, so nesting
 * costs no stack. */
static const char *skip_value(const char *p, const char *end)
{
    p = ws(p, end);
    if (p >= end) {
        return NULL;
    }
    if (*p == '"') {
        return skip_string(p, end);
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                p = skip_string(p, end);
                if (p == NULL) {
                    return NULL;
                }
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }
    const char *start = p;
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n') {
        p++;
    }
    return p > start ? p : NULL;
}

/* Is the string token at [k, k_end) (quotes included) equal to "name"? */
static bool key_is(const char *k, const char *k_end, const char *name)
{
    size_t n = strlen(name);
    return (size_t)(k_end - k) == n + 2u && memcmp(k + 1, name, n) == 0;
}

/* Find member `name` of the object at p ('{'). On success sets the value's
 * [*v, *v_end) and returns true. Only this object's own members are matched,
 * never keys inside nested values. */
static bool find_member(const char *p, const char *end, const char *name,
                        const char **v, const char **v_end)
{
    p = ws(p, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p = ws(p + 1, end);
    if (p < end && *p == '}') {
        return false;
    }
    while (p < end) {
        const char *k = p;
        const char *k_end = skip_string(p, end);
        if (k_end == NULL) {
            return false;
        }
        p = ws(k_end, end);
        if (p >= end || *p != ':') {
            return false;
        }
        const char *val = ws(p + 1, end);
        const char *val_end = skip_value(val, end);
        if (val_end == NULL) {
            return false;
        }
        if (key_is(k, k_end, name)) {
            *v = val;
            *v_end = val_end;
            return true;
        }
        p = ws(val_end, end);
        if (p < end && *p == ',') {
            p = ws(p + 1, end);
        } else {
            return false;             /* '}' (not found) or malformed */
        }
    }
    return false;
}

/* Is the object at p ('{') a well-formed list of "key": value members up to
 * its closing '}'? Nested values are only checked for balance. */
static bool members_ok(const char *p, const char *end)
{
    p = ws(p, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p = ws(p + 1, end);
    if (p < end && *p == '}') {
        return true;
    }
    while (p < end) {
        p = skip_string(p, end);
        if (p == NULL) {
            return false;
        }
        p = ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = skip_value(p + 1, end);
        if (p == NULL) {
            return false;
        }
        p = ws(p, end);
        if (p < end && *p == '}') {
            return true;
        }
        if (p >= end || *p != ',') {
            return false;
        }
        p = ws(p + 1, end);
    }
    return false;
}

/* Whole unsigned integer; rejects signs, fractions, exponents and overflow. */
static bool parse_u32(const char *p, const char *end, uint32_t *out)
{
    uint32_t v = 0;
    if (p >= end) {
        return false;
    }
    for (; p < end; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (UINT32_MAX - d) / 10u) {
            return false;
        }
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

/* Decimal with at most 2 fraction digits, as hundredths: "-2.5" -> -250. */
bool twin_parse_centi(const char *p, const char *end, int32_t *out)
{
    bool neg = false;
    int32_t whole = 0, frac = 0;
    int frac_digits = 0;
    if (p < end && *p == '-') {
        neg = true;
        p++;
    }
    if (p >= end || *p < '0' || *p > '9') {
        return false;
    }
    for (; p < end && *p >= '0' && *p <= '9'; p++) {
        whole = whole * 10 + (*p - '0');
        if (whole > 100000) {
            return false;             /* far outside any sensor range */
        }
    }
    if (p < end && *p == '.') {
        p++;
        if (p >= end) {
            return false;
        }
        for (; p < end && *p >= '0' && *p <= '9'; p++) {
            if (++frac_digits > 2) {
                return false;
            }
            frac = frac * 10 + (*p - '0');
        }
        if (frac_digits == 1) {
            frac *= 10;
        }
    }
    if (p != end) {
        return false;                 /* trailing junk, exponent ... */
    }
    int32_t v = whole * 100 + frac;
    *out = neg ? -v : v;
    return true;
}

bool twin_parse(const char *json, size_t len, twin_desired_t *out)
{
    memset(out, 0, sizeof *out);
    const char *end = json + len;
    const char *obj = ws(json, end);
    if (obj >= end || *obj != '{' || skip_value(obj, end) == NULL || !members_ok(obj, end)) {
        return false;
    }

    /* full twin: look inside "desired"; a patch is the desired object itself */
    const char *v, *v_end;
    if (find_member(obj, end, "desired", &v, &v_end) && *v == '{') {
        if (!members_ok(v, v_end)) {
            return false;
        }
        obj = v;
        end = v_end;
    }

    if (find_member(obj, end, "report_ms", &v, &v_end)) {
        out->report_ms_state = parse_u32(v, v_end, &out->report_ms) ? TWIN_OK : TWIN_BAD;
    }
    if (find_member(obj, end, "limit_c", &v, &v_end)) {
        out->limit_state = twin_parse_centi(v, v_end, &out->limit_centi) ? TWIN_OK : TWIN_BAD;
    }
    if (find_member(obj, end, "window_s", &v, &v_end)) {
        out->window_state = parse_u32(v, v_end, &out->window_s) ? TWIN_OK : TWIN_BAD;
    }
    if (find_member(obj, end, "$version", &v, &v_end)) {
        out->has_version = parse_u32(v, v_end, &out->version);
    }
    return true;
}
