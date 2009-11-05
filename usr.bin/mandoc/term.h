/*	$Id: term.h,v 1.12 2009/10/21 19:13:51 schwarze Exp $ */
/*
 * Copyright (c) 2008, 2009 Kristaps Dzonsons <kristaps@kth.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef TERM_H
#define TERM_H

__BEGIN_DECLS

enum	termenc {
	TERMENC_ASCII
};

struct	termp {
	size_t		  rmargin;	/* Current right margin. */
	size_t		  maxrmargin;	/* Max right margin. */
	size_t		  maxcols;	/* Max size of buf. */
	size_t		  offset;	/* Margin offest. */
	size_t		  col;		/* Bytes in buf. */
	int		  flags;
#define	TERMP_NOSPACE	 (1 << 2)	/* No space before words. */
#define	TERMP_NOLPAD	 (1 << 3)	/* See term_flushln(). */
#define	TERMP_NOBREAK	 (1 << 4)	/* See term_flushln(). */
#define	TERMP_IGNDELIM	 (1 << 6)	/* Delims like regulars. */
#define	TERMP_NONOSPACE	 (1 << 7)	/* No space (no autounset). */
#define	TERMP_DANGLE	 (1 << 8)	/* See term_flushln(). */
#define	TERMP_HANG	 (1 << 9)	/* See term_flushln(). */
#define	TERMP_TWOSPACE	 (1 << 10)	/* See term_flushln(). */
#define	TERMP_NOSPLIT	 (1 << 11)	/* See termp_an_pre/post(). */
#define	TERMP_SPLIT	 (1 << 12)	/* See termp_an_pre/post(). */
#define	TERMP_ANPREC	 (1 << 13)	/* See termp_an_pre(). */
	int		  bold;
	int		  under;
	char		 *buf;		/* Output buffer. */
	enum termenc	  enc;		/* Type of encoding. */
	void		 *symtab;	/* Encoded-symbol table. */
};

void		  term_newln(struct termp *);
void		  term_vspace(struct termp *);
void		  term_word(struct termp *, const char *);
void		  term_flushln(struct termp *);

size_t		  term_hspan(const struct roffsu *);
size_t		  term_vspan(const struct roffsu *);

__END_DECLS

#endif /*!TERM_H*/