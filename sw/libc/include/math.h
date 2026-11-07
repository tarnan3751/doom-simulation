/*
 * math.h - the one math function our programs use, which GCC expands inline.
 * The library has no math functions: the target has no FPU and no program
 * in this repository calls any.
 */
#ifndef _MATH_H
#define _MATH_H

#include <sys/cdefs.h>

__BEGIN_DECLS
double fabs(double x);
__END_DECLS

#endif /* _MATH_H */
