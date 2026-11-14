/*
 * cmd_parser.h - turns a byte stream from the UART into command lines,
 * and splits a line into a command name plus arguments.
 *
 * Framing rules (see docs/protocol.md):
 *   - a line ends with '\n', '\r' or "\r\n" (terminals send a bare '\r'
 *     for Enter; the '\n' of "\r\n" then counts as an ignored empty line)
 *   - at most CMD_LINE_MAX characters; a longer line is discarded up to the
 *     next '\n' and reported once as CMD_TOOLONG
 *   - empty lines are ignored
 *   - tokens are separated by exactly one space
 */
#ifndef CMD_PARSER_H
#define CMD_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#define CMD_LINE_MAX 64u
#define CMD_MAX_ARGS 3u

typedef enum {
    CMD_NONE,     /* nothing complete yet (or an empty line) */
    CMD_LINE,     /* a complete line is ready: cmd_parser_line() */
    CMD_TOOLONG   /* a line longer than CMD_LINE_MAX just ended */
} cmd_result_t;

typedef struct {
    char    buf[CMD_LINE_MAX + 1u];  /* +1 for the terminating NUL */
    uint8_t len;
    bool    overflow;                /* discarding until the next '\n' */
} cmd_parser_t;

typedef struct {
    const char *name;
    const char *args[CMD_MAX_ARGS];
    uint8_t     argc;
} cmd_t;

void         cmd_parser_init(cmd_parser_t *p);
/* Feed one received byte. */
cmd_result_t cmd_parser_feed(cmd_parser_t *p, char c);
/* The line just completed. Valid until the next cmd_parser_feed() call;
 * cmd_split() edits it in place. */
char        *cmd_parser_line(cmd_parser_t *p);

/* Split line in place into name + args. Returns false if a token is empty
 * (leading, trailing or double space) or there are more than CMD_MAX_ARGS
 * arguments; the caller answers ERR ARGS. */
bool         cmd_split(char *line, cmd_t *out);

#endif /* CMD_PARSER_H */
