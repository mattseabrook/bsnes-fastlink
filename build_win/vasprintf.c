#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int vasprintf(char **str, const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int size = _vscprintf(fmt, copy);
    va_end(copy);
    if (size < 0) return -1;
    *str = (char*)malloc((size_t)size + 1);
    if (!*str) return -1;
    return vsprintf(*str, fmt, args);
}

int asprintf(char **strp, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int r = vasprintf(strp, fmt, args);
    va_end(args);
    return r;
}
