/*
 * ctype.c - the classification and case tables behind <ctype.h>, for -128..255 ("C" locale).
 */
#include <ctype.h>

#define IS_UP(c)  ((c) >= 'A' && (c) <= 'Z')
#define IS_LO(c)  ((c) >= 'a' && (c) <= 'z')
#define IS_DI(c)  ((c) >= '0' && (c) <= '9')
#define IS_HX(c)  (IS_DI(c) || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))
#define CLS(c) ((unsigned short)(                                            \
    (IS_UP(c) ? _CT_U : 0) | (IS_LO(c) ? _CT_L : 0) | (IS_DI(c) ? _CT_D : 0) | \
    (((c) == ' ' || ((c) >= '\t' && (c) <= '\r')) ? _CT_S : 0) |              \
    (((c) >= 33 && (c) <= 126 && !IS_UP(c) && !IS_LO(c) && !IS_DI(c)) ? _CT_P : 0) | \
    ((((c) >= 0 && (c) < 32) || (c) == 127) ? _CT_C : 0) |                    \
    (IS_HX(c) ? _CT_X : 0) |                                                  \
    (((c) == ' ' || (c) == '\t') ? _CT_B : 0) |                               \
    ((c) == ' ' ? _CT_SP : 0)))
#define UPPER(c) ((short)(IS_LO(c) ? (c) - 32 : (c)))
#define LOWER(c) ((short)(IS_UP(c) ? (c) + 32 : (c)))

#define X4(F, b)   F(b), F((b) + 1), F((b) + 2), F((b) + 3)
#define X16(F, b)  X4(F, b), X4(F, (b) + 4), X4(F, (b) + 8), X4(F, (b) + 12)
#define X64(F, b)  X16(F, b), X16(F, (b) + 16), X16(F, (b) + 32), X16(F, (b) + 48)
#define X128(F, b) X64(F, b), X64(F, (b) + 64)
#define TABLE(F)   { X128(F, -128), X128(F, 0), X128(F, 128) }

const unsigned short __libc_ctype[384] = TABLE(CLS);
const short __libc_toupper_tab[384] = TABLE(UPPER);
const short __libc_tolower_tab[384] = TABLE(LOWER);
