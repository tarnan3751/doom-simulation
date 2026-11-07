/*
 * strings.h - ASCII case-insensitive comparison.
 */
#ifndef _STRINGS_H
#define _STRINGS_H

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS
int strcasecmp(const char *a, const char *b) __pure_;
int strncasecmp(const char *a, const char *b, size_t n) __pure_;
__END_DECLS

#endif /* _STRINGS_H */
