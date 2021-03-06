#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdbool.h>

size_t utf8_strlen(const char *str);
size_t utf8_strlen_vis(const char *str);
bool utf8_is_dbc(const char *str);
int utf8_mbclen(char start_chr) __attribute__((pure));
int utf8_byte_offset(const char *str, int char_count);

#endif
