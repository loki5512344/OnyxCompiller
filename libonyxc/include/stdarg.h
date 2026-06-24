/* stdarg.h — OnyxCC builtin va_list */
#pragma once
typedef char *va_list;
#define va_start(ap, last) ((ap) = (char *)&(last) + sizeof(last))
#define va_arg(ap, type) (*(type *)((ap) += sizeof(type), (ap) - sizeof(type)))
#define va_end(ap) ((void)0)
