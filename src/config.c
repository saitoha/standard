#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/compile.h>
#include <mruby/data.h>
#include <mruby/hash.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#include "buffer.h"
#include "config.h"
#include "editor.h"
#include "events.h"
#include "input.h"
#include "keycodes.h"
#include "syntax.h"
#include "term.h"


int tabstop_width = 8;

extern color_t syntax_fg[], syntax_bg[];
extern bool syntax_underline[], syntax_bold[];


static mrb_state *gmrbs;


static const char *syntax_names[SYNREG_COUNT] = {
    "default",
    "error",
    "tabbar",
    "tab_active_inner",
    "tab_active_outer",
    "tab_inactive_inner",
    "tab_inactive_outer",
    "linenr",
    "placeholder_empty",
    "placeholder_line",
    "statusbar",
    "modebar"
};

static const struct
{
    const char *name;
    int value;
} key_aliases[] = {
    { "A",          'a' },
    { "B",          'b' },
    { "C",          'c' },
    { "D",          'd' },
    { "E",          'e' },
    { "F",          'f' },
    { "G",          'g' },
    { "H",          'h' },
    { "I",          'i' },
    { "J",          'j' },
    { "K",          'k' },
    { "L",          'l' },
    { "M",          'm' },
    { "N",          'n' },
    { "O",          'o' },
    { "P",          'p' },
    { "Q",          'q' },
    { "R",          'r' },
    { "S",          's' },
    { "T",          't' },
    { "U",          'u' },
    { "V",          'v' },
    { "W",          'w' },
    { "X",          'x' },
    { "Y",          'y' },
    { "Z",          'z' },
    { "BS",         KEY_NSHIFT | KEY_BACKSPACE },
    { "DEL",        KEY_NSHIFT | KEY_DELETE },
    { "INS",        KEY_NSHIFT | KEY_INSERT },
    { "HOME",       KEY_NSHIFT | KEY_HOME },
    { "KEND",       KEY_NSHIFT | KEY_END },
    { "UP",         KEY_NSHIFT | KEY_UP },
    { "DOWN",       KEY_NSHIFT | KEY_DOWN },
    { "LEFT",       KEY_NSHIFT | KEY_LEFT },
    { "RIGHT",      KEY_NSHIFT | KEY_RIGHT },
    { "PAGEUP",     KEY_NSHIFT | KEY_PGUP },
    { "PAGEDOWN",   KEY_NSHIFT | KEY_PGDOWN },
    { "F1",         KEY_NSHIFT | KEY_F1 },
    { "F2",         KEY_NSHIFT | KEY_F2 },
    { "F3",         KEY_NSHIFT | KEY_F3 },
    { "F4",         KEY_NSHIFT | KEY_F4 },
    { "F5",         KEY_NSHIFT | KEY_F5 },
    { "F6",         KEY_NSHIFT | KEY_F6 },
    { "F7",         KEY_NSHIFT | KEY_F7 },
    { "F8",         KEY_NSHIFT | KEY_F8 },
    { "F9",         KEY_NSHIFT | KEY_F9 },
    { "F10",        KEY_NSHIFT | KEY_F10 },
    { "F11",        KEY_NSHIFT | KEY_F11 },
    { "F12",        KEY_NSHIFT | KEY_F12 },

    { "MBUTTON_LEFT",           0 },
    { "MBUTTON_MIDDLE",         1 },
    { "MBUTTON_RIGHT",          2 },
    { "MBUTTON_WHEEL_UP",      64 },
    { "MBUTTON_WHEEL_DOWN",    65 }
};


static void unhandled_exception(const char *file, mrb_state *mrbs)
{
    term_release();


    mrb_value msg = mrb_funcall(mrbs, mrb_obj_value(mrbs->exc), "message", 0);
    fprintf(stderr, "Unhandled exception in %s: %s\n", file, mrb_string_value_cstr(mrbs, &msg));

    fprintf(stderr, "Backtrace:\n");

    // Taken from mruby itself
    mrb_int ciidx = mrb_fixnum(mrb_obj_iv_get(mrbs, mrbs->exc, mrb_intern(mrbs, "ciidx")));
    if (ciidx >= mrbs->c->ciend - mrbs->c->cibase)
        ciidx = 10; /* ciidx is broken... */

    for (int i = ciidx; i >= 0; i--)
    {
        mrb_callinfo *ci = &mrbs->c->cibase[i];

        if (MRB_PROC_CFUNC_P(ci->proc))
            continue;

        char sep;
        if (ci->target_class == ci->proc->target_class)
            sep = '.';
        else
            sep = '#';

        const char *method = mrb_sym2name(mrbs, ci->mid);
        if (method)
        {
            const char *cn = mrb_class_name(mrbs, ci->proc->target_class);

            if (cn)
                fprintf(stderr, "    in %s%c%s\n", cn, sep, method);
            else
                fprintf(stderr, "    in %s\n", method);
        }
        else
            fprintf(stderr, "    (unknown)\n");
    }


    exit(1);
}


static bool event_call(const event_t *event, void *info)
{
    (void)event;

    mrb_sym symbol = (mrb_sym)(uintptr_t)info;

    // TODO: Allow class/object methods
    mrb_funcall_argv(gmrbs, mrb_nil_value(), symbol, 0, NULL);

    if (gmrbs->exc != NULL)
        unhandled_exception("?", gmrbs);


    return true;
}


static bool event_input(const event_t *event, void *info)
{
    (void)event;

    for (int i = 0; ((int *)info)[i]; i++)
        sim_input(((int *)info)[i]);

    return true;
}


static mrb_value highlight(mrb_state *mrbs, mrb_value self)
{
    (void)self;

    mrb_value wat;
    mrb_get_args(mrbs, "A", &wat);

    if (mrb_ary_len(mrbs, wat) != 2)
        mrb_raise(mrbs, mrbs->object_class, "Two arguments expected for “highlight”");


    mrb_value num  = mrb_ary_shift(mrbs, wat);
    mrb_value opts = mrb_ary_shift(mrbs, wat);

    if (!mrb_fixnum_p(num))
        mrb_raise(mrbs, mrbs->object_class, "Fixnum expected as first “highlight” argument");

    if (!mrb_hash_p(opts))
        mrb_raise(mrbs, mrbs->object_class, "Option hash expected as second “highlight” argument");


    int i = mrb_fixnum(num);
    if ((i < 0) || (i >= SYNREG_COUNT))
        mrb_raisef(mrbs, mrbs->object_class, "Highlight region %i is out of range", i);


    syntax_underline[i] = mrb_bool(mrb_hash_get(mrbs, opts, mrb_check_intern_cstr(mrbs, "underline")));
    syntax_bold[i] = mrb_bool(mrb_hash_get(mrbs, opts, mrb_check_intern_cstr(mrbs, "bold")));

    mrb_value fg = mrb_hash_get(mrbs, opts, mrb_check_intern_cstr(mrbs, "termfg"));
    mrb_value bg = mrb_hash_get(mrbs, opts, mrb_check_intern_cstr(mrbs, "termbg"));


    if (mrb_nil_p(fg))
        syntax_fg[i].type = COL_DEFAULT;
    else
    {
        int val = mrb_fixnum(fg); // FIXME
        syntax_fg[i].type = (val < 8) ? COL_8 : (val < 16) ? COL_16 : COL_256;
        syntax_fg[i].color = val;
    }


    if (mrb_nil_p(bg))
        syntax_bg[i].type = COL_DEFAULT;
    else
    {
        int val = mrb_fixnum(bg); // FIXME
        syntax_bg[i].type = (val < 8) ? COL_8 : (val < 16) ? COL_16 : COL_256;
        syntax_bg[i].color = val;
    }


    return mrb_nil_value();
}


static void generic_map(mrb_state *mrbs, int event_type)
{
    mrb_value mappings;
    mrb_get_args(mrbs, "H", &mappings);


    mrb_value keys = mrb_hash_keys(mrbs, mappings);

    mrb_value key;
    while (!mrb_nil_p(key = mrb_ary_shift(mrbs, keys)))
    {
        if (!mrb_fixnum_p(key))
            mrb_raise(mrbs, mrbs->object_class, "Integer expected for map");

        mrb_value target = mrb_hash_get(mrbs, mappings, key);

        bool map_func = mrb_symbol_p(target);
        bool map_ary  = mrb_array_p(target);
        bool map_str  = mrb_string_p(target);
        if (!map_func && !map_ary && !map_str)
            mrb_raise(mrbs, mrbs->object_class, "Must map onto a symbol, a string or an array of integers");

        if (map_func)
            register_event_handler((event_t){ event_type, mrb_fixnum(key) }, event_call, (void *)(uintptr_t)mrb_symbol(target));
        else if (map_ary)
        {
            int len = mrb_ary_len(mrbs, target);
            int *sim_inp = malloc((len + 1) * sizeof(int));

            for (int i = 0; i < len; i++)
            {
                mrb_value v = mrb_ary_entry(target, i);
                if (!mrb_fixnum_p(v))
                    mrb_raise(mrbs, mrbs->object_class, "Must map onto a symbol, a string or an array of integers");

                sim_inp[i] = mrb_fixnum(v);
            }

            sim_inp[len] = 0;

            register_event_handler((event_t){ event_type, mrb_fixnum(key) }, event_input, sim_inp);
        }
        else // if (map_str)
        {
            const char *ptr = RSTRING_PTR(target);
            int len = RSTRING_LEN(target);
            int *sim_inp = malloc((len + 1) * sizeof(int));

            for (int i = 0; i < len; i++)
                sim_inp[i] = ptr[i];
            sim_inp[len] = 0;

            register_event_handler((event_t){ event_type, mrb_fixnum(key) }, event_input, sim_inp);
        }
    }
}

static mrb_value nmap(mrb_state *mrbs, mrb_value self)
{
    (void)self;
    generic_map(mrbs, EVENT_NORMAL_KEY);
    return mrb_nil_value();
}

static mrb_value imap(mrb_state *mrbs, mrb_value self)
{
    (void)self;
    generic_map(mrbs, EVENT_INSERT_KEY);
    return mrb_nil_value();
}

static mrb_value mbdmap(mrb_state *mrbs, mrb_value self)
{
    (void)self;
    generic_map(mrbs, EVENT_MBUTTON_DOWN);
    return mrb_nil_value();
}

static mrb_value mbumap(mrb_state *mrbs, mrb_value self)
{
    (void)self;
    generic_map(mrbs, EVENT_MBUTTON_UP);
    return mrb_nil_value();
}


static mrb_value input(mrb_state *mrbs, mrb_value self)
{
    (void)self;

    mrb_value wat;

    mrb_get_args(mrbs, "A", &wat);


    int len = mrb_ary_len(mrbs, wat);
    for (int i = 0; i < len; i++)
    {
        if (!mrb_fixnum_p(mrb_ary_entry(wat, i)))
            mrb_raise(mrbs, mrbs->object_class, "Array of integers expected for input");

        sim_input(mrb_fixnum(mrb_ary_entry(wat, i)));
    }


    return mrb_nil_value();
}


static void nop_free(mrb_state *mrbs, void *ptr)
{
    (void)mrbs;
    (void)ptr;
}

static struct mrb_data_type buf_type = { "Buffer", nop_free };
static struct mrb_data_type buflin_type = { "BufferLines", nop_free };

static struct RClass *bufcls, *buflincls;

static mrb_value get_active_buffer(mrb_state *mrbs, mrb_value self)
{
    (void)self;
    return mrb_obj_value(mrb_data_object_alloc(mrbs, bufcls, active_buffer, &buf_type));
}

static mrb_value buffer_get_x(mrb_state *mrbs, mrb_value self)
{
    (void)mrbs;
    return mrb_fixnum_value(((buffer_t *)DATA_PTR(self))->x);
}

static mrb_value buffer_set_x(mrb_state *mrbs, mrb_value self)
{
    mrb_int value;
    mrb_get_args(mrbs, "i", &value);
    return mrb_fixnum_value(((buffer_t *)DATA_PTR(self))->x = value);
}

static mrb_value buffer_get_y(mrb_state *mrbs, mrb_value self)
{
    (void)mrbs;
    return mrb_fixnum_value(((buffer_t *)DATA_PTR(self))->y);
}

static mrb_value buffer_set_y(mrb_state *mrbs, mrb_value self)
{
    mrb_int value;
    mrb_get_args(mrbs, "i", &value);
    return mrb_fixnum_value(((buffer_t *)DATA_PTR(self))->y = value);
}

static mrb_value buffer_get_lines(mrb_state *mrbs, mrb_value self)
{
    return mrb_obj_value(mrb_data_object_alloc(mrbs, buflincls, DATA_PTR(self), &buflin_type));
}

static mrb_value buffer_del(mrb_state *mrbs, mrb_value self)
{
    mrb_int chars;
    mrb_get_args(mrbs, "i", &chars);

    if (DATA_PTR(self) == active_buffer)
        delete_chars(chars);
    else
        buffer_delete(DATA_PTR(self), chars);

    return mrb_nil_value();
}

static mrb_value buffer_get_line(mrb_state *mrbs, mrb_value self)
{
    mrb_int line;
    mrb_get_args(mrbs, "i", &line);
    return mrb_str_new_cstr(mrbs, ((buffer_t *)DATA_PTR(self))->lines[line]);
}


static mrb_value editor_scroll(mrb_state *mrbs, mrb_value self)
{
    mrb_int lines;
    mrb_get_args(mrbs, "i", &lines);
    scroll(lines);
    return mrb_nil_value();
}


void load_config(void)
{
    FILE *fp = fopen(".stdrc", "r");

    if (fp == NULL)
    {
        perror("Could not open .stdrc");
        exit(1);
    }


    gmrbs = mrb_open();


    for (int i = 0; i < SYNREG_COUNT; i++)
    {
        char definition[128];
        sprintf(definition, "def %s *opts\n[%i, opts.reduce(:merge)]\nend\n", syntax_names[i], i);
        mrb_load_string(gmrbs, definition);
    }

    mrb_load_string(gmrbs, "def bold\n{bold: true}\nend\n");
    mrb_load_string(gmrbs, "def underline\n{underline: true}\nend\n");

    mrb_define_method(gmrbs, gmrbs->object_class, "highlight", &highlight, ARGS_REQ(1));
    mrb_define_alias(gmrbs, gmrbs->object_class, "hi", "highlight");


    mrb_define_method(gmrbs, gmrbs->object_class, "nmap", &nmap, ARGS_REQ(1));
    mrb_define_method(gmrbs, gmrbs->object_class, "imap", &imap, ARGS_REQ(1));
    mrb_define_method(gmrbs, gmrbs->object_class, "mbdmap", &mbdmap, ARGS_REQ(1));
    mrb_define_method(gmrbs, gmrbs->object_class, "mbumap", &mbumap, ARGS_REQ(1));

    mrb_define_method(gmrbs, gmrbs->object_class, "input", &input, ARGS_REQ(1));
    mrb_define_alias(gmrbs, gmrbs->object_class, "i", "input");

    for (int i = 0; i < (int)(sizeof(key_aliases) / sizeof(key_aliases[0])); i++)
        mrb_define_global_const(gmrbs, key_aliases[i].name, mrb_fixnum_value(key_aliases[i].value));


    mrb_load_string(gmrbs, "class Fixnum\ndef s\nself & ~0x20\nend\ndef c\nself | 0x800\nend\ndef a\nself | 0x1000\nend\nend\n");


    bufcls = mrb_define_class(gmrbs, "Buffer", NULL);
    mrb_define_class_method(gmrbs, bufcls, "active", &get_active_buffer, ARGS_NONE());
    mrb_define_method(gmrbs, bufcls, "x",  &buffer_get_x, ARGS_NONE());
    mrb_define_method(gmrbs, bufcls, "x=", &buffer_set_x, ARGS_REQ(1));
    mrb_define_method(gmrbs, bufcls, "y",  &buffer_get_y, ARGS_NONE());
    mrb_define_method(gmrbs, bufcls, "y=", &buffer_set_y, ARGS_REQ(1));
    mrb_define_method(gmrbs, bufcls, "lines", &buffer_get_lines, ARGS_NONE());
    mrb_define_method(gmrbs, bufcls, "delete", &buffer_del, ARGS_REQ(1));

    buflincls = mrb_define_class(gmrbs, "BufferLines", NULL);
    mrb_define_method(gmrbs, buflincls, "[]", &buffer_get_line, ARGS_REQ(1));

    mrb_define_method(gmrbs, gmrbs->object_class, "scroll", &editor_scroll, ARGS_REQ(1));


    mrb_load_file(gmrbs, fp);
    fclose(fp);


    mrb_value tabstop_val = mrb_gv_get(gmrbs, mrb_intern_cstr(gmrbs, "$tabstop"));

    if (!mrb_nil_p(tabstop_val))
        tabstop_width = mrb_fixnum(tabstop_val);

    if (gmrbs->exc != NULL)
        unhandled_exception(".stdrc", gmrbs);
}
