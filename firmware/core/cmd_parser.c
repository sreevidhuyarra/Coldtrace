#include "cmd_parser.h"

#include <stddef.h>

void cmd_parser_init(cmd_parser_t *p)
{
    p->len = 0;
    p->overflow = false;
    p->buf[0] = '\0';
}

cmd_result_t cmd_parser_feed(cmd_parser_t *p, char c)
{
    /* '\r' ends a line too: terminals send a bare '\r' for Enter. For "\r\n"
     * the '\n' then arrives as an empty line, which is ignored. */
    if (c == '\r' || c == '\n') {
        if (p->overflow) {
            cmd_parser_init(p);
            return CMD_TOOLONG;
        }
        if (p->len == 0) {
            return CMD_NONE;               /* empty line: ignored */
        }
        p->buf[p->len] = '\0';
        p->len = 0;                        /* ready for the next line */
        return CMD_LINE;
    }

    if (p->overflow) {
        return CMD_NONE;                   /* still discarding */
    }
    if (p->len >= CMD_LINE_MAX) {
        p->overflow = true;                /* character 65: line is too long */
        return CMD_NONE;
    }
    p->buf[p->len++] = c;
    return CMD_NONE;
}

char *cmd_parser_line(cmd_parser_t *p)
{
    return p->buf;
}

bool cmd_split(char *line, cmd_t *out)
{
    out->name = NULL;
    out->argc = 0;

    char *tok = line;   /* start of the token being scanned */

    for (char *s = line; ; s++) {
        if (*s != ' ' && *s != '\0') {
            continue;
        }
        bool at_end = (*s == '\0');
        if (s == tok) {
            return false;                  /* empty token */
        }
        *s = '\0';
        if (out->name == NULL) {
            out->name = tok;
        } else {
            if (out->argc >= CMD_MAX_ARGS) {
                return false;
            }
            out->args[out->argc++] = tok;
        }
        if (at_end) {
            return true;
        }
        tok = s + 1;
    }
}
