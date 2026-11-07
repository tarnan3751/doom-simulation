/*
 * ctype.h - character classification, "C" locale (ASCII) only, as single-evaluation
 * table lookups.  The tables cover -128..255 (index c + 128), so EOF and negative
 * `signed char` values (-fsigned-char builds) are safe; -128..-2 have no class.
 */
#ifndef _CTYPE_H
#define _CTYPE_H

#include <sys/cdefs.h>

#define _CT_U 0x01   /* upper */
#define _CT_L 0x02   /* lower */
#define _CT_D 0x04   /* digit */
#define _CT_S 0x08   /* space: \t \n \v \f \r ' ' */
#define _CT_P 0x10   /* punctuation */
#define _CT_C 0x20   /* control */
#define _CT_X 0x40   /* hex digit */
#define _CT_B 0x80   /* blank: ' ' and \t */
#define _CT_SP 0x100 /* the space character itself (for isprint) */

__BEGIN_DECLS
extern const unsigned short __libc_ctype[384];
extern const short __libc_toupper_tab[384];
extern const short __libc_tolower_tab[384];
__END_DECLS

#define __ctype_is(c, m) ((int)(__libc_ctype[(int)(c) + 128] & (m)))
#define isalnum(c)  __ctype_is(c, _CT_U | _CT_L | _CT_D)
#define isalpha(c)  __ctype_is(c, _CT_U | _CT_L)
#define isblank(c)  __ctype_is(c, _CT_B)
#define iscntrl(c)  __ctype_is(c, _CT_C)
#define isdigit(c)  __ctype_is(c, _CT_D)
#define isgraph(c)  __ctype_is(c, _CT_U | _CT_L | _CT_D | _CT_P)
#define islower(c)  __ctype_is(c, _CT_L)
#define isprint(c)  __ctype_is(c, _CT_U | _CT_L | _CT_D | _CT_P | _CT_SP)
#define ispunct(c)  __ctype_is(c, _CT_P)
#define isspace(c)  __ctype_is(c, _CT_S)
#define isupper(c)  __ctype_is(c, _CT_U)
#define isxdigit(c) __ctype_is(c, _CT_X)
#define tolower(c)  ((int)__libc_tolower_tab[(int)(c) + 128])
#define toupper(c)  ((int)__libc_toupper_tab[(int)(c) + 128])

#endif /* _CTYPE_H */
