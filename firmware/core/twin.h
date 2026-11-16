/*
 * twin.h - read the device-twin settings out of IoT Hub's JSON.
 *
 * Two shapes arrive:
 *   full twin (GET response):  {"desired":{"report_ms":5000,"$version":3,...},"reported":{...}}
 *   desired patch:             {"report_ms":5000,"$version":4}
 * twin_parse() accepts either and picks out the settings ColdTrace knows.
 * Unknown keys, nested objects, arrays and strings are skipped safely.
 *
 * A small purpose-built reader rather than a JSON library: no malloc, no
 * recursion, bounded by the input length, and unit-tested with bad input.
 */
#ifndef TWIN_H
#define TWIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Field state after parsing. */
typedef enum {
    TWIN_ABSENT = 0,   /* key not present: leave the setting as it is */
    TWIN_OK,           /* present and a well-formed number */
    TWIN_BAD           /* present but not a usable number (string, null, 1.234 for limit_c...) */
} twin_field_t;

typedef struct {
    twin_field_t report_ms_state;
    uint32_t     report_ms;
    twin_field_t limit_state;
    int32_t      limit_centi;    /* limit_c x 100; at most 2 decimals */
    twin_field_t window_state;
    uint32_t     window_s;
    bool         has_version;
    uint32_t     version;        /* "$version" of the desired properties */
} twin_desired_t;

/* Returns false if json is not a JSON object (then *out is all ABSENT). */
bool twin_parse(const char *json, size_t len, twin_desired_t *out);

/* "-2.5" -> -250: a decimal with at most 2 fraction digits, as hundredths.
 * The whole of [p, end) must be the number. */
bool twin_parse_centi(const char *p, const char *end, int32_t *out);

#endif /* TWIN_H */
