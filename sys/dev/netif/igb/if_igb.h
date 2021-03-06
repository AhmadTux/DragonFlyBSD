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

#ifndef _IF_IGB_H_
#define _IF_IGB_H_

/* Tunables */

/*
 * IGB_TXD: Maximum number of Transmit Descriptors
 *
 *   This value is the number of transmit descriptors allocated by the driver.
 *   Increasing this value allows the driver to queue more transmits. Each
 *   descriptor is 16 bytes.
 *   Since TDLEN should be multiple of 128bytes, the number of transmit
 *   desscriptors should meet the following condition.
 *      (num_tx_desc * sizeof(struct e1000_tx_desc)) % 128 == 0
 */
#define IGB_MIN_TXD		256
#define IGB_DEFAULT_TXD		1024
#define IGB_MAX_TXD		4096

/*
 * IGB_RXD: Maximum number of Transmit Descriptors
 *
 *   This value is the number of receive descriptors allocated by the driver.
 *   Increasing this value allows the driver to buffer more incoming packets.
 *   Each descriptor is 16 bytes.  A receive buffer is also allocated for each
 *   descriptor. The maximum MTU size is 16110.
 *   Since TDLEN should be multiple of 128bytes, the number of transmit
 *   desscriptors should meet the following condition.
 *      (num_tx_desc * sizeof(struct e1000_tx_desc)) % 128 == 0
 */
#define IGB_MIN_RXD		256
#define IGB_DEFAULT_RXD		1024
#define IGB_MAX_RXD		4096

/*
 * This parameter controls when the driver calls the routine to reclaim
 * transmit descriptors. Cleaning earlier seems a win.
 */
#define IGB_TX_CLEANUP_THRESHOLD(sc)	((sc)->num_tx_desc / 2)

/*
 * This parameter controls whether or not autonegotation is enabled.
 *              0 - Disable autonegotiation
 *              1 - Enable  autonegotiation
 */
#define DO_AUTO_NEG		1

/*
 * This parameter control whether or not the driver will wait for
 * autonegotiation to complete.
 *              1 - Wait for autonegotiation to complete
 *              0 - Don't wait for autonegotiation to complete
 */
#define WAIT_FOR_AUTO_NEG_DEFAULT	0

/* Tunables -- End */

#define AUTONEG_ADV_DEFAULT	(ADVERTISE_10_HALF | ADVERTISE_10_FULL | \
				 ADVERTISE_100_HALF | ADVERTISE_100_FULL | \
				 ADVERTISE_1000_FULL)

#define AUTO_ALL_MODES			0

/* PHY master/slave setting */
#define IGB_MASTER_SLAVE		e1000_ms_hw_default

/*
 * Micellaneous constants
 */
#define IGB_VENDOR_ID			0x8086

#define IGB_JUMBO_PBA			0x00000028
#define IGB_DEFAULT_PBA			0x00000030
#define IGB_SMARTSPEED_DOWNSHIFT	3
#define IGB_SMARTSPEED_MAX		15
#define IGB_MAX_LOOP			10

#define IGB_RX_PTHRESH			(hw->mac.type <= e1000_82576 ? 16 : 8)
#define IGB_RX_HTHRESH			8
#define IGB_RX_WTHRESH			1

#define IGB_TX_PTHRESH			8
#define IGB_TX_HTHRESH			1
#define IGB_TX_WTHRESH			((hw->mac.type != e1000_82575 && \
                                          sc->msix_mem) ? 1 : 16)

#define MAX_NUM_MULTICAST_ADDRESSES	128
#define IGB_FC_PAUSE_TIME		0x0680

#define IGB_INTR_RATE			10000

/*
 * TDBA/RDBA should be aligned on 16 byte boundary. But TDLEN/RDLEN should be
 * multiple of 128 bytes. So we align TDBA/RDBA on 128 byte boundary. This will
 * also optimize cache line size effect. H/W supports up to cache line size 128.
 */
#define IGB_DBA_ALIGN			128

/* PCI Config defines */
#define IGB_MSIX_BAR			3

#define IGB_MAX_SCATTER			64
#define IGB_VFTA_SIZE			128
#define IGB_TSO_SIZE			(65535 + \
					 sizeof(struct ether_vlan_header))
#define IGB_TSO_SEG_SIZE		4096	/* Max dma segment size */
#define IGB_HDR_BUF			128
#define IGB_PKTTYPE_MASK		0x0000FFF0

#define IGB_CSUM_FEATURES		(CSUM_IP | CSUM_TCP | CSUM_UDP)
#define IGB_IPVHL_SIZE			1 /* sizeof(ip.ip_vhl) */
#define IGB_TXCSUM_MINHL		(ETHER_HDR_LEN + EVL_ENCAPLEN + \
					 IGB_IPVHL_SIZE)

struct igb_softc;

/*
 * Bus dma information structure
 */
struct igb_dma {
	bus_addr_t		dma_paddr;
	void			*dma_vaddr;
	bus_dma_tag_t		dma_tag;
	bus_dmamap_t		dma_map;
};

/*
 * Driver queue struct: this is the interrupt container
 * for the associated tx and rx ring.
 */
struct igb_queue {
	struct igb_softc	*sc;
	uint32_t		msix;		/* This queue's MSIX vector */
	uint32_t		eims;		/* This queue's EIMS bit */
	uint32_t		eitr_setting;
	struct resource		*res;
	void			*tag;
	struct igb_tx_ring	*txr;
	struct igb_rx_ring	*rxr;
	uint64_t		irqs;
};

/*
 * Transmit ring: one per queue
 */
struct igb_tx_ring {
	struct igb_softc	*sc;
	uint32_t		me;
	struct igb_dma		txdma;
	struct e1000_tx_desc	*tx_base;
	uint32_t		next_avail_desc;
	uint32_t		next_to_clean;
	uint16_t		tx_avail;
	struct igb_tx_buf	*tx_buf;
	bus_dma_tag_t		tx_tag;

	u_long			no_desc_avail;
	u_long			tx_packets;

	u_long			ctx_try_pullup;
	u_long			ctx_drop1;
	u_long			ctx_drop2;
	u_long			ctx_pullup1;
	u_long			ctx_pullup1_failed;
	u_long			ctx_pullup2;
	u_long			ctx_pullup2_failed;
};

/*
 * Receive ring: one per queue
 */
struct igb_rx_ring {
	struct igb_softc	*sc;
	uint32_t		me;
	struct igb_dma		rxdma;
	union e1000_adv_rx_desc	*rx_base;
	boolean_t		discard;
	uint32_t		next_to_check;
	struct igb_rx_buf	*rx_buf;
	bus_dma_tag_t		rx_tag;
	bus_dmamap_t		rx_sparemap;

	/*
	 * First/last mbuf pointers, for
	 * collecting multisegment RX packets.
	 */
	struct mbuf		*fmp;
	struct mbuf		*lmp;

	/* Soft stats */
	u_long			rx_packets;
};

struct igb_softc {
	struct arpcom		arpcom;
	struct e1000_hw		hw;

	struct e1000_osdep	osdep;
	device_t		dev;

	bus_dma_tag_t		parent_tag;

	int			mem_rid;
	struct resource 	*mem_res;

	struct resource 	*msix_mem;
	void			*tag;
	uint32_t		que_mask;

	int			linkvec;
	int			link_mask;
	int			link_irq;

	struct ifmedia		media;
	struct callout		timer;

#if 0
	int			msix;	/* total vectors allocated */
#endif
	int			intr_type;
	int			intr_rid;
	struct resource		*intr_res;
	void			*intr_tag;

	int			if_flags;
	int			max_frame_size;
	int			min_frame_size;
	int			pause_frames;
	uint16_t		num_queues;
	uint16_t		vf_ifp;	/* a VF interface */

	/* Management and WOL features */
	int			wol;
	int			has_manage;

	/* Info about the interface */
	uint8_t			link_active;
	uint16_t		link_speed;
	uint16_t		link_duplex;
	uint32_t		smartspeed;
	uint32_t		dma_coalesce;

	int			intr_rate;

	/* Interface queues */
	struct igb_queue	*queues;

	/*
	 * Transmit rings
	 */
	struct igb_tx_ring	*tx_rings;
	int			num_tx_desc;

	/* Multicast array pointer */
	uint8_t			*mta;

	/* 
	 * Receive rings
	 */
	struct igb_rx_ring	*rx_rings;
	int			num_rx_desc;
	uint32_t		rx_mbuf_sz;
	uint32_t		rx_mask;

	/* Misc stats maintained by the driver */
	u_long			dropped_pkts;
	u_long			mbuf_defrag_failed;
	u_long			no_tx_dma_setup;
	u_long			watchdog_events;
	u_long			rx_overruns;
	u_long			device_control;
	u_long			rx_control;
	u_long			int_mask;
	u_long			eint_mask;
	u_long			packet_buf_alloc_rx;
	u_long			packet_buf_alloc_tx;

	/* sysctl tree glue */
	struct sysctl_ctx_list	sysctl_ctx;
	struct sysctl_oid	*sysctl_tree;

	void 			*stats;
};

struct igb_tx_buf {
	int		next_eop;	/* Index of the desc to watch */
	struct mbuf	*m_head;
	bus_dmamap_t	map;		/* bus_dma map for packet */
};

struct igb_rx_buf {
	struct mbuf	*m_head;
	bus_dmamap_t	map;	/* bus_dma map for packet */
	bus_addr_t	paddr;
};

#define UPDATE_VF_REG(reg, last, cur)		\
{						\
	uint32_t new = E1000_READ_REG(hw, reg);	\
	if (new < last)				\
		cur += 0x100000000LL;		\
	last = new;				\
	cur &= 0xFFFFFFFF00000000LL;		\
	cur |= new;				\
}

#endif /* _IF_IGB_H_ */
