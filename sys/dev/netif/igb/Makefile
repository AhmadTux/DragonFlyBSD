KMOD=	if_igb
SRCS=	if_igb.c
SRCS+=	device_if.h bus_if.h pci_if.h
SRCS+=	opt_polling.h

.ifndef BUILDING_WITH_KERNEL
opt_polling.h:
	echo '#define DEVICE_POLLING 1' > ${.OBJDIR}/${.TARGET}
.endif

.include <bsd.kmod.mk>
