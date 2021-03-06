/*
 * Copyright (c) 2001-2011, Intel Corporation 
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright notice, 
 *     this list of conditions and the following disclaimer.
 * 
 *  2. Redistributions in binary form must reproduce the above copyright 
 *     notice, this list of conditions and the following disclaimer in the 
 *     documentation and/or other materials provided with the distribution.
 * 
 *  3. Neither the name of the Intel Corporation nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "opt_polling.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/serialize.h>
#include <sys/serialize2.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/ifq_var.h>
#include <net/toeplitz.h>
#include <net/toeplitz2.h>
#include <net/vlan/if_vlan_var.h>
#include <net/vlan/if_vlan_ether.h>
#include <net/if_poll.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <bus/pci/pcivar.h>
#include <bus/pci/pcireg.h>

#include <dev/netif/ig_hal/e1000_api.h>
#include <dev/netif/ig_hal/e1000_82575.h>
#include <dev/netif/igb/if_igb.h>

#define IGB_NAME	"Intel(R) PRO/1000 "
#define IGB_DEVICE(id)	\
	{ IGB_VENDOR_ID, E1000_DEV_ID_##id, IGB_NAME #id }
#define IGB_DEVICE_NULL	{ 0, 0, NULL }

static struct igb_device {
	uint16_t	vid;
	uint16_t	did;
	const char	*desc;
} igb_devices[] = {
	IGB_DEVICE(82575EB_COPPER),
	IGB_DEVICE(82575EB_FIBER_SERDES),
	IGB_DEVICE(82575GB_QUAD_COPPER),
	IGB_DEVICE(82576),
	IGB_DEVICE(82576_NS),
	IGB_DEVICE(82576_NS_SERDES),
	IGB_DEVICE(82576_FIBER),
	IGB_DEVICE(82576_SERDES),
	IGB_DEVICE(82576_SERDES_QUAD),
	IGB_DEVICE(82576_QUAD_COPPER),
	IGB_DEVICE(82576_QUAD_COPPER_ET2),
	IGB_DEVICE(82576_VF),
	IGB_DEVICE(82580_COPPER),
	IGB_DEVICE(82580_FIBER),
	IGB_DEVICE(82580_SERDES),
	IGB_DEVICE(82580_SGMII),
	IGB_DEVICE(82580_COPPER_DUAL),
	IGB_DEVICE(82580_QUAD_FIBER),
	IGB_DEVICE(DH89XXCC_SERDES),
	IGB_DEVICE(DH89XXCC_SGMII),
	IGB_DEVICE(DH89XXCC_SFP),
	IGB_DEVICE(DH89XXCC_BACKPLANE),
	IGB_DEVICE(I350_COPPER),
	IGB_DEVICE(I350_FIBER),
	IGB_DEVICE(I350_SERDES),
	IGB_DEVICE(I350_SGMII),
	IGB_DEVICE(I350_VF),

	/* required last entry */
	IGB_DEVICE_NULL
};

static int	igb_probe(device_t);
static int	igb_attach(device_t);
static int	igb_detach(device_t);
static int	igb_shutdown(device_t);
static int	igb_suspend(device_t);
static int	igb_resume(device_t);

static boolean_t igb_is_valid_ether_addr(const uint8_t *);
static void	igb_setup_ifp(struct igb_softc *);
static int	igb_txctx_pullup(struct igb_tx_ring *, struct mbuf **);
static boolean_t igb_txctx(struct igb_tx_ring *, struct mbuf *);
static void	igb_add_sysctl(struct igb_softc *);
static int	igb_sysctl_intr_rate(SYSCTL_HANDLER_ARGS);

static void	igb_vf_init_stats(struct igb_softc *);
static void	igb_reset(struct igb_softc *);
static void	igb_update_stats_counters(struct igb_softc *);
static void	igb_update_vf_stats_counters(struct igb_softc *);
static void	igb_update_link_status(struct igb_softc *);
static void	igb_init_tx_unit(struct igb_softc *);
static void	igb_init_rx_unit(struct igb_softc *);

static void	igb_set_vlan(struct igb_softc *);
static void	igb_set_multi(struct igb_softc *);
static void	igb_set_promisc(struct igb_softc *);
static void	igb_disable_promisc(struct igb_softc *);

static int	igb_dma_alloc(struct igb_softc *);
static void	igb_dma_free(struct igb_softc *);
static int	igb_create_tx_ring(struct igb_tx_ring *);
static int	igb_create_rx_ring(struct igb_rx_ring *);
static void	igb_free_tx_ring(struct igb_tx_ring *);
static void	igb_free_rx_ring(struct igb_rx_ring *);
static void	igb_destroy_tx_ring(struct igb_tx_ring *, int);
static void	igb_destroy_rx_ring(struct igb_rx_ring *, int);
static void	igb_init_tx_ring(struct igb_tx_ring *);
static int	igb_init_rx_ring(struct igb_rx_ring *);
static int	igb_newbuf(struct igb_rx_ring *, int, boolean_t);
static int	igb_encap(struct igb_tx_ring *, struct mbuf **);

static void	igb_stop(struct igb_softc *);
static void	igb_init(void *);
static int	igb_ioctl(struct ifnet *, u_long, caddr_t, struct ucred *);
static void	igb_media_status(struct ifnet *, struct ifmediareq *);
static int	igb_media_change(struct ifnet *);
static void	igb_timer(void *);
static void	igb_watchdog(struct ifnet *);
static void	igb_start(struct ifnet *);
#ifdef DEVICE_POLLING
static void	igb_poll(struct ifnet *, enum poll_cmd, int);
#endif

static void	igb_intr(void *);
static void	igb_rxeof(struct igb_rx_ring *, int);
static void	igb_txeof(struct igb_tx_ring *);
static void	igb_set_itr(struct igb_softc *);
static void	igb_enable_intr(struct igb_softc *);
static void	igb_disable_intr(struct igb_softc *);

/* Management and WOL Support */
static void	igb_get_mgmt(struct igb_softc *);
static void	igb_rel_mgmt(struct igb_softc *);
static void	igb_get_hw_control(struct igb_softc *);
static void     igb_rel_hw_control(struct igb_softc *);
static void     igb_enable_wol(device_t);

static device_method_t igb_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		igb_probe),
	DEVMETHOD(device_attach,	igb_attach),
	DEVMETHOD(device_detach,	igb_detach),
	DEVMETHOD(device_shutdown,	igb_shutdown),
	DEVMETHOD(device_suspend,	igb_suspend),
	DEVMETHOD(device_resume,	igb_resume),
	{ 0, 0 }
};

static driver_t igb_driver = {
	"igb",
	igb_methods,
	sizeof(struct igb_softc),
};

static devclass_t igb_devclass;

DECLARE_DUMMY_MODULE(if_igb);
MODULE_DEPEND(igb, ig_hal, 1, 1, 1);
DRIVER_MODULE(if_igb, pci, igb_driver, igb_devclass, NULL, NULL);

static int	igb_rxd = IGB_DEFAULT_RXD;
static int	igb_txd = IGB_DEFAULT_TXD;
static int	igb_msi_enable = 1;
static int	igb_msix_enable = 1;
static int	igb_eee_disabled = 1;	/* Energy Efficient Ethernet */
static int	igb_fc_setting = e1000_fc_full;

/*
 * DMA Coalescing, only for i350 - default to off,
 * this feature is for power savings
 */
static int	igb_dma_coalesce = 0;

TUNABLE_INT("hw.igb.rxd", &igb_rxd);
TUNABLE_INT("hw.igb.txd", &igb_txd);
TUNABLE_INT("hw.igb.msi.enable", &igb_msi_enable);
TUNABLE_INT("hw.igb.msix.enable", &igb_msix_enable);
TUNABLE_INT("hw.igb.fc_setting", &igb_fc_setting);

/* i350 specific */
TUNABLE_INT("hw.igb.eee_disabled", &igb_eee_disabled);
TUNABLE_INT("hw.igb.dma_coalesce", &igb_dma_coalesce);

static __inline void
igb_rxcsum(uint32_t staterr, struct mbuf *mp)
{
	/* Ignore Checksum bit is set */
	if (staterr & E1000_RXD_STAT_IXSM)
		return;

	if ((staterr & (E1000_RXD_STAT_IPCS | E1000_RXDEXT_STATERR_IPE)) ==
	    E1000_RXD_STAT_IPCS)
		mp->m_pkthdr.csum_flags |= CSUM_IP_CHECKED | CSUM_IP_VALID;

	if (staterr & (E1000_RXD_STAT_TCPCS | E1000_RXD_STAT_UDPCS)) {
		if ((staterr & E1000_RXDEXT_STATERR_TCPE) == 0) {
			mp->m_pkthdr.csum_flags |= CSUM_DATA_VALID |
			    CSUM_PSEUDO_HDR | CSUM_FRAG_NOT_CHECKED;
			mp->m_pkthdr.csum_data = htons(0xffff);
		}
	}
}

static int
igb_probe(device_t dev)
{
	const struct igb_device *d;
	uint16_t vid, did;

	vid = pci_get_vendor(dev);
	did = pci_get_device(dev);

	for (d = igb_devices; d->desc != NULL; ++d) {
		if (vid == d->vid && did == d->did) {
			device_set_desc(dev, d->desc);
			return 0;
		}
	}
	return ENXIO;
}

static int
igb_attach(device_t dev)
{
	struct igb_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint16_t eeprom_data;
	u_int intr_flags;
	int error = 0;

#ifdef notyet
	/* SYSCTL stuff */
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
	    OID_AUTO, "nvm", CTLTYPE_INT|CTLFLAG_RW, adapter, 0,
	    igb_sysctl_nvm_info, "I", "NVM Information");

	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
	    OID_AUTO, "enable_aim", CTLTYPE_INT|CTLFLAG_RW,
	    &igb_enable_aim, 1, "Interrupt Moderation");

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
	    OID_AUTO, "flow_control", CTLTYPE_INT|CTLFLAG_RW,
	    adapter, 0, igb_set_flowcntl, "I", "Flow Control");
#endif

	callout_init_mp(&sc->timer);

	sc->dev = sc->osdep.dev = dev;

	/*
	 * Determine hardware and mac type
	 */
	sc->hw.vendor_id = pci_get_vendor(dev);
	sc->hw.device_id = pci_get_device(dev);
	sc->hw.revision_id = pci_read_config(dev, PCIR_REVID, 1);
	sc->hw.subsystem_vendor_id = pci_read_config(dev, PCIR_SUBVEND_0, 2);
	sc->hw.subsystem_device_id = pci_read_config(dev, PCIR_SUBDEV_0, 2);

	if (e1000_set_mac_type(&sc->hw))
		return ENXIO;

	/* Are we a VF device? */
	if (sc->hw.mac.type == e1000_vfadapt ||
	    sc->hw.mac.type == e1000_vfadapt_i350)
		sc->vf_ifp = 1;
	else
		sc->vf_ifp = 0;

	/* Enable bus mastering */
	pci_enable_busmaster(dev);

	/*
	 * Allocate IO memory
	 */
	sc->mem_rid = PCIR_BAR(0);
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->mem_rid,
	    RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Unable to allocate bus resource: memory\n");
		error = ENXIO;
		goto failed;
	}
	sc->osdep.mem_bus_space_tag = rman_get_bustag(sc->mem_res);
	sc->osdep.mem_bus_space_handle = rman_get_bushandle(sc->mem_res);

	sc->hw.hw_addr = (uint8_t *)&sc->osdep.mem_bus_space_handle;

	/*
	 * Allocate interrupt
	 */
	sc->intr_type = pci_alloc_1intr(dev, igb_msi_enable,
	    &sc->intr_rid, &intr_flags);

	sc->intr_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->intr_rid,
	    intr_flags);
	if (sc->intr_res == NULL) {
		device_printf(dev, "Unable to allocate bus resource: "
		    "interrupt\n");
		error = ENXIO;
		goto failed;
	}

	/* Save PCI command register for Shared Code */
	sc->hw.bus.pci_cmd_word = pci_read_config(dev, PCIR_COMMAND, 2);
	sc->hw.back = &sc->osdep;

	sc->num_queues = 1; /* Defaults for Legacy or MSI */
	sc->intr_rate = IGB_INTR_RATE;

	/* Do Shared Code initialization */
	if (e1000_setup_init_funcs(&sc->hw, TRUE)) {
		device_printf(dev, "Setup of Shared code failed\n");
		error = ENXIO;
		goto failed;
	}

	e1000_get_bus_info(&sc->hw);

	sc->hw.mac.autoneg = DO_AUTO_NEG;
	sc->hw.phy.autoneg_wait_to_complete = FALSE;
	sc->hw.phy.autoneg_advertised = AUTONEG_ADV_DEFAULT;

	/* Copper options */
	if (sc->hw.phy.media_type == e1000_media_type_copper) {
		sc->hw.phy.mdix = AUTO_ALL_MODES;
		sc->hw.phy.disable_polarity_correction = FALSE;
		sc->hw.phy.ms_type = IGB_MASTER_SLAVE;
	}

	/* Set the frame limits assuming  standard ethernet sized frames. */
	sc->max_frame_size = ETHERMTU + ETHER_HDR_LEN + ETHER_CRC_LEN;
	sc->min_frame_size = ETHER_MIN_LEN;

	/* Allocate RX/TX rings' busdma(9) stuffs */
	error = igb_dma_alloc(sc);
	if (error)
		goto failed;

	/* Allocate the appropriate stats memory */
	if (sc->vf_ifp) {
		sc->stats = kmalloc(sizeof(struct e1000_vf_stats), M_DEVBUF,
		    M_WAITOK | M_ZERO);
		igb_vf_init_stats(sc);
	} else {
		sc->stats = kmalloc(sizeof(struct e1000_hw_stats), M_DEVBUF,
		    M_WAITOK | M_ZERO);
	}

	/* Allocate multicast array memory. */
	sc->mta = kmalloc(ETHER_ADDR_LEN * MAX_NUM_MULTICAST_ADDRESSES,
	    M_DEVBUF, M_WAITOK);

	/* Some adapter-specific advanced features */
	if (sc->hw.mac.type >= e1000_i350) {
#ifdef notyet
		igb_set_sysctl_value(adapter, "dma_coalesce",
		    "configure dma coalesce",
		    &adapter->dma_coalesce, igb_dma_coalesce);
		igb_set_sysctl_value(adapter, "eee_disabled",
		    "enable Energy Efficient Ethernet",
		    &adapter->hw.dev_spec._82575.eee_disable,
		    igb_eee_disabled);
#else
		sc->dma_coalesce = igb_dma_coalesce;
		sc->hw.dev_spec._82575.eee_disable = igb_eee_disabled;
#endif
		e1000_set_eee_i350(&sc->hw);
	}

	/*
	 * Start from a known state, this is important in reading the nvm and
	 * mac from that.
	 */
	e1000_reset_hw(&sc->hw);

	/* Make sure we have a good EEPROM before we read from it */
	if (e1000_validate_nvm_checksum(&sc->hw) < 0) {
		/*
		 * Some PCI-E parts fail the first check due to
		 * the link being in sleep state, call it again,
		 * if it fails a second time its a real issue.
		 */
		if (e1000_validate_nvm_checksum(&sc->hw) < 0) {
			device_printf(dev,
			    "The EEPROM Checksum Is Not Valid\n");
			error = EIO;
			goto failed;
		}
	}

	/* Copy the permanent MAC address out of the EEPROM */
	if (e1000_read_mac_addr(&sc->hw) < 0) {
		device_printf(dev, "EEPROM read error while reading MAC"
		    " address\n");
		error = EIO;
		goto failed;
	}
	if (!igb_is_valid_ether_addr(sc->hw.mac.addr)) {
		device_printf(dev, "Invalid MAC address\n");
		error = EIO;
		goto failed;
	}

#ifdef notyet
	/* 
	** Configure Interrupts
	*/
	if ((adapter->msix > 1) && (igb_enable_msix))
		error = igb_allocate_msix(adapter);
	else /* MSI or Legacy */
		error = igb_allocate_legacy(adapter);
	if (error)
		goto err_late;
#endif

	/* Setup OS specific network interface */
	igb_setup_ifp(sc);

	/* Add sysctl tree, must after igb_setup_ifp() */
	igb_add_sysctl(sc);

	/* Now get a good starting state */
	igb_reset(sc);

	/* Initialize statistics */
	igb_update_stats_counters(sc);

	sc->hw.mac.get_link_status = 1;
	igb_update_link_status(sc);

	/* Indicate SOL/IDER usage */
	if (e1000_check_reset_block(&sc->hw)) {
		device_printf(dev,
		    "PHY reset is blocked due to SOL/IDER session.\n");
	}

	/* Determine if we have to control management hardware */
	sc->has_manage = e1000_enable_mng_pass_thru(&sc->hw);

	/*
	 * Setup Wake-on-Lan
	 */
	/* APME bit in EEPROM is mapped to WUC.APME */
	eeprom_data = E1000_READ_REG(&sc->hw, E1000_WUC) & E1000_WUC_APME;
	if (eeprom_data)
		sc->wol = E1000_WUFC_MAG;
	/* XXX disable WOL */
	sc->wol = 0; 

#ifdef notyet
	/* Register for VLAN events */
	adapter->vlan_attach = EVENTHANDLER_REGISTER(vlan_config,
	     igb_register_vlan, adapter, EVENTHANDLER_PRI_FIRST);
	adapter->vlan_detach = EVENTHANDLER_REGISTER(vlan_unconfig,
	     igb_unregister_vlan, adapter, EVENTHANDLER_PRI_FIRST);
#endif

#ifdef notyet
	igb_add_hw_stats(adapter);
#endif

	error = bus_setup_intr(dev, sc->intr_res, INTR_MPSAFE, igb_intr, sc,
	    &sc->intr_tag, ifp->if_serializer);
	if (error) {
		device_printf(dev, "Failed to register interrupt handler");
		ether_ifdetach(&sc->arpcom.ac_if);
		goto failed;
	}

	ifp->if_cpuid = rman_get_cpuid(sc->intr_res);
	KKASSERT(ifp->if_cpuid >= 0 && ifp->if_cpuid < ncpus);

	return 0;

failed:
	igb_detach(dev);
	return error;
}

static int
igb_detach(device_t dev)
{
	struct igb_softc *sc = device_get_softc(dev);

	if (device_is_attached(dev)) {
		struct ifnet *ifp = &sc->arpcom.ac_if;

		ifnet_serialize_all(ifp);

		igb_stop(sc);

		e1000_phy_hw_reset(&sc->hw);

		/* Give control back to firmware */
		igb_rel_mgmt(sc);
		igb_rel_hw_control(sc);

		if (sc->wol) {
			E1000_WRITE_REG(&sc->hw, E1000_WUC, E1000_WUC_PME_EN);
			E1000_WRITE_REG(&sc->hw, E1000_WUFC, sc->wol);
			igb_enable_wol(dev);
		}

		bus_teardown_intr(dev, sc->intr_res, sc->intr_tag);

		ifnet_deserialize_all(ifp);

		ether_ifdetach(ifp);
	} else if (sc->mem_res != NULL) {
		igb_rel_hw_control(sc);
	}
	bus_generic_detach(dev);

	if (sc->intr_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->intr_rid,
		    sc->intr_res);
	}
	if (sc->intr_type == PCI_INTR_TYPE_MSI)
		pci_release_msi(dev);

	if (sc->mem_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->mem_rid,
		    sc->mem_res);
	}

	igb_dma_free(sc);

	if (sc->mta != NULL)
		kfree(sc->mta, M_DEVBUF);
	if (sc->stats != NULL)
		kfree(sc->stats, M_DEVBUF);

	if (sc->sysctl_tree != NULL)
		sysctl_ctx_free(&sc->sysctl_ctx);

	return 0;
}

static int
igb_shutdown(device_t dev)
{
	return igb_suspend(dev);
}

static int
igb_suspend(device_t dev)
{
	struct igb_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;

	ifnet_serialize_all(ifp);

	igb_stop(sc);

	igb_rel_mgmt(sc);
	igb_rel_hw_control(sc);

	if (sc->wol) {
		E1000_WRITE_REG(&sc->hw, E1000_WUC, E1000_WUC_PME_EN);
		E1000_WRITE_REG(&sc->hw, E1000_WUFC, sc->wol);
		igb_enable_wol(dev);
	}

	ifnet_deserialize_all(ifp);

	return bus_generic_suspend(dev);
}

static int
igb_resume(device_t dev)
{
	struct igb_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;

	ifnet_serialize_all(ifp);

	igb_init(sc);
	igb_get_mgmt(sc);

	if_devstart(ifp);

	ifnet_deserialize_all(ifp);

	return bus_generic_resume(dev);
}

static int
igb_ioctl(struct ifnet *ifp, u_long command, caddr_t data, struct ucred *cr)
{
	struct igb_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	int max_frame_size, mask, reinit;
	int error = 0;

	ASSERT_IFNET_SERIALIZED_ALL(ifp);

	switch (command) {
	case SIOCSIFMTU:
		max_frame_size = 9234;
		if (ifr->ifr_mtu > max_frame_size - ETHER_HDR_LEN -
		    ETHER_CRC_LEN) {
			error = EINVAL;
			break;
		}

		ifp->if_mtu = ifr->ifr_mtu;
		sc->max_frame_size = ifp->if_mtu + ETHER_HDR_LEN +
		    ETHER_CRC_LEN;

		if (ifp->if_flags & IFF_RUNNING)
			igb_init(sc);
		break;

	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (ifp->if_flags & IFF_RUNNING) {
				if ((ifp->if_flags ^ sc->if_flags) &
				    (IFF_PROMISC | IFF_ALLMULTI)) {
					igb_disable_promisc(sc);
					igb_set_promisc(sc);
				}
			} else {
				igb_init(sc);
			}
		} else if (ifp->if_flags & IFF_RUNNING) {
			igb_stop(sc);
		}
		sc->if_flags = ifp->if_flags;
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (ifp->if_flags & IFF_RUNNING) {
			igb_disable_intr(sc);
			igb_set_multi(sc);
#ifdef DEVICE_POLLING
			if (!(ifp->if_flags & IFF_POLLING))
#endif
				igb_enable_intr(sc);
		}
		break;

	case SIOCSIFMEDIA:
		/*
		 * As the speed/duplex settings are being
		 * changed, we need toreset the PHY.
		 */
		sc->hw.phy.reset_disable = FALSE;

		/* Check SOL/IDER usage */
		if (e1000_check_reset_block(&sc->hw)) {
			if_printf(ifp, "Media change is "
			    "blocked due to SOL/IDER session.\n");
			break;
		}
		/* FALL THROUGH */

	case SIOCGIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->media, command);
		break;

	case SIOCSIFCAP:
		reinit = 0;
		mask = ifr->ifr_reqcap ^ ifp->if_capenable;
		if (mask & IFCAP_HWCSUM) {
			ifp->if_capenable ^= (mask & IFCAP_HWCSUM);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWTAGGING) {
			ifp->if_capenable ^= IFCAP_VLAN_HWTAGGING;
			reinit = 1;
		}
		if (reinit && (ifp->if_flags & IFF_RUNNING))
			igb_init(sc);
		break;

	default:
		error = ether_ioctl(ifp, command, data);
		break;
	}
	return error;
}

static void
igb_init(void *xsc)
{
	struct igb_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int i;

	ASSERT_IFNET_SERIALIZED_ALL(ifp);

	igb_stop(sc);

	/* Get the latest mac address, User can use a LAA */
	bcopy(IF_LLADDR(ifp), sc->hw.mac.addr, ETHER_ADDR_LEN);

	/* Put the address into the Receive Address Array */
	e1000_rar_set(&sc->hw, sc->hw.mac.addr, 0);

	igb_reset(sc);
	igb_update_link_status(sc);

	E1000_WRITE_REG(&sc->hw, E1000_VET, ETHERTYPE_VLAN);

	/* Set hardware offload abilities */
	if (ifp->if_capenable & IFCAP_TXCSUM)
		ifp->if_hwassist = IGB_CSUM_FEATURES;
	else
		ifp->if_hwassist = 0;

	/* Configure for OS presence */
	igb_get_mgmt(sc);

	/* Prepare transmit descriptors and buffers */
	for (i = 0; i < sc->num_queues; ++i)
		igb_init_tx_ring(&sc->tx_rings[i]);
	igb_init_tx_unit(sc);

	/* Setup Multicast table */
	igb_set_multi(sc);

#if 0
	/*
	 * Figure out the desired mbuf pool
	 * for doing jumbo/packetsplit
	 */
	if (adapter->max_frame_size <= 2048)
		adapter->rx_mbuf_sz = MCLBYTES;
	else if (adapter->max_frame_size <= 4096)
		adapter->rx_mbuf_sz = MJUMPAGESIZE;
	else
		adapter->rx_mbuf_sz = MJUM9BYTES;
#else
	sc->rx_mbuf_sz = MCLBYTES;
#endif

	/* Prepare receive descriptors and buffers */
	for (i = 0; i < sc->num_queues; ++i) {
		int error;

		error = igb_init_rx_ring(&sc->rx_rings[i]);
		if (error) {
			if_printf(ifp, "Could not setup receive structures\n");
			igb_stop(sc);
			return;
		}
	}
	igb_init_rx_unit(sc);

	/* Enable VLAN support */
	if (ifp->if_capenable & IFCAP_VLAN_HWTAGGING)
		igb_set_vlan(sc);

	/* Don't lose promiscuous settings */
	igb_set_promisc(sc);

	/* Configure interrupt moderation */
	igb_set_itr(sc);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;

	callout_reset(&sc->timer, hz, igb_timer, sc);
	e1000_clear_hw_cntrs_base_generic(&sc->hw);

#if 0
	if (adapter->msix > 1) /* Set up queue routing */
		igb_configure_queues(adapter);
#endif

	/* this clears any pending interrupts */
	E1000_READ_REG(&sc->hw, E1000_ICR);
#ifdef DEVICE_POLLING
	/*
	 * Only enable interrupts if we are not polling, make sure
	 * they are off otherwise.
	 */
	if (ifp->if_flags & IFF_POLLING)
		igb_disable_intr(sc);
	else
#endif /* DEVICE_POLLING */
	{
		igb_enable_intr(sc);
		E1000_WRITE_REG(&sc->hw, E1000_ICS, E1000_ICS_LSC);
	}

	/* Set Energy Efficient Ethernet */
	e1000_set_eee_i350(&sc->hw);

	/* Don't reset the phy next time init gets called */
	sc->hw.phy.reset_disable = TRUE;
}

static void
igb_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct igb_softc *sc = ifp->if_softc;
	u_char fiber_type = IFM_1000_SX;

	ASSERT_IFNET_SERIALIZED_ALL(ifp);

	igb_update_link_status(sc);

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;

	if (!sc->link_active)
		return;

	ifmr->ifm_status |= IFM_ACTIVE;

	if (sc->hw.phy.media_type == e1000_media_type_fiber ||
	    sc->hw.phy.media_type == e1000_media_type_internal_serdes) {
		ifmr->ifm_active |= fiber_type | IFM_FDX;
	} else {
		switch (sc->link_speed) {
		case 10:
			ifmr->ifm_active |= IFM_10_T;
			break;

		case 100:
			ifmr->ifm_active |= IFM_100_TX;
			break;

		case 1000:
			ifmr->ifm_active |= IFM_1000_T;
			break;
		}
		if (sc->link_duplex == FULL_DUPLEX)
			ifmr->ifm_active |= IFM_FDX;
		else
			ifmr->ifm_active |= IFM_HDX;
	}
}

static int
igb_media_change(struct ifnet *ifp)
{
	struct igb_softc *sc = ifp->if_softc;
	struct ifmedia *ifm = &sc->media;

	ASSERT_IFNET_SERIALIZED_ALL(ifp);

	if (IFM_TYPE(ifm->ifm_media) != IFM_ETHER)
		return EINVAL;

	switch (IFM_SUBTYPE(ifm->ifm_media)) {
	case IFM_AUTO:
		sc->hw.mac.autoneg = DO_AUTO_NEG;
		sc->hw.phy.autoneg_advertised = AUTONEG_ADV_DEFAULT;
		break;

	case IFM_1000_LX:
	case IFM_1000_SX:
	case IFM_1000_T:
		sc->hw.mac.autoneg = DO_AUTO_NEG;
		sc->hw.phy.autoneg_advertised = ADVERTISE_1000_FULL;
		break;

	case IFM_100_TX:
		sc->hw.mac.autoneg = FALSE;
		sc->hw.phy.autoneg_advertised = 0;
		if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX)
			sc->hw.mac.forced_speed_duplex = ADVERTISE_100_FULL;
		else
			sc->hw.mac.forced_speed_duplex = ADVERTISE_100_HALF;
		break;

	case IFM_10_T:
		sc->hw.mac.autoneg = FALSE;
		sc->hw.phy.autoneg_advertised = 0;
		if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX)
			sc->hw.mac.forced_speed_duplex = ADVERTISE_10_FULL;
		else
			sc->hw.mac.forced_speed_duplex = ADVERTISE_10_HALF;
		break;

	default:
		if_printf(ifp, "Unsupported media type\n");
		break;
	}

	igb_init(sc);

	return 0;
}

static void
igb_set_promisc(struct igb_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct e1000_hw *hw = &sc->hw;
	uint32_t reg;

	if (sc->vf_ifp) {
		e1000_promisc_set_vf(hw, e1000_promisc_enabled);
		return;
	}

	reg = E1000_READ_REG(hw, E1000_RCTL);
	if (ifp->if_flags & IFF_PROMISC) {
		reg |= (E1000_RCTL_UPE | E1000_RCTL_MPE);
		E1000_WRITE_REG(hw, E1000_RCTL, reg);
	} else if (ifp->if_flags & IFF_ALLMULTI) {
		reg |= E1000_RCTL_MPE;
		reg &= ~E1000_RCTL_UPE;
		E1000_WRITE_REG(hw, E1000_RCTL, reg);
	}
}

static void
igb_disable_promisc(struct igb_softc *sc)
{
	struct e1000_hw *hw = &sc->hw;
	uint32_t reg;

	if (sc->vf_ifp) {
		e1000_promisc_set_vf(hw, e1000_promisc_disabled);
		return;
	}
	reg = E1000_READ_REG(hw, E1000_RCTL);
	reg &= ~E1000_RCTL_UPE;
	reg &= ~E1000_RCTL_MPE;
	E1000_WRITE_REG(hw, E1000_RCTL, reg);
}

static void
igb_set_multi(struct igb_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct ifmultiaddr *ifma;
	uint32_t reg_rctl = 0;
	uint8_t *mta;
	int mcnt = 0;

	mta = sc->mta;
	bzero(mta, ETH_ADDR_LEN * MAX_NUM_MULTICAST_ADDRESSES);

	TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (ifma->ifma_addr->sa_family != AF_LINK)
			continue;

		if (mcnt == MAX_NUM_MULTICAST_ADDRESSES)
			break;

		bcopy(LLADDR((struct sockaddr_dl *)ifma->ifma_addr),
		    &mta[mcnt * ETH_ADDR_LEN], ETH_ADDR_LEN);
		mcnt++;
	}

	if (mcnt >= MAX_NUM_MULTICAST_ADDRESSES) {
		reg_rctl = E1000_READ_REG(&sc->hw, E1000_RCTL);
		reg_rctl |= E1000_RCTL_MPE;
		E1000_WRITE_REG(&sc->hw, E1000_RCTL, reg_rctl);
	} else {
		e1000_update_mc_addr_list(&sc->hw, mta, mcnt);
	}
}

static void
igb_timer(void *xsc)
{
	struct igb_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;

	ifnet_serialize_all(ifp);

	igb_update_link_status(sc);
	igb_update_stats_counters(sc);

	callout_reset(&sc->timer, hz, igb_timer, sc);

	ifnet_deserialize_all(ifp);
}

static void
igb_update_link_status(struct igb_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct e1000_hw *hw = &sc->hw;
	uint32_t link_check, thstat, ctrl;

	link_check = thstat = ctrl = 0;

	/* Get the cached link value or read for real */
	switch (hw->phy.media_type) {
	case e1000_media_type_copper:
		if (hw->mac.get_link_status) {
			/* Do the work to read phy */
			e1000_check_for_link(hw);
			link_check = !hw->mac.get_link_status;
		} else {
			link_check = TRUE;
		}
		break;

	case e1000_media_type_fiber:
		e1000_check_for_link(hw);
		link_check = E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_LU;
		break;

	case e1000_media_type_internal_serdes:
		e1000_check_for_link(hw);
		link_check = hw->mac.serdes_has_link;
		break;

	/* VF device is type_unknown */
	case e1000_media_type_unknown:
		e1000_check_for_link(hw);
		link_check = !hw->mac.get_link_status;
		/* Fall thru */
	default:
		break;
	}

	/* Check for thermal downshift or shutdown */
	if (hw->mac.type == e1000_i350) {
		thstat = E1000_READ_REG(hw, E1000_THSTAT);
		ctrl = E1000_READ_REG(hw, E1000_CTRL_EXT);
	}

	/* Now we check if a transition has happened */
	if (link_check && sc->link_active == 0) {
		e1000_get_speed_and_duplex(hw, 
		    &sc->link_speed, &sc->link_duplex);
		if (bootverbose) {
			if_printf(ifp, "Link is up %d Mbps %s\n",
			    sc->link_speed,
			    sc->link_duplex == FULL_DUPLEX ?
			    "Full Duplex" : "Half Duplex");
		}
		sc->link_active = 1;

		ifp->if_baudrate = sc->link_speed * 1000000;
		if ((ctrl & E1000_CTRL_EXT_LINK_MODE_GMII) &&
		    (thstat & E1000_THSTAT_LINK_THROTTLE))
			if_printf(ifp, "Link: thermal downshift\n");
		/* This can sleep */
		ifp->if_link_state = LINK_STATE_UP;
		if_link_state_change(ifp);
	} else if (!link_check && sc->link_active == 1) {
		ifp->if_baudrate = sc->link_speed = 0;
		sc->link_duplex = 0;
		if (bootverbose)
			if_printf(ifp, "Link is Down\n");
		if ((ctrl & E1000_CTRL_EXT_LINK_MODE_GMII) &&
		    (thstat & E1000_THSTAT_PWR_DOWN))
			if_printf(ifp, "Link: thermal shutdown\n");
		sc->link_active = 0;
		/* This can sleep */
		ifp->if_link_state = LINK_STATE_DOWN;
		if_link_state_change(ifp);
	}
}

static void
igb_stop(struct igb_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int i;

	ASSERT_IFNET_SERIALIZED_ALL(ifp);

	igb_disable_intr(sc);

	callout_stop(&sc->timer);

	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
	ifp->if_timer = 0;

	e1000_reset_hw(&sc->hw);
	E1000_WRITE_REG(&sc->hw, E1000_WUC, 0);

	e1000_led_off(&sc->hw);
	e1000_cleanup_led(&sc->hw);

	for (i = 0; i < sc->num_queues; ++i)
		igb_free_tx_ring(&sc->tx_rings[i]);
	for (i = 0; i < sc->num_queues; ++i)
		igb_free_rx_ring(&sc->rx_rings[i]);
}

static void
igb_reset(struct igb_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct e1000_hw *hw = &sc->hw;
	struct e1000_fc_info *fc = &hw->fc;
	uint32_t pba = 0;
	uint16_t hwm;

	/* Let the firmware know the OS is in control */
	igb_get_hw_control(sc);

	/*
	 * Packet Buffer Allocation (PBA)
	 * Writing PBA sets the receive portion of the buffer
	 * the remainder is used for the transmit buffer.
	 */
	switch (hw->mac.type) {
	case e1000_82575:
		pba = E1000_PBA_32K;
		break;

	case e1000_82576:
	case e1000_vfadapt:
		pba = E1000_READ_REG(hw, E1000_RXPBS);
		pba &= E1000_RXPBS_SIZE_MASK_82576;
		break;

	case e1000_82580:
	case e1000_i350:
	case e1000_vfadapt_i350:
		pba = E1000_READ_REG(hw, E1000_RXPBS);
		pba = e1000_rxpbs_adjust_82580(pba);
		break;
		/* XXX pba = E1000_PBA_35K; */

	default:
		break;
	}

	/* Special needs in case of Jumbo frames */
	if (hw->mac.type == e1000_82575 && ifp->if_mtu > ETHERMTU) {
		uint32_t tx_space, min_tx, min_rx;

		pba = E1000_READ_REG(hw, E1000_PBA);
		tx_space = pba >> 16;
		pba &= 0xffff;

		min_tx = (sc->max_frame_size +
		    sizeof(struct e1000_tx_desc) - ETHER_CRC_LEN) * 2;
		min_tx = roundup2(min_tx, 1024);
		min_tx >>= 10;
		min_rx = sc->max_frame_size;
		min_rx = roundup2(min_rx, 1024);
		min_rx >>= 10;
		if (tx_space < min_tx && (min_tx - tx_space) < pba) {
			pba = pba - (min_tx - tx_space);
			/*
			 * if short on rx space, rx wins
			 * and must trump tx adjustment
			 */
			if (pba < min_rx)
				pba = min_rx;
		}
		E1000_WRITE_REG(hw, E1000_PBA, pba);
	}

	/*
	 * These parameters control the automatic generation (Tx) and
	 * response (Rx) to Ethernet PAUSE frames.
	 * - High water mark should allow for at least two frames to be
	 *   received after sending an XOFF.
	 * - Low water mark works best when it is very near the high water mark.
	 *   This allows the receiver to restart by sending XON when it has
	 *   drained a bit.
	 */
	hwm = min(((pba << 10) * 9 / 10),
	    ((pba << 10) - 2 * sc->max_frame_size));

	if (hw->mac.type < e1000_82576) {
		fc->high_water = hwm & 0xFFF8; /* 8-byte granularity */
		fc->low_water = fc->high_water - 8;
	} else {
		fc->high_water = hwm & 0xFFF0; /* 16-byte granularity */
		fc->low_water = fc->high_water - 16;
	}
	fc->pause_time = IGB_FC_PAUSE_TIME;
	fc->send_xon = TRUE;

	/* Issue a global reset */
	e1000_reset_hw(hw);
	E1000_WRITE_REG(hw, E1000_WUC, 0);

	if (e1000_init_hw(hw) < 0)
		if_printf(ifp, "Hardware Initialization Failed\n");

	/* Setup DMA Coalescing */
	if (hw->mac.type == e1000_i350 && sc->dma_coalesce) {
		uint32_t reg;

		hwm = (pba - 4) << 10;
		reg = ((pba - 6) << E1000_DMACR_DMACTHR_SHIFT)
		    & E1000_DMACR_DMACTHR_MASK;

		/* transition to L0x or L1 if available..*/
		reg |= (E1000_DMACR_DMAC_EN | E1000_DMACR_DMAC_LX_MASK);

		/* timer = +-1000 usec in 32usec intervals */
		reg |= (1000 >> 5);
		E1000_WRITE_REG(hw, E1000_DMACR, reg);

		/* No lower threshold */
		E1000_WRITE_REG(hw, E1000_DMCRTRH, 0);

		/* set hwm to PBA -  2 * max frame size */
		E1000_WRITE_REG(hw, E1000_FCRTC, hwm);

		/* Set the interval before transition */
		reg = E1000_READ_REG(hw, E1000_DMCTLX);
		reg |= 0x800000FF; /* 255 usec */
		E1000_WRITE_REG(hw, E1000_DMCTLX, reg);

		/* free space in tx packet buffer to wake from DMA coal */
		E1000_WRITE_REG(hw, E1000_DMCTXTH,
		    (20480 - (2 * sc->max_frame_size)) >> 6);

		/* make low power state decision controlled by DMA coal */
		reg = E1000_READ_REG(hw, E1000_PCIEMISC);
		E1000_WRITE_REG(hw, E1000_PCIEMISC,
		    reg | E1000_PCIEMISC_LX_DECISION);
		if_printf(ifp, "DMA Coalescing enabled\n");
	}

	E1000_WRITE_REG(&sc->hw, E1000_VET, ETHERTYPE_VLAN);
	e1000_get_phy_info(hw);
	e1000_check_for_link(hw);
}

static void
igb_setup_ifp(struct igb_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;

	if_initname(ifp, device_get_name(sc->dev), device_get_unit(sc->dev));
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init =  igb_init;
	ifp->if_ioctl = igb_ioctl;
	ifp->if_start = igb_start;
#ifdef DEVICE_POLLING
	ifp->if_poll = igb_poll;
#endif
	ifp->if_watchdog = igb_watchdog;

	ifq_set_maxlen(&ifp->if_snd, sc->num_tx_desc - 1);
	ifq_set_ready(&ifp->if_snd);

	ether_ifattach(ifp, sc->hw.mac.addr, NULL);

	ifp->if_capabilities =
	    IFCAP_HWCSUM | IFCAP_VLAN_HWTAGGING | IFCAP_VLAN_MTU;
	ifp->if_capenable = ifp->if_capabilities;
	ifp->if_hwassist = IGB_CSUM_FEATURES;

	/*
	 * Tell the upper layer(s) we support long frames
	 */
	ifp->if_data.ifi_hdrlen = sizeof(struct ether_vlan_header);

	/*
	 * Specify the media types supported by this adapter and register
	 * callbacks to update media and link information
	 */
	ifmedia_init(&sc->media, IFM_IMASK, igb_media_change, igb_media_status);
	if (sc->hw.phy.media_type == e1000_media_type_fiber ||
	    sc->hw.phy.media_type == e1000_media_type_internal_serdes) {
		ifmedia_add(&sc->media, IFM_ETHER | IFM_1000_SX | IFM_FDX,
		    0, NULL);
		ifmedia_add(&sc->media, IFM_ETHER | IFM_1000_SX, 0, NULL);
	} else {
		ifmedia_add(&sc->media, IFM_ETHER | IFM_10_T, 0, NULL);
		ifmedia_add(&sc->media, IFM_ETHER | IFM_10_T | IFM_FDX,
		    0, NULL);
		ifmedia_add(&sc->media, IFM_ETHER | IFM_100_TX, 0, NULL);
		ifmedia_add(&sc->media, IFM_ETHER | IFM_100_TX | IFM_FDX,
		    0, NULL);
		if (sc->hw.phy.type != e1000_phy_ife) {
			ifmedia_add(&sc->media,
			    IFM_ETHER | IFM_1000_T | IFM_FDX, 0, NULL);
			ifmedia_add(&sc->media,
			    IFM_ETHER | IFM_1000_T, 0, NULL);
		}
	}
	ifmedia_add(&sc->media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->media, IFM_ETHER | IFM_AUTO);
}

static void
igb_add_sysctl(struct igb_softc *sc)
{
	sysctl_ctx_init(&sc->sysctl_ctx);
	sc->sysctl_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO,
	    device_get_nameunit(sc->dev), CTLFLAG_RD, 0, "");
	if (sc->sysctl_tree == NULL) {
		device_printf(sc->dev, "can't add sysctl node\n");
		return;
	}

	SYSCTL_ADD_INT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
	    OID_AUTO, "rxd", CTLFLAG_RD, &sc->num_rx_desc, 0, NULL);
	SYSCTL_ADD_INT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
	    OID_AUTO, "txd", CTLFLAG_RD, &sc->num_tx_desc, 0, NULL);

	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
	    OID_AUTO, "intr_rate", CTLTYPE_INT | CTLFLAG_RW,
	    sc, 0, igb_sysctl_intr_rate, "I", "interrupt rate");
}

static int
igb_dma_alloc(struct igb_softc *sc)
{
	int error, i;

	/* First allocate the top level queue structs */
	sc->queues = kmalloc(sizeof(struct igb_queue) * sc->num_queues,
	    M_DEVBUF, M_WAITOK | M_ZERO);

	/*
	 * Create top level busdma tag
	 */
	error = bus_dma_tag_create(NULL, 1, 0,
	    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
	    BUS_SPACE_MAXSIZE_32BIT, 0, BUS_SPACE_MAXSIZE_32BIT, 0,
	    &sc->parent_tag);
	if (error) {
		device_printf(sc->dev, "could not create top level DMA tag\n");
		return error;
	}

	/*
	 * Allocate TX descriptor rings and buffers
	 */
	sc->tx_rings = kmalloc(sizeof(struct igb_tx_ring) * sc->num_queues,
	    M_DEVBUF, M_WAITOK | M_ZERO);
	for (i = 0; i < sc->num_queues; ++i) {
		struct igb_tx_ring *txr = &sc->tx_rings[i];

		/* Set up some basics */
		txr->sc = sc;
		txr->me = i;

		error = igb_create_tx_ring(txr);
		if (error)
			return error;
	}

	/*
	 * Allocate RX descriptor rings and buffers
	 */ 
	sc->rx_rings = kmalloc(sizeof(struct igb_rx_ring) * sc->num_queues,
	    M_DEVBUF, M_WAITOK | M_ZERO);
	for (i = 0; i < sc->num_queues; ++i) {
		struct igb_rx_ring *rxr = &sc->rx_rings[i];

		/* Set up some basics */
		rxr->sc = sc;
		rxr->me = i;

		error = igb_create_rx_ring(rxr);
		if (error)
			return error;
	}

	/*
	 * Finally set up the queue holding structs
	 */
	for (i = 0; i < sc->num_queues; i++) {
		struct igb_queue *que = &sc->queues[i];

		que->sc = sc;
		que->txr = &sc->tx_rings[i];
		que->rxr = &sc->rx_rings[i];
	}
	return 0;
}

static void
igb_dma_free(struct igb_softc *sc)
{
	int i;

	if (sc->queues != NULL)
		kfree(sc->queues, M_DEVBUF);

	if (sc->tx_rings != NULL) {
		for (i = 0; i < sc->num_queues; ++i)
			igb_destroy_tx_ring(&sc->tx_rings[i], sc->num_tx_desc);
		kfree(sc->tx_rings, M_DEVBUF);
	}

	if (sc->rx_rings != NULL) {
		for (i = 0; i < sc->num_queues; ++i)
			igb_destroy_rx_ring(&sc->rx_rings[i], sc->num_rx_desc);
		kfree(sc->rx_rings, M_DEVBUF);
	}
}

static int
igb_create_tx_ring(struct igb_tx_ring *txr)
{
	int tsize, error, i;

	/*
	 * Validate number of transmit descriptors. It must not exceed
	 * hardware maximum, and must be multiple of IGB_DBA_ALIGN.
	 */
	if (((igb_txd * sizeof(struct e1000_tx_desc)) % IGB_DBA_ALIGN) != 0 ||
	    (igb_txd > IGB_MAX_TXD) || (igb_txd < IGB_MIN_TXD)) {
		device_printf(txr->sc->dev,
		    "Using %d TX descriptors instead of %d!\n",
		    IGB_DEFAULT_TXD, igb_txd);
		txr->sc->num_tx_desc = IGB_DEFAULT_TXD;
	} else {
		txr->sc->num_tx_desc = igb_txd;
	}

	/*
	 * Allocate TX descriptor ring
	 */
	tsize = roundup2(txr->sc->num_tx_desc * sizeof(union e1000_adv_tx_desc),
	    IGB_DBA_ALIGN);
	txr->txdma.dma_vaddr = bus_dmamem_coherent_any(txr->sc->parent_tag,
	    IGB_DBA_ALIGN, tsize, BUS_DMA_WAITOK,
	    &txr->txdma.dma_tag, &txr->txdma.dma_map, &txr->txdma.dma_paddr);
	if (txr->txdma.dma_vaddr == NULL) {
		device_printf(txr->sc->dev,
		    "Unable to allocate TX Descriptor memory\n");
		return ENOMEM;
	}
	txr->tx_base = txr->txdma.dma_vaddr;
	bzero(txr->tx_base, tsize);

	txr->tx_buf = kmalloc(sizeof(struct igb_tx_buf) * txr->sc->num_tx_desc,
	    M_DEVBUF, M_WAITOK | M_ZERO);

	/*
	 * Create DMA tag for TX buffers
	 */
	error = bus_dma_tag_create(txr->sc->parent_tag,
	    1, 0,		/* alignment, bounds */
	    BUS_SPACE_MAXADDR,	/* lowaddr */
	    BUS_SPACE_MAXADDR,	/* highaddr */
	    NULL, NULL,		/* filter, filterarg */
	    IGB_TSO_SIZE,	/* maxsize */
	    IGB_MAX_SCATTER,	/* nsegments */
	    PAGE_SIZE,		/* maxsegsize */
	    BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW |
	    BUS_DMA_ONEBPAGE,	/* flags */
	    &txr->tx_tag);
	if (error) {
		device_printf(txr->sc->dev, "Unable to allocate TX DMA tag\n");
		kfree(txr->tx_buf, M_DEVBUF);
		txr->tx_buf = NULL;
		return error;
	}

	/*
	 * Create DMA maps for TX buffers
	 */
	for (i = 0; i < txr->sc->num_tx_desc; ++i) {
		struct igb_tx_buf *txbuf = &txr->tx_buf[i];

		error = bus_dmamap_create(txr->tx_tag,
		    BUS_DMA_WAITOK | BUS_DMA_ONEBPAGE, &txbuf->map);
		if (error) {
			device_printf(txr->sc->dev,
			    "Unable to create TX DMA map\n");
			igb_destroy_tx_ring(txr, i);
			return error;
		}
	}
	return 0;
}

static void
igb_free_tx_ring(struct igb_tx_ring *txr)
{
	int i;

	for (i = 0; i < txr->sc->num_tx_desc; ++i) {
		struct igb_tx_buf *txbuf = &txr->tx_buf[i];

		if (txbuf->m_head != NULL) {
			bus_dmamap_unload(txr->tx_tag, txbuf->map);
			m_freem(txbuf->m_head);
			txbuf->m_head = NULL;
		}
	}
}

static void
igb_destroy_tx_ring(struct igb_tx_ring *txr, int ndesc)
{
	int i;

	if (txr->txdma.dma_vaddr != NULL) {
		bus_dmamap_unload(txr->txdma.dma_tag, txr->txdma.dma_map);
		bus_dmamem_free(txr->txdma.dma_tag, txr->txdma.dma_vaddr,
		    txr->txdma.dma_map);
		bus_dma_tag_destroy(txr->txdma.dma_tag);
		txr->txdma.dma_vaddr = NULL;
	}

	if (txr->tx_buf == NULL)
		return;

	for (i = 0; i < ndesc; ++i) {
		struct igb_tx_buf *txbuf = &txr->tx_buf[i];

		KKASSERT(txbuf->m_head == NULL);
		bus_dmamap_destroy(txr->tx_tag, txbuf->map);
	}
	bus_dma_tag_destroy(txr->tx_tag);

	kfree(txr->tx_buf, M_DEVBUF);
	txr->tx_buf = NULL;
}

static void
igb_init_tx_ring(struct igb_tx_ring *txr)
{
	int i;

	/* Clear the old descriptor contents */
	bzero(txr->tx_base,
	    sizeof(union e1000_adv_tx_desc) * txr->sc->num_tx_desc);

	/* Reset indices */
	txr->next_avail_desc = 0;
	txr->next_to_clean = 0;

	/* Clear the watch index */
	for (i = 0; i < txr->sc->num_tx_desc; ++i)
		txr->tx_buf[i].next_eop = -1;

	/* Set number of descriptors available */
	txr->tx_avail = txr->sc->num_tx_desc;
}

static void
igb_init_tx_unit(struct igb_softc *sc)
{
	struct e1000_hw *hw = &sc->hw;
	uint32_t tctl;
	int i;

	/* Setup the Tx Descriptor Rings */
	for (i = 0; i < sc->num_queues; ++i) {
		struct igb_tx_ring *txr = &sc->tx_rings[i];
		uint64_t bus_addr = txr->txdma.dma_paddr;
		uint32_t txdctl = 0;

		E1000_WRITE_REG(hw, E1000_TDLEN(i),
		    sc->num_tx_desc * sizeof(struct e1000_tx_desc));
		E1000_WRITE_REG(hw, E1000_TDBAH(i),
		    (uint32_t)(bus_addr >> 32));
		E1000_WRITE_REG(hw, E1000_TDBAL(i),
		    (uint32_t)bus_addr);

		/* Setup the HW Tx Head and Tail descriptor pointers */
		E1000_WRITE_REG(hw, E1000_TDT(i), 0);
		E1000_WRITE_REG(hw, E1000_TDH(i), 0);

		txdctl |= IGB_TX_PTHRESH;
		txdctl |= IGB_TX_HTHRESH << 8;
		txdctl |= IGB_TX_WTHRESH << 16;
		txdctl |= E1000_TXDCTL_QUEUE_ENABLE;
		E1000_WRITE_REG(hw, E1000_TXDCTL(i), txdctl);
	}

	if (sc->vf_ifp)
		return;

	e1000_config_collision_dist(hw);

	/* Program the Transmit Control Register */
	tctl = E1000_READ_REG(hw, E1000_TCTL);
	tctl &= ~E1000_TCTL_CT;
	tctl |= (E1000_TCTL_PSP | E1000_TCTL_RTLC | E1000_TCTL_EN |
	    (E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT));

	/* This write will effectively turn on the transmit unit. */
	E1000_WRITE_REG(hw, E1000_TCTL, tctl);
}

static boolean_t
igb_txctx(struct igb_tx_ring *txr, struct mbuf *mp)
{
	struct e1000_adv_tx_context_desc *TXD;
	struct igb_tx_buf *txbuf;
	uint32_t vlan_macip_lens, type_tucmd_mlhl, mss_l4len_idx;
	struct ether_vlan_header *eh;
	struct ip *ip = NULL;
	int ehdrlen, ctxd, ip_hlen = 0;
	uint16_t etype, vlantag = 0;
	boolean_t offload = TRUE;

	if ((mp->m_pkthdr.csum_flags & IGB_CSUM_FEATURES) == 0)
		offload = FALSE;

	vlan_macip_lens = type_tucmd_mlhl = mss_l4len_idx = 0;
	ctxd = txr->next_avail_desc;
	txbuf = &txr->tx_buf[ctxd];
	TXD = (struct e1000_adv_tx_context_desc *)&txr->tx_base[ctxd];

	/*
	 * In advanced descriptors the vlan tag must 
	 * be placed into the context descriptor, thus
	 * we need to be here just for that setup.
	 */
	if (mp->m_flags & M_VLANTAG) {
		vlantag = htole16(mp->m_pkthdr.ether_vlantag);
		vlan_macip_lens |= (vlantag << E1000_ADVTXD_VLAN_SHIFT);
	} else if (!offload) {
		return FALSE;
	}

	/*
	 * Determine where frame payload starts.
	 * Jump over vlan headers if already present,
	 * helpful for QinQ too.
	 */
	KASSERT(mp->m_len >= ETHER_HDR_LEN,
	    ("igb_txctx_pullup is not called (eh)?\n"));
	eh = mtod(mp, struct ether_vlan_header *);
	if (eh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		KASSERT(mp->m_len >= ETHER_HDR_LEN + EVL_ENCAPLEN,
		    ("igb_txctx_pullup is not called (evh)?\n"));
		etype = ntohs(eh->evl_proto);
		ehdrlen = ETHER_HDR_LEN + EVL_ENCAPLEN;
	} else {
		etype = ntohs(eh->evl_encap_proto);
		ehdrlen = ETHER_HDR_LEN;
	}

	/* Set the ether header length */
	vlan_macip_lens |= ehdrlen << E1000_ADVTXD_MACLEN_SHIFT;

	switch (etype) {
	case ETHERTYPE_IP:
		KASSERT(mp->m_len >= ehdrlen + IGB_IPVHL_SIZE,
		    ("igb_txctx_pullup is not called (eh+ip_vhl)?\n"));

		/* NOTE: We could only safely access ip.ip_vhl part */
		ip = (struct ip *)(mp->m_data + ehdrlen);
		ip_hlen = ip->ip_hl << 2;

		if (mp->m_pkthdr.csum_flags & CSUM_IP)
			type_tucmd_mlhl |= E1000_ADVTXD_TUCMD_IPV4;
		break;

#ifdef notyet
	case ETHERTYPE_IPV6:
		ip6 = (struct ip6_hdr *)(mp->m_data + ehdrlen);
		ip_hlen = sizeof(struct ip6_hdr);
		type_tucmd_mlhl |= E1000_ADVTXD_TUCMD_IPV6;
		break;
#endif

	default:
		offload = FALSE;
		break;
	}

	vlan_macip_lens |= ip_hlen;
	type_tucmd_mlhl |= E1000_ADVTXD_DCMD_DEXT | E1000_ADVTXD_DTYP_CTXT;

	if (mp->m_pkthdr.csum_flags & CSUM_TCP)
		type_tucmd_mlhl |= E1000_ADVTXD_TUCMD_L4T_TCP;
	else if (mp->m_pkthdr.csum_flags & CSUM_UDP)
		type_tucmd_mlhl |= E1000_ADVTXD_TUCMD_L4T_UDP;

	/* 82575 needs the queue index added */
	if (txr->sc->hw.mac.type == e1000_82575)
		mss_l4len_idx = txr->me << 4;

	/* Now copy bits into descriptor */
	TXD->vlan_macip_lens = htole32(vlan_macip_lens);
	TXD->type_tucmd_mlhl = htole32(type_tucmd_mlhl);
	TXD->seqnum_seed = htole32(0);
	TXD->mss_l4len_idx = htole32(mss_l4len_idx);

	txbuf->m_head = NULL;
	txbuf->next_eop = -1;

	/* We've consumed the first desc, adjust counters */
	if (++ctxd == txr->sc->num_tx_desc)
		ctxd = 0;
	txr->next_avail_desc = ctxd;
	--txr->tx_avail;

	return offload;
}

static void
igb_txeof(struct igb_tx_ring *txr)
{
	struct ifnet *ifp = &txr->sc->arpcom.ac_if;
	int first, last, done;
	struct igb_tx_buf *txbuf;
	struct e1000_tx_desc *tx_desc, *eop_desc;

	if (txr->tx_avail == txr->sc->num_tx_desc)
		return;

	first = txr->next_to_clean;
	tx_desc = &txr->tx_base[first];
	txbuf = &txr->tx_buf[first];
	last = txbuf->next_eop;
	eop_desc = &txr->tx_base[last];

	/*
	 * What this does is get the index of the
	 * first descriptor AFTER the EOP of the 
	 * first packet, that way we can do the
	 * simple comparison on the inner while loop.
	 */
	if (++last == txr->sc->num_tx_desc)
		last = 0;
	done = last;

	while (eop_desc->upper.fields.status & E1000_TXD_STAT_DD) {
		/* We clean the range of the packet */
		while (first != done) {
			tx_desc->upper.data = 0;
			tx_desc->lower.data = 0;
			tx_desc->buffer_addr = 0;
			++txr->tx_avail;

			if (txbuf->m_head) {
				bus_dmamap_unload(txr->tx_tag, txbuf->map);
				m_freem(txbuf->m_head);
				txbuf->m_head = NULL;
			}
			txbuf->next_eop = -1;

			if (++first == txr->sc->num_tx_desc)
				first = 0;

			txbuf = &txr->tx_buf[first];
			tx_desc = &txr->tx_base[first];
		}
		++ifp->if_opackets;

		/* See if we can continue to the next packet */
		last = txbuf->next_eop;
		if (last != -1) {
			eop_desc = &txr->tx_base[last];

			/* Get new done point */
			if (++last == txr->sc->num_tx_desc)
				last = 0;
			done = last;
		} else {
			break;
		}
	}
	txr->next_to_clean = first;

	/*
	 * If we have a minimum free, clear IFF_OACTIVE
	 * to tell the stack that it is OK to send packets.
	 */
	if (txr->tx_avail > IGB_TX_CLEANUP_THRESHOLD(txr->sc)) {
		ifp->if_flags &= ~IFF_OACTIVE;

#ifdef foo
		/* All clean, turn off the watchdog */
		if (txr->tx_avail == txr->sc->num_tx_desc)
			ifp->if_timer = 0;
#else
		/*
		 * We have enough TX descriptors, turn off
		 * the watchdog.  On some 82575EB chips,
		 * tiny amount of done TX descriptors will
		 * not trigger TX descriptor write-back.
		 */
		ifp->if_timer = 0;
#endif
	}
}

static int
igb_create_rx_ring(struct igb_rx_ring *rxr)
{
	int rsize, i, error;

	/*
	 * Validate number of receive descriptors. It must not exceed
	 * hardware maximum, and must be multiple of IGB_DBA_ALIGN.
	 */
	if (((igb_rxd * sizeof(struct e1000_rx_desc)) % IGB_DBA_ALIGN) != 0 ||
	    (igb_rxd > IGB_MAX_RXD) || (igb_rxd < IGB_MIN_RXD)) {
		device_printf(rxr->sc->dev,
		    "Using %d RX descriptors instead of %d!\n",
		    IGB_DEFAULT_RXD, igb_rxd);
		rxr->sc->num_rx_desc = IGB_DEFAULT_RXD;
	} else {
		rxr->sc->num_rx_desc = igb_rxd;
	}

	/*
	 * Allocate RX descriptor ring
	 */
	rsize = roundup2(rxr->sc->num_rx_desc * sizeof(union e1000_adv_rx_desc),
	    IGB_DBA_ALIGN);
	rxr->rxdma.dma_vaddr = bus_dmamem_coherent_any(rxr->sc->parent_tag,
	    IGB_DBA_ALIGN, rsize, BUS_DMA_WAITOK,
	    &rxr->rxdma.dma_tag, &rxr->rxdma.dma_map,
	    &rxr->rxdma.dma_paddr);
	if (rxr->rxdma.dma_vaddr == NULL) {
		device_printf(rxr->sc->dev,
		    "Unable to allocate RxDescriptor memory\n");
		return ENOMEM;
	}
	rxr->rx_base = rxr->rxdma.dma_vaddr;
	bzero(rxr->rx_base, rsize);

	rxr->rx_buf = kmalloc(sizeof(struct igb_rx_buf) * rxr->sc->num_rx_desc,
	    M_DEVBUF, M_WAITOK | M_ZERO);

	/*
	 * Create DMA tag for RX buffers
	 */
	error = bus_dma_tag_create(rxr->sc->parent_tag,
	    1, 0,		/* alignment, bounds */
	    BUS_SPACE_MAXADDR,	/* lowaddr */
	    BUS_SPACE_MAXADDR,	/* highaddr */
	    NULL, NULL,		/* filter, filterarg */
	    MCLBYTES,		/* maxsize */
	    1,			/* nsegments */
	    MCLBYTES,		/* maxsegsize */
	    BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW, /* flags */
	    &rxr->rx_tag);
	if (error) {
		device_printf(rxr->sc->dev,
		    "Unable to create RX payload DMA tag\n");
		kfree(rxr->rx_buf, M_DEVBUF);
		rxr->rx_buf = NULL;
		return error;
	}

	/*
	 * Create spare DMA map for RX buffers
	 */
	error = bus_dmamap_create(rxr->rx_tag, BUS_DMA_WAITOK,
	    &rxr->rx_sparemap);
	if (error) {
		device_printf(rxr->sc->dev,
		    "Unable to create spare RX DMA maps\n");
		bus_dma_tag_destroy(rxr->rx_tag);
		kfree(rxr->rx_buf, M_DEVBUF);
		rxr->rx_buf = NULL;
		return error;
	}

	/*
	 * Create DMA maps for RX buffers
	 */
	for (i = 0; i < rxr->sc->num_rx_desc; i++) {
		struct igb_rx_buf *rxbuf = &rxr->rx_buf[i];

		error = bus_dmamap_create(rxr->rx_tag,
		    BUS_DMA_WAITOK, &rxbuf->map);
		if (error) {
			device_printf(rxr->sc->dev,
			    "Unable to create RX DMA maps\n");
			igb_destroy_rx_ring(rxr, i);
			return error;
		}
	}
	return 0;
}

static void
igb_free_rx_ring(struct igb_rx_ring *rxr)
{
	int i;

	for (i = 0; i < rxr->sc->num_rx_desc; ++i) {
		struct igb_rx_buf *rxbuf = &rxr->rx_buf[i];

		if (rxbuf->m_head != NULL) {
			bus_dmamap_unload(rxr->rx_tag, rxbuf->map);
			m_freem(rxbuf->m_head);
			rxbuf->m_head = NULL;
		}
	}

	if (rxr->fmp != NULL)
		m_freem(rxr->fmp);
	rxr->fmp = NULL;
	rxr->lmp = NULL;
}

static void
igb_destroy_rx_ring(struct igb_rx_ring *rxr, int ndesc)
{
	int i;

	if (rxr->rxdma.dma_vaddr != NULL) {
		bus_dmamap_unload(rxr->rxdma.dma_tag, rxr->rxdma.dma_map);
		bus_dmamem_free(rxr->rxdma.dma_tag, rxr->rxdma.dma_vaddr,
		    rxr->rxdma.dma_map);
		bus_dma_tag_destroy(rxr->rxdma.dma_tag);
		rxr->rxdma.dma_vaddr = NULL;
	}

	if (rxr->rx_buf == NULL)
		return;

	for (i = 0; i < ndesc; ++i) {
		struct igb_rx_buf *rxbuf = &rxr->rx_buf[i];

		KKASSERT(rxbuf->m_head == NULL);
		bus_dmamap_destroy(rxr->rx_tag, rxbuf->map);
	}
	bus_dmamap_destroy(rxr->rx_tag, rxr->rx_sparemap);
	bus_dma_tag_destroy(rxr->rx_tag);

	kfree(rxr->rx_buf, M_DEVBUF);
	rxr->rx_buf = NULL;
}

static void
igb_setup_rxdesc(union e1000_adv_rx_desc *rxd, const struct igb_rx_buf *rxbuf)
{
	rxd->read.pkt_addr = htole64(rxbuf->paddr);
	rxd->wb.upper.status_error = 0;
}

static int
igb_newbuf(struct igb_rx_ring *rxr, int i, boolean_t wait)
{
	struct mbuf *m;
	bus_dma_segment_t seg;
	bus_dmamap_t map;
	struct igb_rx_buf *rxbuf;
	int error, nseg;

	m = m_getcl(wait ? MB_WAIT : MB_DONTWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		if (wait) {
			if_printf(&rxr->sc->arpcom.ac_if,
			    "Unable to allocate RX mbuf\n");
		}
		return ENOBUFS;
	}
	m->m_len = m->m_pkthdr.len = MCLBYTES;

	if (rxr->sc->max_frame_size <= MCLBYTES - ETHER_ALIGN)
		m_adj(m, ETHER_ALIGN);

	error = bus_dmamap_load_mbuf_segment(rxr->rx_tag,
	    rxr->rx_sparemap, m, &seg, 1, &nseg, BUS_DMA_NOWAIT);
	if (error) {
		m_freem(m);
		if (wait) {
			if_printf(&rxr->sc->arpcom.ac_if,
			    "Unable to load RX mbuf\n");
		}
		return error;
	}

	rxbuf = &rxr->rx_buf[i];
	if (rxbuf->m_head != NULL)
		bus_dmamap_unload(rxr->rx_tag, rxbuf->map);

	map = rxbuf->map;
	rxbuf->map = rxr->rx_sparemap;
	rxr->rx_sparemap = map;

	rxbuf->m_head = m;
	rxbuf->paddr = seg.ds_addr;

	igb_setup_rxdesc(&rxr->rx_base[i], rxbuf);
	return 0;
}

static int
igb_init_rx_ring(struct igb_rx_ring *rxr)
{
	int i;

	/* Clear the ring contents */
	bzero(rxr->rx_base,
	    rxr->sc->num_rx_desc * sizeof(union e1000_adv_rx_desc));

	/* Now replenish the ring mbufs */
	for (i = 0; i < rxr->sc->num_rx_desc; ++i) {
		int error;

		error = igb_newbuf(rxr, i, TRUE);
		if (error)
			return error;
	}

	/* Setup our descriptor indices */
	rxr->next_to_check = 0;

	rxr->fmp = NULL;
	rxr->lmp = NULL;
	rxr->discard = FALSE;

	return 0;
}

static void
igb_init_rx_unit(struct igb_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct e1000_hw *hw = &sc->hw;
	uint32_t rctl, rxcsum, srrctl = 0;
	int i;

	/*
	 * Make sure receives are disabled while setting
	 * up the descriptor ring
	 */
	rctl = E1000_READ_REG(hw, E1000_RCTL);
	E1000_WRITE_REG(hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);

#if 0
	/*
	** Set up for header split
	*/
	if (igb_header_split) {
		/* Use a standard mbuf for the header */
		srrctl |= IGB_HDR_BUF << E1000_SRRCTL_BSIZEHDRSIZE_SHIFT;
		srrctl |= E1000_SRRCTL_DESCTYPE_HDR_SPLIT_ALWAYS;
	} else
#endif
		srrctl |= E1000_SRRCTL_DESCTYPE_ADV_ONEBUF;

	/*
	** Set up for jumbo frames
	*/
	if (ifp->if_mtu > ETHERMTU) {
		rctl |= E1000_RCTL_LPE;
#if 0
		if (adapter->rx_mbuf_sz == MJUMPAGESIZE) {
			srrctl |= 4096 >> E1000_SRRCTL_BSIZEPKT_SHIFT;
			rctl |= E1000_RCTL_SZ_4096 | E1000_RCTL_BSEX;
		} else if (adapter->rx_mbuf_sz > MJUMPAGESIZE) {
			srrctl |= 8192 >> E1000_SRRCTL_BSIZEPKT_SHIFT;
			rctl |= E1000_RCTL_SZ_8192 | E1000_RCTL_BSEX;
		}
		/* Set maximum packet len */
		psize = adapter->max_frame_size;
		/* are we on a vlan? */
		if (adapter->ifp->if_vlantrunk != NULL)
			psize += VLAN_TAG_SIZE;
		E1000_WRITE_REG(&adapter->hw, E1000_RLPML, psize);
#else
		srrctl |= 2048 >> E1000_SRRCTL_BSIZEPKT_SHIFT;
		rctl |= E1000_RCTL_SZ_2048;
#endif
	} else {
		rctl &= ~E1000_RCTL_LPE;
		srrctl |= 2048 >> E1000_SRRCTL_BSIZEPKT_SHIFT;
		rctl |= E1000_RCTL_SZ_2048;
	}

	/* Setup the Base and Length of the Rx Descriptor Rings */
	for (i = 0; i < sc->num_queues; ++i) {
		struct igb_rx_ring *rxr = &sc->rx_rings[i];
		uint64_t bus_addr = rxr->rxdma.dma_paddr;
		uint32_t rxdctl;

		E1000_WRITE_REG(hw, E1000_RDLEN(i),
		    sc->num_rx_desc * sizeof(struct e1000_rx_desc));
		E1000_WRITE_REG(hw, E1000_RDBAH(i),
		    (uint32_t)(bus_addr >> 32));
		E1000_WRITE_REG(hw, E1000_RDBAL(i),
		    (uint32_t)bus_addr);
		E1000_WRITE_REG(hw, E1000_SRRCTL(i), srrctl);
		/* Enable this Queue */
		rxdctl = E1000_READ_REG(hw, E1000_RXDCTL(i));
		rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;
		rxdctl &= 0xFFF00000;
		rxdctl |= IGB_RX_PTHRESH;
		rxdctl |= IGB_RX_HTHRESH << 8;
		rxdctl |= IGB_RX_WTHRESH << 16;
		E1000_WRITE_REG(hw, E1000_RXDCTL(i), rxdctl);
	}

	/*
	 * Setup for RX MultiQueue
	 */
	rxcsum = E1000_READ_REG(hw, E1000_RXCSUM);
#if 0
	if (adapter->num_queues >1) {
		u32 random[10], mrqc, shift = 0;
		union igb_reta {
			u32 dword;
			u8  bytes[4];
		} reta;

		arc4rand(&random, sizeof(random), 0);
		if (adapter->hw.mac.type == e1000_82575)
			shift = 6;
		/* Warning FM follows */
		for (int i = 0; i < 128; i++) {
			reta.bytes[i & 3] =
			    (i % adapter->num_queues) << shift;
			if ((i & 3) == 3)
				E1000_WRITE_REG(hw,
				    E1000_RETA(i >> 2), reta.dword);
		}
		/* Now fill in hash table */
		mrqc = E1000_MRQC_ENABLE_RSS_4Q;
		for (int i = 0; i < 10; i++)
			E1000_WRITE_REG_ARRAY(hw,
			    E1000_RSSRK(0), i, random[i]);

		mrqc |= (E1000_MRQC_RSS_FIELD_IPV4 |
		    E1000_MRQC_RSS_FIELD_IPV4_TCP);
		mrqc |= (E1000_MRQC_RSS_FIELD_IPV6 |
		    E1000_MRQC_RSS_FIELD_IPV6_TCP);
		mrqc |=( E1000_MRQC_RSS_FIELD_IPV4_UDP |
		    E1000_MRQC_RSS_FIELD_IPV6_UDP);
		mrqc |=( E1000_MRQC_RSS_FIELD_IPV6_UDP_EX |
		    E1000_MRQC_RSS_FIELD_IPV6_TCP_EX);

		E1000_WRITE_REG(hw, E1000_MRQC, mrqc);

		/*
		** NOTE: Receive Full-Packet Checksum Offload 
		** is mutually exclusive with Multiqueue. However
		** this is not the same as TCP/IP checksums which
		** still work.
		*/
		rxcsum |= E1000_RXCSUM_PCSD;
	} else
#endif
	{
		/* Non RSS setup */
		if (ifp->if_capenable & IFCAP_RXCSUM)
			rxcsum |= E1000_RXCSUM_IPPCSE;
		else
			rxcsum &= ~E1000_RXCSUM_TUOFL;
	}
	E1000_WRITE_REG(hw, E1000_RXCSUM, rxcsum);

	/* Setup the Receive Control Register */
	rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
	rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO |
	    E1000_RCTL_RDMTS_HALF |
	    (hw->mac.mc_filter_type << E1000_RCTL_MO_SHIFT);
	/* Strip CRC bytes. */
	rctl |= E1000_RCTL_SECRC;
	/* Make sure VLAN Filters are off */
	rctl &= ~E1000_RCTL_VFE;
	/* Don't store bad packets */
	rctl &= ~E1000_RCTL_SBP;

	/* Enable Receives */
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);

	/*
	 * Setup the HW Rx Head and Tail Descriptor Pointers
	 *   - needs to be after enable
	 */
	for (i = 0; i < sc->num_queues; ++i) {
		struct igb_rx_ring *rxr = &sc->rx_rings[i];

		E1000_WRITE_REG(hw, E1000_RDH(i), rxr->next_to_check);
		E1000_WRITE_REG(hw, E1000_RDT(i), rxr->sc->num_rx_desc - 1);
	}
}

static void
igb_rxeof(struct igb_rx_ring *rxr, int count)
{
	struct ifnet *ifp = &rxr->sc->arpcom.ac_if;
	union e1000_adv_rx_desc	*cur;
	uint32_t staterr;
	int i;

	i = rxr->next_to_check;
	cur = &rxr->rx_base[i];
	staterr = le32toh(cur->wb.upper.status_error);

	if ((staterr & E1000_RXD_STAT_DD) == 0)
		return;

	while ((staterr & E1000_RXD_STAT_DD) && count != 0) {
		struct igb_rx_buf *rxbuf = &rxr->rx_buf[i];
		struct mbuf *m = NULL;
		boolean_t eop;

		eop = (staterr & E1000_RXD_STAT_EOP) ? TRUE : FALSE;
		if (eop)
			--count;

		if ((staterr & E1000_RXDEXT_ERR_FRAME_ERR_MASK) == 0 &&
		    !rxr->discard) {
			struct mbuf *mp = rxbuf->m_head;
			uint16_t vlan;
			int len;

			len = le16toh(cur->wb.upper.length);
			if (rxr->sc->hw.mac.type == e1000_i350 &&
			    (staterr & E1000_RXDEXT_STATERR_LB))
				vlan = be16toh(cur->wb.upper.vlan);
			else
				vlan = le16toh(cur->wb.upper.vlan);

			bus_dmamap_sync(rxr->rx_tag, rxbuf->map,
			    BUS_DMASYNC_POSTREAD);

			if (igb_newbuf(rxr, i, FALSE) != 0) {
				ifp->if_iqdrops++;
				goto discard;
			}

			mp->m_len = len;
			if (rxr->fmp == NULL) {
				mp->m_pkthdr.len = len;
				rxr->fmp = mp;
				rxr->lmp = mp;
			} else {
				rxr->lmp->m_next = mp;
				rxr->lmp = rxr->lmp->m_next;
				rxr->fmp->m_pkthdr.len += len;
			}

			if (eop) {
				m = rxr->fmp;
				rxr->fmp = NULL;
				rxr->lmp = NULL;

				m->m_pkthdr.rcvif = ifp;
				ifp->if_ipackets++;

				if (ifp->if_capenable & IFCAP_RXCSUM)
					igb_rxcsum(staterr, m);

				if (staterr & E1000_RXD_STAT_VP) {
					m->m_pkthdr.ether_vlantag = vlan;
					m->m_flags |= M_VLANTAG;
				}

#if 0
				if (ifp->if_capenable & IFCAP_RSS) {
					pi = emx_rssinfo(m, &pi0, mrq,
							 rss_hash, staterr);
				}
#endif
			}
		} else {
			ifp->if_ierrors++;
discard:
			igb_setup_rxdesc(cur, rxbuf);
			if (!eop)
				rxr->discard = TRUE;
			else
				rxr->discard = FALSE;
			if (rxr->fmp != NULL) {
				m_freem(rxr->fmp);
				rxr->fmp = NULL;
				rxr->lmp = NULL;
			}
			m = NULL;
		}

		if (m != NULL)
			ether_input_pkt(ifp, m, NULL);

		/* Advance our pointers to the next descriptor. */
		if (++i == rxr->sc->num_rx_desc)
			i = 0;

		cur = &rxr->rx_base[i];
		staterr = le32toh(cur->wb.upper.status_error);
	}
	rxr->next_to_check = i;

	if (--i < 0)
		i = rxr->sc->num_rx_desc - 1;
	E1000_WRITE_REG(&rxr->sc->hw, E1000_RDT(rxr->me), i);
}


static void
igb_set_vlan(struct igb_softc *sc)
{
	struct e1000_hw *hw = &sc->hw;
	uint32_t reg;
#if 0
	struct ifnet *ifp = sc->arpcom.ac_if;
#endif

	if (sc->vf_ifp) {
		e1000_rlpml_set_vf(hw, sc->max_frame_size + VLAN_TAG_SIZE);
		return;
	}

	reg = E1000_READ_REG(hw, E1000_CTRL);
	reg |= E1000_CTRL_VME;
	E1000_WRITE_REG(hw, E1000_CTRL, reg);

#if 0
	/* Enable the Filter Table */
	if (ifp->if_capenable & IFCAP_VLAN_HWFILTER) {
		reg = E1000_READ_REG(hw, E1000_RCTL);
		reg &= ~E1000_RCTL_CFIEN;
		reg |= E1000_RCTL_VFE;
		E1000_WRITE_REG(hw, E1000_RCTL, reg);
	}
#endif

	/* Update the frame size */
	E1000_WRITE_REG(&sc->hw, E1000_RLPML,
	    sc->max_frame_size + VLAN_TAG_SIZE);

#if 0
	/* Don't bother with table if no vlans */
	if ((adapter->num_vlans == 0) ||
	    ((ifp->if_capenable & IFCAP_VLAN_HWFILTER) == 0))
		return;
	/*
	** A soft reset zero's out the VFTA, so
	** we need to repopulate it now.
	*/
	for (int i = 0; i < IGB_VFTA_SIZE; i++)
		if (adapter->shadow_vfta[i] != 0) {
			if (adapter->vf_ifp)
				e1000_vfta_set_vf(hw,
				    adapter->shadow_vfta[i], TRUE);
			else
				E1000_WRITE_REG_ARRAY(hw, E1000_VFTA,
				 i, adapter->shadow_vfta[i]);
		}
#endif
}

static void
igb_enable_intr(struct igb_softc *sc)
{
	lwkt_serialize_handler_enable(sc->arpcom.ac_if.if_serializer);

	/* With RSS set up what to auto clear */
	if (sc->msix_mem) {
		uint32_t mask = (sc->que_mask | sc->link_mask);

		E1000_WRITE_REG(&sc->hw, E1000_EIAC, mask);
		E1000_WRITE_REG(&sc->hw, E1000_EIAM, mask);
		E1000_WRITE_REG(&sc->hw, E1000_EIMS, mask);
		E1000_WRITE_REG(&sc->hw, E1000_IMS, E1000_IMS_LSC);
	} else {
		E1000_WRITE_REG(&sc->hw, E1000_IMS, IMS_ENABLE_MASK);
	}
	E1000_WRITE_FLUSH(&sc->hw);
}

static void
igb_disable_intr(struct igb_softc *sc)
{
	if (sc->msix_mem != NULL) {
		E1000_WRITE_REG(&sc->hw, E1000_EIMC, 0xffffffff);
		E1000_WRITE_REG(&sc->hw, E1000_EIAC, 0);
	} 
	E1000_WRITE_REG(&sc->hw, E1000_IMC, 0xffffffff);
	E1000_WRITE_FLUSH(&sc->hw);

	lwkt_serialize_handler_disable(sc->arpcom.ac_if.if_serializer);
}

/*
 * Bit of a misnomer, what this really means is
 * to enable OS management of the system... aka
 * to disable special hardware management features 
 */
static void
igb_get_mgmt(struct igb_softc *sc)
{
	if (sc->has_manage) {
		int manc2h = E1000_READ_REG(&sc->hw, E1000_MANC2H);
		int manc = E1000_READ_REG(&sc->hw, E1000_MANC);

		/* disable hardware interception of ARP */
		manc &= ~E1000_MANC_ARP_EN;

		/* enable receiving management packets to the host */
		manc |= E1000_MANC_EN_MNG2HOST;
		manc2h |= 1 << 5; /* Mng Port 623 */
		manc2h |= 1 << 6; /* Mng Port 664 */
		E1000_WRITE_REG(&sc->hw, E1000_MANC2H, manc2h);
		E1000_WRITE_REG(&sc->hw, E1000_MANC, manc);
	}
}

/*
 * Give control back to hardware management controller
 * if there is one.
 */
static void
igb_rel_mgmt(struct igb_softc *sc)
{
	if (sc->has_manage) {
		int manc = E1000_READ_REG(&sc->hw, E1000_MANC);

		/* Re-enable hardware interception of ARP */
		manc |= E1000_MANC_ARP_EN;
		manc &= ~E1000_MANC_EN_MNG2HOST;

		E1000_WRITE_REG(&sc->hw, E1000_MANC, manc);
	}
}

/*
 * Sets CTRL_EXT:DRV_LOAD bit.
 *
 * For ASF and Pass Through versions of f/w this means that
 * the driver is loaded. 
 */
static void
igb_get_hw_control(struct igb_softc *sc)
{
	uint32_t ctrl_ext;

	if (sc->vf_ifp)
		return;

	/* Let firmware know the driver has taken over */
	ctrl_ext = E1000_READ_REG(&sc->hw, E1000_CTRL_EXT);
	E1000_WRITE_REG(&sc->hw, E1000_CTRL_EXT,
	    ctrl_ext | E1000_CTRL_EXT_DRV_LOAD);
}

/*
 * Resets CTRL_EXT:DRV_LOAD bit.
 *
 * For ASF and Pass Through versions of f/w this means that the
 * driver is no longer loaded.
 */
static void
igb_rel_hw_control(struct igb_softc *sc)
{
	uint32_t ctrl_ext;

	if (sc->vf_ifp)
		return;

	/* Let firmware taken over control of h/w */
	ctrl_ext = E1000_READ_REG(&sc->hw, E1000_CTRL_EXT);
	E1000_WRITE_REG(&sc->hw, E1000_CTRL_EXT,
	    ctrl_ext & ~E1000_CTRL_EXT_DRV_LOAD);
}

static int
igb_is_valid_ether_addr(const uint8_t *addr)
{
	uint8_t zero_addr[ETHER_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };

	if ((addr[0] & 1) || !bcmp(addr, zero_addr, ETHER_ADDR_LEN))
		return FALSE;
	return TRUE;
}

/*
 * Enable PCI Wake On Lan capability
 */
static void
igb_enable_wol(device_t dev)
{
	uint16_t cap, status;
	uint8_t id;

	/* First find the capabilities pointer*/
	cap = pci_read_config(dev, PCIR_CAP_PTR, 2);

	/* Read the PM Capabilities */
	id = pci_read_config(dev, cap, 1);
	if (id != PCIY_PMG)     /* Something wrong */
		return;

	/*
	 * OK, we have the power capabilities,
	 * so now get the status register
	 */
	cap += PCIR_POWER_STATUS;
	status = pci_read_config(dev, cap, 2);
	status |= PCIM_PSTAT_PME | PCIM_PSTAT_PMEENABLE;
	pci_write_config(dev, cap, status, 2);
}

static void
igb_update_stats_counters(struct igb_softc *sc)
{
	struct e1000_hw *hw = &sc->hw;
	struct e1000_hw_stats *stats;
	struct ifnet *ifp = &sc->arpcom.ac_if;

	/* 
	 * The virtual function adapter has only a
	 * small controlled set of stats, do only 
	 * those and return.
	 */
	if (sc->vf_ifp) {
		igb_update_vf_stats_counters(sc);
		return;
	}
	stats = sc->stats;

	if (sc->hw.phy.media_type == e1000_media_type_copper ||
	    (E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_LU)) {
		stats->symerrs +=
		    E1000_READ_REG(hw,E1000_SYMERRS);
		stats->sec += E1000_READ_REG(hw, E1000_SEC);
	}

	stats->crcerrs += E1000_READ_REG(hw, E1000_CRCERRS);
	stats->mpc += E1000_READ_REG(hw, E1000_MPC);
	stats->scc += E1000_READ_REG(hw, E1000_SCC);
	stats->ecol += E1000_READ_REG(hw, E1000_ECOL);

	stats->mcc += E1000_READ_REG(hw, E1000_MCC);
	stats->latecol += E1000_READ_REG(hw, E1000_LATECOL);
	stats->colc += E1000_READ_REG(hw, E1000_COLC);
	stats->dc += E1000_READ_REG(hw, E1000_DC);
	stats->rlec += E1000_READ_REG(hw, E1000_RLEC);
	stats->xonrxc += E1000_READ_REG(hw, E1000_XONRXC);
	stats->xontxc += E1000_READ_REG(hw, E1000_XONTXC);

	/*
	 * For watchdog management we need to know if we have been
	 * paused during the last interval, so capture that here.
	 */ 
	sc->pause_frames = E1000_READ_REG(hw, E1000_XOFFRXC);
	stats->xoffrxc += sc->pause_frames;
	stats->xofftxc += E1000_READ_REG(hw, E1000_XOFFTXC);
	stats->fcruc += E1000_READ_REG(hw, E1000_FCRUC);
	stats->prc64 += E1000_READ_REG(hw, E1000_PRC64);
	stats->prc127 += E1000_READ_REG(hw, E1000_PRC127);
	stats->prc255 += E1000_READ_REG(hw, E1000_PRC255);
	stats->prc511 += E1000_READ_REG(hw, E1000_PRC511);
	stats->prc1023 += E1000_READ_REG(hw, E1000_PRC1023);
	stats->prc1522 += E1000_READ_REG(hw, E1000_PRC1522);
	stats->gprc += E1000_READ_REG(hw, E1000_GPRC);
	stats->bprc += E1000_READ_REG(hw, E1000_BPRC);
	stats->mprc += E1000_READ_REG(hw, E1000_MPRC);
	stats->gptc += E1000_READ_REG(hw, E1000_GPTC);

	/* For the 64-bit byte counters the low dword must be read first. */
	/* Both registers clear on the read of the high dword */

	stats->gorc += E1000_READ_REG(hw, E1000_GORCL) +
	    ((uint64_t)E1000_READ_REG(hw, E1000_GORCH) << 32);
	stats->gotc += E1000_READ_REG(hw, E1000_GOTCL) +
	    ((uint64_t)E1000_READ_REG(hw, E1000_GOTCH) << 32);

	stats->rnbc += E1000_READ_REG(hw, E1000_RNBC);
	stats->ruc += E1000_READ_REG(hw, E1000_RUC);
	stats->rfc += E1000_READ_REG(hw, E1000_RFC);
	stats->roc += E1000_READ_REG(hw, E1000_ROC);
	stats->rjc += E1000_READ_REG(hw, E1000_RJC);

	stats->tor += E1000_READ_REG(hw, E1000_TORH);
	stats->tot += E1000_READ_REG(hw, E1000_TOTH);

	stats->tpr += E1000_READ_REG(hw, E1000_TPR);
	stats->tpt += E1000_READ_REG(hw, E1000_TPT);
	stats->ptc64 += E1000_READ_REG(hw, E1000_PTC64);
	stats->ptc127 += E1000_READ_REG(hw, E1000_PTC127);
	stats->ptc255 += E1000_READ_REG(hw, E1000_PTC255);
	stats->ptc511 += E1000_READ_REG(hw, E1000_PTC511);
	stats->ptc1023 += E1000_READ_REG(hw, E1000_PTC1023);
	stats->ptc1522 += E1000_READ_REG(hw, E1000_PTC1522);
	stats->mptc += E1000_READ_REG(hw, E1000_MPTC);
	stats->bptc += E1000_READ_REG(hw, E1000_BPTC);

	/* Interrupt Counts */

	stats->iac += E1000_READ_REG(hw, E1000_IAC);
	stats->icrxptc += E1000_READ_REG(hw, E1000_ICRXPTC);
	stats->icrxatc += E1000_READ_REG(hw, E1000_ICRXATC);
	stats->ictxptc += E1000_READ_REG(hw, E1000_ICTXPTC);
	stats->ictxatc += E1000_READ_REG(hw, E1000_ICTXATC);
	stats->ictxqec += E1000_READ_REG(hw, E1000_ICTXQEC);
	stats->ictxqmtc += E1000_READ_REG(hw, E1000_ICTXQMTC);
	stats->icrxdmtc += E1000_READ_REG(hw, E1000_ICRXDMTC);
	stats->icrxoc += E1000_READ_REG(hw, E1000_ICRXOC);

	/* Host to Card Statistics */

	stats->cbtmpc += E1000_READ_REG(hw, E1000_CBTMPC);
	stats->htdpmc += E1000_READ_REG(hw, E1000_HTDPMC);
	stats->cbrdpc += E1000_READ_REG(hw, E1000_CBRDPC);
	stats->cbrmpc += E1000_READ_REG(hw, E1000_CBRMPC);
	stats->rpthc += E1000_READ_REG(hw, E1000_RPTHC);
	stats->hgptc += E1000_READ_REG(hw, E1000_HGPTC);
	stats->htcbdpc += E1000_READ_REG(hw, E1000_HTCBDPC);
	stats->hgorc += (E1000_READ_REG(hw, E1000_HGORCL) +
	    ((uint64_t)E1000_READ_REG(hw, E1000_HGORCH) << 32));
	stats->hgotc += (E1000_READ_REG(hw, E1000_HGOTCL) +
	    ((uint64_t)E1000_READ_REG(hw, E1000_HGOTCH) << 32));
	stats->lenerrs += E1000_READ_REG(hw, E1000_LENERRS);
	stats->scvpc += E1000_READ_REG(hw, E1000_SCVPC);
	stats->hrmpc += E1000_READ_REG(hw, E1000_HRMPC);

	stats->algnerrc += E1000_READ_REG(hw, E1000_ALGNERRC);
	stats->rxerrc += E1000_READ_REG(hw, E1000_RXERRC);
	stats->tncrs += E1000_READ_REG(hw, E1000_TNCRS);
	stats->cexterr += E1000_READ_REG(hw, E1000_CEXTERR);
	stats->tsctc += E1000_READ_REG(hw, E1000_TSCTC);
	stats->tsctfc += E1000_READ_REG(hw, E1000_TSCTFC);

	ifp->if_collisions = stats->colc;

	/* Rx Errors */
	ifp->if_ierrors = stats->rxerrc + stats->crcerrs + stats->algnerrc +
	    stats->ruc + stats->roc + stats->mpc + stats->cexterr;

	/* Tx Errors */
	ifp->if_oerrors = stats->ecol + stats->latecol + sc->watchdog_events;

	/* Driver specific counters */
	sc->device_control = E1000_READ_REG(hw, E1000_CTRL);
	sc->rx_control = E1000_READ_REG(hw, E1000_RCTL);
	sc->int_mask = E1000_READ_REG(hw, E1000_IMS);
	sc->eint_mask = E1000_READ_REG(hw, E1000_EIMS);
	sc->packet_buf_alloc_tx =
	    ((E1000_READ_REG(hw, E1000_PBA) & 0xffff0000) >> 16);
	sc->packet_buf_alloc_rx =
	    (E1000_READ_REG(hw, E1000_PBA) & 0xffff);
}

static void
igb_vf_init_stats(struct igb_softc *sc)
{
	struct e1000_hw *hw = &sc->hw;
	struct e1000_vf_stats *stats;

	stats = sc->stats;
	stats->last_gprc = E1000_READ_REG(hw, E1000_VFGPRC);
	stats->last_gorc = E1000_READ_REG(hw, E1000_VFGORC);
	stats->last_gptc = E1000_READ_REG(hw, E1000_VFGPTC);
	stats->last_gotc = E1000_READ_REG(hw, E1000_VFGOTC);
	stats->last_mprc = E1000_READ_REG(hw, E1000_VFMPRC);
}
 
static void
igb_update_vf_stats_counters(struct igb_softc *sc)
{
	struct e1000_hw *hw = &sc->hw;
	struct e1000_vf_stats *stats;

	if (sc->link_speed == 0)
		return;

	stats = sc->stats;
	UPDATE_VF_REG(E1000_VFGPRC, stats->last_gprc, stats->gprc);
	UPDATE_VF_REG(E1000_VFGORC, stats->last_gorc, stats->gorc);
	UPDATE_VF_REG(E1000_VFGPTC, stats->last_gptc, stats->gptc);
	UPDATE_VF_REG(E1000_VFGOTC, stats->last_gotc, stats->gotc);
	UPDATE_VF_REG(E1000_VFMPRC, stats->last_mprc, stats->mprc);
}

#ifdef DEVICE_POLLING

static void
igb_poll(struct ifnet *ifp, enum poll_cmd cmd, int count)
{
	struct igb_softc *sc = ifp->if_softc;
	uint32_t reg_icr;

	ASSERT_SERIALIZED(ifp->if_serializer);

	switch (cmd) {
	case POLL_REGISTER:
		igb_disable_intr(sc);
		break;

	case POLL_DEREGISTER:
		igb_enable_intr(sc);
		break;

	case POLL_AND_CHECK_STATUS:
		reg_icr = E1000_READ_REG(&sc->hw, E1000_ICR);
		if (reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
			sc->hw.mac.get_link_status = 1;
			igb_update_link_status(sc);
		}
		/* FALL THROUGH */
	case POLL_ONLY:
		if (ifp->if_flags & IFF_RUNNING) {
			igb_rxeof(sc->queues[0].rxr, count);

			igb_txeof(sc->queues[0].txr);
			if (!ifq_is_empty(&ifp->if_snd))
				if_devstart(ifp);
		}
		break;
	}
}

#endif /* DEVICE_POLLING */

static void
igb_intr(void *xsc)
{
	struct igb_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint32_t reg_icr;

	ASSERT_IFNET_SERIALIZED_ALL(ifp);

	reg_icr = E1000_READ_REG(&sc->hw, E1000_ICR);

	/* Hot eject?  */
	if (reg_icr == 0xffffffff)
		return;

	/* Definitely not our interrupt.  */
	if (reg_icr == 0x0)
		return;

	if ((reg_icr & E1000_ICR_INT_ASSERTED) == 0)
		return;

	if (ifp->if_flags & IFF_RUNNING) {
		igb_rxeof(sc->queues[0].rxr, -1);

		igb_txeof(sc->queues[0].txr);
		if (!ifq_is_empty(&ifp->if_snd))
			if_devstart(ifp);
	}

	/* Link status change */
	if (reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
		sc->hw.mac.get_link_status = 1;
		igb_update_link_status(sc);
	}

	if (reg_icr & E1000_ICR_RXO)
		sc->rx_overruns++;
}

static int
igb_txctx_pullup(struct igb_tx_ring *txr, struct mbuf **m0)
{
	struct mbuf *m = *m0;
	struct ether_header *eh;
	int len;

	txr->ctx_try_pullup++;

	len = ETHER_HDR_LEN + IGB_IPVHL_SIZE;

	if (__predict_false(!M_WRITABLE(m))) {
		if (__predict_false(m->m_len < ETHER_HDR_LEN)) {
			txr->ctx_drop1++;
			m_freem(m);
			*m0 = NULL;
			return ENOBUFS;
		}
		eh = mtod(m, struct ether_header *);

		if (eh->ether_type == htons(ETHERTYPE_VLAN))
			len += EVL_ENCAPLEN;

		if (m->m_len < len) {
			txr->ctx_drop2++;
			m_freem(m);
			*m0 = NULL;
			return ENOBUFS;
		}
		return 0;
	}

	if (__predict_false(m->m_len < ETHER_HDR_LEN)) {
		txr->ctx_pullup1++;
		m = m_pullup(m, ETHER_HDR_LEN);
		if (m == NULL) {
			txr->ctx_pullup1_failed++;
			*m0 = NULL;
			return ENOBUFS;
		}
		*m0 = m;
	}
	eh = mtod(m, struct ether_header *);

	if (eh->ether_type == htons(ETHERTYPE_VLAN))
		len += EVL_ENCAPLEN;

	if (m->m_len < len) {
		txr->ctx_pullup2++;
		m = m_pullup(m, len);
		if (m == NULL) {
			txr->ctx_pullup2_failed++;
			*m0 = NULL;
			return ENOBUFS;
		}
		*m0 = m;
	}
	return 0;
}

static int
igb_encap(struct igb_tx_ring *txr, struct mbuf **m_headp)
{
	bus_dma_segment_t segs[IGB_MAX_SCATTER];
	bus_dmamap_t map;
	struct igb_tx_buf *tx_buf, *tx_buf_mapped;
	union e1000_adv_tx_desc	*txd = NULL;
	struct mbuf *m_head = *m_headp;
	uint32_t olinfo_status = 0, cmd_type_len = 0;
	int maxsegs, nsegs, i, j, error, first, last = 0;
	uint32_t hdrlen = 0;

	if (m_head->m_len < IGB_TXCSUM_MINHL &&
	    ((m_head->m_pkthdr.csum_flags & IGB_CSUM_FEATURES) ||
	     (m_head->m_flags & M_VLANTAG))) {
		/*
		 * Make sure that ethernet header and ip.ip_hl are in
		 * contiguous memory, since if TXCSUM or VLANTAG is
		 * enabled, later TX context descriptor's setup need
		 * to access ip.ip_hl.
		 */
		error = igb_txctx_pullup(txr, m_headp);
		if (error) {
			KKASSERT(*m_headp == NULL);
			return error;
		}
		m_head = *m_headp;
	}

	/* Set basic descriptor constants */
	cmd_type_len |= E1000_ADVTXD_DTYP_DATA;
	cmd_type_len |= E1000_ADVTXD_DCMD_IFCS | E1000_ADVTXD_DCMD_DEXT;
	if (m_head->m_flags & M_VLANTAG)
		cmd_type_len |= E1000_ADVTXD_DCMD_VLE;

	/*
	 * Map the packet for DMA.
	 *
	 * Capture the first descriptor index,
	 * this descriptor will have the index
	 * of the EOP which is the only one that
	 * now gets a DONE bit writeback.
	 */
	first = txr->next_avail_desc;
	tx_buf = &txr->tx_buf[first];
	tx_buf_mapped = tx_buf;
	map = tx_buf->map;

	KASSERT(txr->tx_avail > 2, ("invalid avail TX desc\n"));
	maxsegs = txr->tx_avail - 2;
	KASSERT(maxsegs >= IGB_MAX_SCATTER - 2, ("not enough spare TX desc\n"));
	if (maxsegs > IGB_MAX_SCATTER)
		maxsegs = IGB_MAX_SCATTER;

	error = bus_dmamap_load_mbuf_defrag(txr->tx_tag, map, m_headp,
	    segs, maxsegs, &nsegs, BUS_DMA_NOWAIT);
	if (error) {
		if (error == ENOBUFS)
			txr->sc->mbuf_defrag_failed++;
		else
			txr->sc->no_tx_dma_setup++;

		m_freem(*m_headp);
		*m_headp = NULL;
		return error;
	}
	bus_dmamap_sync(txr->tx_tag, map, BUS_DMASYNC_PREWRITE);

	m_head = *m_headp;

#if 0
	/*
	 * Set up the context descriptor:
	 * used when any hardware offload is done.
	 * This includes CSUM, VLAN, and TSO. It
	 * will use the first descriptor.
	 */
	if (m_head->m_pkthdr.csum_flags & CSUM_TSO) {
		if (igb_tso_setup(txr, m_head, &hdrlen)) {
			cmd_type_len |= E1000_ADVTXD_DCMD_TSE;
			olinfo_status |= E1000_TXD_POPTS_IXSM << 8;
			olinfo_status |= E1000_TXD_POPTS_TXSM << 8;
		} else
			return (ENXIO); 
	} else if (igb_tx_ctx_setup(txr, m_head))
		olinfo_status |= E1000_TXD_POPTS_TXSM << 8;
#else
	if (igb_txctx(txr, m_head)) {
		olinfo_status |= (E1000_TXD_POPTS_IXSM << 8);
		if (m_head->m_pkthdr.csum_flags & (CSUM_UDP | CSUM_TCP))
			olinfo_status |= (E1000_TXD_POPTS_TXSM << 8);
	}
#endif

	/* Calculate payload length */
	olinfo_status |= ((m_head->m_pkthdr.len - hdrlen)
	    << E1000_ADVTXD_PAYLEN_SHIFT);

	/* 82575 needs the queue index added */
	if (txr->sc->hw.mac.type == e1000_82575)
		olinfo_status |= txr->me << 4;

	/* Set up our transmit descriptors */
	i = txr->next_avail_desc;
	for (j = 0; j < nsegs; j++) {
		bus_size_t seg_len;
		bus_addr_t seg_addr;

		tx_buf = &txr->tx_buf[i];
		txd = (union e1000_adv_tx_desc *)&txr->tx_base[i];
		seg_addr = segs[j].ds_addr;
		seg_len = segs[j].ds_len;

		txd->read.buffer_addr = htole64(seg_addr);
		txd->read.cmd_type_len = htole32(cmd_type_len | seg_len);
		txd->read.olinfo_status = htole32(olinfo_status);
		last = i;
		if (++i == txr->sc->num_tx_desc)
			i = 0;
		tx_buf->m_head = NULL;
		tx_buf->next_eop = -1;
	}

	KASSERT(txr->tx_avail > nsegs, ("invalid avail TX desc\n"));
	txr->next_avail_desc = i;
	txr->tx_avail -= nsegs;

	tx_buf->m_head = m_head;
	tx_buf_mapped->map = tx_buf->map;
	tx_buf->map = map;

	/*
	 * Last Descriptor of Packet
	 * needs End Of Packet (EOP)
	 * and Report Status (RS)
	 */
	txd->read.cmd_type_len |=
	    htole32(E1000_ADVTXD_DCMD_EOP | E1000_ADVTXD_DCMD_RS);
	/*
	 * Keep track in the first buffer which
	 * descriptor will be written back
	 */
	tx_buf = &txr->tx_buf[first];
	tx_buf->next_eop = last;

	/*
	 * Advance the Transmit Descriptor Tail (TDT), this tells the E1000
	 * that this frame is available to transmit.
	 */
	E1000_WRITE_REG(&txr->sc->hw, E1000_TDT(txr->me), i);
	++txr->tx_packets;

	return 0;
}

static void
igb_start(struct ifnet *ifp)
{
	struct igb_softc *sc = ifp->if_softc;
	struct igb_tx_ring *txr = sc->queues[0].txr;
	struct mbuf *m_head;

	ASSERT_IFNET_SERIALIZED_ALL(ifp);

	if ((ifp->if_flags & (IFF_RUNNING|IFF_OACTIVE)) != IFF_RUNNING)
		return;

	if (!sc->link_active) {
		ifq_purge(&ifp->if_snd);
		return;
	}

	/* Call cleanup if number of TX descriptors low */
	if (txr->tx_avail <= IGB_TX_CLEANUP_THRESHOLD(sc))
		igb_txeof(txr);

	while (!ifq_is_empty(&ifp->if_snd)) {
		if (txr->tx_avail < IGB_MAX_SCATTER) {
			ifp->if_flags |= IFF_OACTIVE;
			/* Set watchdog on */
			ifp->if_timer = 5;
			break;
		}

		m_head = ifq_dequeue(&ifp->if_snd, NULL);
		if (m_head == NULL)
			break;

		if (igb_encap(txr, &m_head)) {
			ifp->if_oerrors++;
			continue;
		}

		/* Send a copy of the frame to the BPF listener */
		ETHER_BPF_MTAP(ifp, m_head);
	}
}

static void
igb_watchdog(struct ifnet *ifp)
{
	struct igb_softc *sc = ifp->if_softc;
	struct igb_tx_ring *txr = sc->queues[0].txr;

	ASSERT_IFNET_SERIALIZED_ALL(ifp);

	/* 
	 * If flow control has paused us since last checking
	 * it invalidates the watchdog timing, so dont run it.
	 */
	if (sc->pause_frames) {
		sc->pause_frames = 0;
		ifp->if_timer = 5;
		return;
	}

	if_printf(ifp, "Watchdog timeout -- resetting\n");
	if_printf(ifp, "Queue(%d) tdh = %d, hw tdt = %d\n", txr->me,
	    E1000_READ_REG(&sc->hw, E1000_TDH(txr->me)),
	    E1000_READ_REG(&sc->hw, E1000_TDT(txr->me)));
	if_printf(ifp, "TX(%d) desc avail = %d, "
	    "Next TX to Clean = %d\n",
	    txr->me, txr->tx_avail, txr->next_to_clean);

	ifp->if_oerrors++;
	sc->watchdog_events++;

	igb_init(sc);
	if (!ifq_is_empty(&ifp->if_snd))
		if_devstart(ifp);
}

static void
igb_set_itr(struct igb_softc *sc)
{
	uint32_t itr = 0;

	if (sc->intr_rate > 0) {
		if (sc->hw.mac.type == e1000_82575) {
			itr = 1000000000 / 256 / sc->intr_rate;
			/*
			 * NOTE:
			 * Document is wrong on the 2 bits left shift
			 */
		} else {
			itr = 1000000 / sc->intr_rate;
			itr <<= 2;
		}
		itr &= 0x7FFC;
	}
	if (sc->hw.mac.type == e1000_82575)
		itr |= itr << 16;
	else
		itr |= E1000_EITR_CNT_IGNR;
	E1000_WRITE_REG(&sc->hw, E1000_EITR(0), itr);
}

static int
igb_sysctl_intr_rate(SYSCTL_HANDLER_ARGS)
{
	struct igb_softc *sc = (void *)arg1;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int error, intr_rate;

	intr_rate = sc->intr_rate;
	error = sysctl_handle_int(oidp, &intr_rate, 0, req);
	if (error || req->newptr == NULL)
		return error;
	if (intr_rate < 0)
		return EINVAL;

	ifnet_serialize_all(ifp);

	sc->intr_rate = intr_rate;
	if (ifp->if_flags & IFF_RUNNING)
		igb_set_itr(sc);

	ifnet_deserialize_all(ifp);

	if (bootverbose)
		if_printf(ifp, "Interrupt rate set to %d/sec\n", sc->intr_rate);
	return 0;
}
