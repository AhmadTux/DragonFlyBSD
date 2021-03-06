/*
 * @(#)private.h	8.6
 * $FreeBSD: src/lib/libc/stdtime/private.h,v 1.6.8.1 2000/08/23 00:19:15 jhb Exp $
 * $DragonFly: src/usr.sbin/zic/private.h,v 1.4 2008/10/19 20:15:58 swildner Exp $
 */

#ifndef PRIVATE_H

#define PRIVATE_H
/*
** This file is in the public domain, so clarified as of
** 1996-06-05 by Arthur David Olson.
*/

/* Stuff moved from Makefile.inc to reduce clutter */
#ifndef TM_GMTOFF
#define TM_GMTOFF	tm_gmtoff
#define TM_ZONE		tm_zone
#define TZDIR		"/usr/share/zoneinfo"
#endif /* ndef TM_GMTOFF */

/*
** This header is for use ONLY with the time conversion code.
** There is no guarantee that it will remain unchanged,
** or that it will remain at all.
** Do NOT copy it to any system include directory.
** Thank you!
*/

#define GRANDPARENTED	"Local time zone must be set--see zic manual page"

/*
** Nested includes
*/

#include "sys/types.h"	/* for time_t */
#include "stdio.h"
#include "errno.h"
#include "string.h"
#include "limits.h"	/* for CHAR_BIT et al. */
#include "time.h"
#include "stdlib.h"

#include <sys/wait.h>	/* for WIFEXITED and WEXITSTATUS */

#include "unistd.h"	/* for F_OK, R_OK, and other POSIX goodness */

/* Unlike <ctype.h>'s isdigit, this also works if c < 0 | c > UCHAR_MAX. */
#define is_digit(c) ((unsigned)(c) - '0' <= 9)

#include "stdint.h"

/*
** Private function declarations.
*/

char *		icalloc(int nelem, int elsize);
char *		icatalloc(char * old, const char * new);
char *		icpyalloc(const char * string);
char *		imalloc(int n);
void *		irealloc(void * pointer, int size);
void		icfree(char * pointer);
void		ifree(char * pointer);
const char *	scheck(const char * string, const char * format);

/*
** Finally, some convenience items.
*/

#ifndef TRUE
#define TRUE	1
#endif /* !defined TRUE */

#ifndef FALSE
#define FALSE	0
#endif /* !defined FALSE */

#ifndef TYPE_BIT
#define TYPE_BIT(type)	(sizeof (type) * CHAR_BIT)
#endif /* !defined TYPE_BIT */

#ifndef TYPE_SIGNED
#define TYPE_SIGNED(type) (((type) -1) < 0)
#endif /* !defined TYPE_SIGNED */

/*
** Since the definition of TYPE_INTEGRAL contains floating point numbers,
** it cannot be used in preprocessor directives.
*/

#ifndef TYPE_INTEGRAL
#define TYPE_INTEGRAL(type) (((type) 0.5) != 0.5)
#endif /* !defined TYPE_INTEGRAL */

#ifndef INT_STRLEN_MAXIMUM
/*
** 302 / 1000 is log10(2.0) rounded up.
** Subtract one for the sign bit if the type is signed;
** add one for integer division truncation;
** add one more for a minus sign if the type is signed.
*/
#define INT_STRLEN_MAXIMUM(type) \
	((TYPE_BIT(type) - TYPE_SIGNED(type)) * 302 / 1000 + \
	1 + TYPE_SIGNED(type))
#endif /* !defined INT_STRLEN_MAXIMUM */

/*
** INITIALIZE(x)
*/

#ifndef GNUC_or_lint
#ifdef lint
#define GNUC_or_lint
#endif /* defined lint */
#ifndef lint
#ifdef __GNUC__
#define GNUC_or_lint
#endif /* defined __GNUC__ */
#endif /* !defined lint */
#endif /* !defined GNUC_or_lint */

#ifndef INITIALIZE
#ifdef GNUC_or_lint
#define INITIALIZE(x)	((x) = 0)
#endif /* defined GNUC_or_lint */
#ifndef GNUC_or_lint
#define INITIALIZE(x)
#endif /* !defined GNUC_or_lint */
#endif /* !defined INITIALIZE */

/*
** For the benefit of GNU folk...
** `_(MSGID)' uses the current locale's message library string for MSGID.
** The default is to use gettext if available, and use MSGID otherwise.
*/

#ifndef _
#define _(msgid) msgid
#endif /* !defined _ */

#ifndef TZ_DOMAIN
#define TZ_DOMAIN "tz"
#endif /* !defined TZ_DOMAIN */

#define YEARSPERREPEAT		400	/* years before a Gregorian repeat */

/*
** UNIX was a registered trademark of The Open Group in 2003.
*/

#endif /* !defined PRIVATE_H */
