PROG=	gdb
SRCS=	gdb.c

CFLAGS+= -I${.OBJDIR}/../libgdb -I${.CURDIR}/../libgdb -I${BASEDIR}/include
CFLAGS+= -I${BASEDIR}/bfd -I${BASEDIR}/gdb -I${BASEDIR}/gdb/config
CFLAGS+= -I${BASEDIR}/gdb/common -I${.CURDIR}/../libbfd
CFLAGS+= -I${.OBJDIR}/../libbfd

DPADD=  ${GDBLIBS}
DPADD+= ${LIBNCURSES} ${LIBM} ${LIBZ} ${LIBKVM}
LDADD=  -L../libgdb -lgdb
LDADD+= -L../libreadline -lreadline
LDADD+= -L../libopcodes -lopcodes
LDADD+= -L../libbfd -lbfd
LDADD+= -L../libiberty -liberty
LDADD+= -L../libdecnumber -ldecnumber
LDADD+= -lncurses -lm -lz -lkvm

SUBDIR=	doc

.include <bsd.prog.mk>

.PATH:	${BASEDIR}/gdb
