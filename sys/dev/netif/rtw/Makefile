# $DragonFly: src/sys/dev/netif/rtw/Makefile,v 1.2 2006/12/10 05:10:41 sephe Exp $
KMOD	= if_rtw

SRCS	= if_rtw_pci.c smc93cx6.c rtwphyio.c rtwphy.c rtw.c
SRCS	+= device_if.h bus_if.h pci_if.h opt_aic7xxx.h

.if !defined(BUILDING_WITH_KERNEL)
opt_aic7xxx.h:
	echo "" > ${.TARGET}
.endif

.include <bsd.kmod.mk>
