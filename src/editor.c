#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "commands.h"
#include "config.h"
#include "editor.h"
#include "keycodes.h"
#include "syntax.h"
#include "term.h"
#include "tools.h"
#include "utf8.h"


static int desired_cursor_x = 0;

static enum
{
    MODE_NORMAL,
    MODE_INSERT,
    MODE_REPLACE
} input_mode = MODE_NORMAL;


void reposition_cursor(bool update_desire)
{
    term_cursor_pos(term_width - 16, term_height - 2);
    syntax_region(SYNREG_STATUSBAR);
    int position = printf("%i,%i", active_buffer->y + 1, active_buffer->x + 1);
    printf("%*c", 13 - position, ' ');


    int x = 0;
    for (int i = 0, j = 0; j < active_buffer->x; i += utf8_mbclen(active_buffer->lines[active_buffer->y][i]), j++)
    {
        if (active_buffer->lines[active_buffer->y][i] == '\t')
            x += tabstop_width - x % tabstop_width;
        else
            x++;
    }

    if (update_desire)
        desired_cursor_x = x;

    x += 1 + active_buffer->linenr_width + 1;

    term_cursor_pos(x, active_buffer->line_screen_pos[active_buffer->y]);


    fflush(stdout);
}


void error(const char *format, ...)
{
    term_cursor_pos(0, term_height - 1);
    syntax_region(SYNREG_ERROR);

    va_list va;
    va_start(va, format);
    vprintf(format, va);
    va_end(va);

    reposition_cursor(false);
}


static char *cmdtok(char *s)
{
    static char *saved;

    if (s != NULL)
        saved = s;

    if (saved == NULL)
        return NULL;

    s = saved;

    while (!isspace(*saved) && *saved)
    {
        if (*saved == '\\')
        {
            if (!*(++saved))
                break;
        }
        if (*saved == '"')
        {
            saved++;
            while ((*saved != '"') && *saved)
                saved++;
            if (!*saved)
                break;
        }
        if (*saved == '\'')
        {
            saved++;
            while ((*saved != '\'') && *saved)
                saved++;
            if (!*saved)
                break;
        }
        saved++;
    }

    if (isspace(*saved))
    {
        *saved = 0;
        while (isspace(*++saved));
    }

    if (!*saved)
        saved = NULL;

    return s;
}


static void command_line(void)
{
    term_cursor_pos(0, term_height - 1);
    syntax_region(SYNREG_DEFAULT);
    printf("%-*c", term_width - 1, ':');
    fflush(stdout);
    term_cursor_pos(1, term_height - 1);

    char cmd[128];

    int i = 0;
    while ((i < 127) && ((cmd[i++] = getchar()) != '\n'))
    {
        if ((cmd[i - 1] == ':') && (i == 1))
            cmd[--i] = 0;
        else if (cmd[i - 1] == 127)
        {
            print_flushed("\b \b");

            cmd[--i] = 0;
            if (i)
                cmd[--i] = 0;
            else
                break;
        }
        else
        {
            putchar(cmd[i - 1]);
            fflush(stdout);
        }
    }

    if (i)
        cmd[--i] = 0;


    // FIXME
    char *cmd_line[32];

    cmd_line[0] = cmdtok(cmd);
    for (int j = 0; cmd_line[j]; j++)
        cmd_line[j + 1] = cmdtok(NULL);


    for (i = 0; command_handlers[i].cmd && strcmp(command_handlers[i].cmd, cmd_line[0]); i++);

    if (command_handlers[i].cmd)
        command_handlers[i].execute(cmd_line);
    else
    {
        term_cursor_pos(0, term_height - 1);
        syntax_region(SYNREG_ERROR);
        printf("Unknown command “%s”.", cmd_line[0]);
        fflush(stdout);
    }


    reposition_cursor(false);
}


static void line_change_update_x(void)
{
    if (desired_cursor_x == -1)
    {
        int len = (int)utf8_strlen(active_buffer->lines[active_buffer->y]);
        active_buffer->x = len ? (len - 1) : 0;
        return;
    }


    int x = 0, i = 0, j = 0;
    for (; active_buffer->lines[active_buffer->y][i] && (x < desired_cursor_x); i += utf8_mbclen(active_buffer->lines[active_buffer->y][i]), j++)
    {
        if (active_buffer->lines[active_buffer->y][i] == '\t')
            x += tabstop_width - x % tabstop_width;
        else
            x++;
    }

    if ((desired_cursor_x == x) || (input_mode == MODE_INSERT))
        active_buffer->x = j;
    else
        active_buffer->x = j ? (j - 1) : 0;
}


// Screen lines required
static int slr(buffer_t *buf, int line)
{
    return (1 + buf->linenr_width + 1 + utf8_strlen(buf->lines[line]) + buffer_width - 1) / buffer_width;
}


// escape sequence translation table 1; TT for '\e[A' etc.
static int eseq_tt1[26] = {
    ['A' - 'A'] = KEY_UP,
    ['B' - 'A'] = KEY_DOWN,
    ['C' - 'A'] = KEY_RIGHT,
    ['D' - 'A'] = KEY_LEFT,
    ['F' - 'A'] = KEY_END,
    ['H' - 'A'] = KEY_HOME
};


static int read_escape_sequence(void)
{
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    char escape_sequence[8];
    int eseq_len = read(STDIN_FILENO, escape_sequence, 7);
    fcntl(STDIN_FILENO, F_SETFL, 0);

    if (eseq_len <= 0)
        return '\e';

    escape_sequence[eseq_len] = 0;

    if (escape_sequence[0] == '[')
    {
        if ((escape_sequence[1] >= 'A') && (escape_sequence[1] <= 'Z'))
            return eseq_tt1[escape_sequence[1] - 'A'];

        if ((escape_sequence[1] == '3') && (escape_sequence[2] == '~'))
            return KEY_DELETE;
    }

    return 0;
}


static void draw_line(buffer_t *buffer, int line)
{
    syntax_region(SYNREG_LINENR);
    printf(" %*i ", buffer->linenr_width, line);

    int x = 0;

    syntax_region(SYNREG_DEFAULT);
    for (int i = 0; active_buffer->lines[line][i]; i++)
    {
        if (buffer->lines[line][i] == '\t')
        {
            printf("%*c", tabstop_width - x % tabstop_width, ' ');
            x += tabstop_width - x % tabstop_width;
        }
        else
        {
            putchar(buffer->lines[line][i]);
            if ((buffer->lines[line][i] & 0xc0) != 0x80)
                x++;
        }
    }

    printf("%*s\n", buffer_width - x - buffer->linenr_width - 2, "");
}


static void write_string(const char *s)
{
    bool old_modified = active_buffer->modified;

    int old_slr = slr(active_buffer, active_buffer->y);
    int old_lc = active_buffer->line_count;

    buffer_insert(active_buffer, s);

    int new_slr = slr(active_buffer, active_buffer->y);

    if (!old_modified || (old_lc != active_buffer->line_count) || (old_slr != new_slr))
    {
        full_redraw();

        while (active_buffer->y > active_buffer->ye)
        {
            int screen_lines_required = new_slr - active_buffer->oll_unused_lines;
            while (screen_lines_required > 0)
                screen_lines_required -= slr(active_buffer, active_buffer->ys++);
            full_redraw();
        }
    }
    else
    {
        term_cursor_pos(0, active_buffer->line_screen_pos[active_buffer->y]);
        draw_line(active_buffer, active_buffer->y);
    }

    reposition_cursor(true);
}


static void delete_chars(int count)
{
    bool old_modified = active_buffer->modified;

    int old_slr = slr(active_buffer, active_buffer->y);
    int old_lc = active_buffer->line_count;

    buffer_delete(active_buffer, count);

    int new_slr = slr(active_buffer, active_buffer->y);

    if (!old_modified || (old_lc != active_buffer->line_count) || (old_slr != new_slr))
        full_redraw();
    else
    {
        term_cursor_pos(0, active_buffer->line_screen_pos[active_buffer->y]);
        draw_line(active_buffer, active_buffer->y);
    }

    reposition_cursor(true);
}


void editor(void)
{
    full_redraw();

    for (;;)
    {
        int inp = getchar();

        if (inp == '\e')
            inp = read_escape_sequence();

        if (input_mode == MODE_NORMAL)
        {
            switch (inp)
            {
                case ':':
                    command_line();
                    break;

                case 'h': inp = KEY_LEFT;   break;
                case 'l': inp = KEY_RIGHT;  break;
                case 'j': inp = KEY_DOWN;   break;
                case 'k': inp = KEY_UP;     break;

                case 127: inp = KEY_LEFT;   break;

                case 'x': inp = KEY_DELETE; break;

                case 'a':
                    // Advancing is always possible, except for when the line is empty
                    if (active_buffer->lines[active_buffer->y][0])
                        active_buffer->x++;
                case 'i':
                    input_mode = MODE_INSERT;
                    term_cursor_pos(0, term_height - 1);
                    syntax_region(SYNREG_MODEBAR);
                    print("--- INSERT ---");
                    reposition_cursor(false);
                    break;
            }
        }
        else if (inp == '\e')
        {
            term_cursor_pos(0, term_height - 1);
            printf("%-*c", term_width - 2, ' ');

            if ((input_mode == MODE_INSERT) && (active_buffer->x > 0))
                active_buffer->x--;

            input_mode = MODE_NORMAL;

            reposition_cursor(desired_cursor_x >= 0);
        }
        else if ((input_mode == MODE_INSERT) && (inp > 0) && (inp < 256))
        {
            char full_mbc[5] = { inp };
            if (inp & 0x80)
            {
                int expected = utf8_mbclen(inp);

                if (expected)
                {
                    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
                    read(STDIN_FILENO, full_mbc, expected - 1);
                    fcntl(STDIN_FILENO, F_SETFL, 0);
                }
            }

            write_string(full_mbc);
        }

        switch (inp)
        {
            case KEY_LEFT:
                if (active_buffer->x)
                    active_buffer->x--;
                reposition_cursor(true);
                break;

            case KEY_RIGHT:
                if (active_buffer->x < (int)utf8_strlen(active_buffer->lines[active_buffer->y]) - (input_mode != MODE_INSERT))
                    active_buffer->x++;
                reposition_cursor(true);
                break;

            case KEY_DOWN:
                if (active_buffer->y < active_buffer->line_count - 1)
                    active_buffer->y++;
                if (active_buffer->y > active_buffer->ye)
                {
                    int screen_lines_required = slr(active_buffer, active_buffer->y) - active_buffer->oll_unused_lines;
                    while (screen_lines_required > 0)
                        screen_lines_required -= slr(active_buffer, active_buffer->ys++);
                    full_redraw();
                }
                line_change_update_x();
                reposition_cursor(false);
                break;

            case KEY_UP:
                if (active_buffer->y > 0)
                    active_buffer->y--;
                line_change_update_x();
                if (active_buffer->y < active_buffer->ys)
                {
                    active_buffer->ys = active_buffer->y;
                    full_redraw();
                }
                reposition_cursor(false);
                break;

            case KEY_END:
                desired_cursor_x = -1;
                line_change_update_x();
                reposition_cursor(false);
                break;

            case KEY_HOME:
                active_buffer->x = 0;
                reposition_cursor(true);
                break;

            case KEY_DELETE:
                delete_chars(1);
                break;
        }
    }
}


void full_redraw(void)
{
    term_clear();


    int position = 1;
    syntax_region(SYNREG_TABBAR);
    putchar(' ');

    for (buffer_list_t *bl = buffer_list; bl != NULL; bl = bl->next)
    {
        buffer_t *buf = bl->buffer;

        syntax_region((buf == active_buffer) ? SYNREG_TAB_ACTIVE_OUTER : SYNREG_TAB_INACTIVE_OUTER);
        putchar('/');
        syntax_region((buf == active_buffer) ? SYNREG_TAB_ACTIVE_INNER : SYNREG_TAB_INACTIVE_INNER);
        printf(" %s%s ", buf->modified ? "*" : "", buf->name);
        syntax_region((buf == active_buffer) ? SYNREG_TAB_ACTIVE_OUTER : SYNREG_TAB_INACTIVE_OUTER);
        putchar('\\');

        syntax_region(SYNREG_TABBAR);
        putchar(' ');

        position += 2 + buf->modified + utf8_strlen(buf->name) + 3;
    }


    int remaining = (16 * term_width - position) % term_width; // FIXME

    printf("%*s\n", remaining, "");


    int y_pos = 1, line;

    for (line = active_buffer->ys; line < active_buffer->line_count; line++)
    {
        int new_y_pos = y_pos + slr(active_buffer, line);

        if (new_y_pos > term_height - 2)
            break;

        draw_line(active_buffer, line);

        active_buffer->line_screen_pos[line] = y_pos;

        y_pos = new_y_pos;
    }

    if (line < active_buffer->line_count)
    {
        active_buffer->oll_unused_lines = term_height - 2 - y_pos;
        while (y_pos++ < term_height - 2)
        {
            syntax_region(SYNREG_LINENR);
            printf(" %*i ", active_buffer->linenr_width,line);
            syntax_region(SYNREG_PLACEHOLDER_LINE);
            puts("@");
        }
    }
    else
    {
        active_buffer->oll_unused_lines = 0;
        while (y_pos++ < term_height - 2)
        {
            syntax_region(SYNREG_LINENR);
            printf(" %*s ", active_buffer->linenr_width, "-");
            syntax_region(SYNREG_PLACEHOLDER_EMPTY);
            puts("~");
        }
    }

    active_buffer->ye = line - 1;


    syntax_region(SYNREG_STATUSBAR);

    printf("%-*s", term_width - 16, active_buffer->location);
    position = printf("%i,%i", active_buffer->y + 1, active_buffer->x + 1);

    printf("%*c", 13 - position, ' ');

    bool top = !active_buffer->ys;
    bool bot = (line >= active_buffer->line_count - 1);

    if (top && bot)
        puts("All");
    else if (top)
        puts("Top");
    else if (bot)
        puts("Bot");
    else
        printf("%2i%%\n", (active_buffer->ys * 100) / (active_buffer->line_count - line + active_buffer->ys));


    if (input_mode != MODE_NORMAL)
    {
        syntax_region(SYNREG_MODEBAR);
        if (input_mode == MODE_INSERT)
            print("--- INSERT ---");
        else if (input_mode == MODE_REPLACE)
            print("--- REPLACE ---");
    }


    reposition_cursor(false);
}


void update_active_buffer(void)
{
    full_redraw();

    desired_cursor_x = active_buffer->x;
}
