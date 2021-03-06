/*-
 * Copyright (c) 1988, 1989, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1989 by Berkeley Softworks
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Adam de Boor.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from: @(#)make.h	8.3 (Berkeley) 6/13/95
 * $FreeBSD: src/usr.bin/make/make.h,v 1.29 2005/02/01 10:50:36 harti Exp $
 * $DragonFly: src/usr.bin/make/make.h,v 1.40 2005/08/23 21:03:02 okumoto Exp $
 */

#ifndef make_h_a91074b9
#define	make_h_a91074b9

/**
 * make.h
 *	The global definitions for make
 */

#include <stdbool.h>

/* buildworld needs this on FreeBSD */
#ifndef __arysize
#define __arysize(ary)		(sizeof(ary)/sizeof((ary)[0]))
#endif

struct Buffer;
struct CLI;
struct GNode;
struct Lst;
struct Shell;

/*
 * Warning flags
 */
enum {
	WARN_DIRSYNTAX	= 0x0001	/* syntax errors in directives */
};

int Make_TimeStamp(struct GNode *, struct GNode *);
bool Make_OODate(struct GNode *);
int Make_HandleUse(struct GNode *, struct GNode *);
void Make_Update(struct GNode *);
void Make_DoAllVar(struct GNode *);
bool Make_Run(struct Lst *, bool);
void Main_ParseArgLine(struct CLI *, const char [], int);
int Main_ParseWarn(const char *, int);

#endif /* make_h_a91074b9 */
