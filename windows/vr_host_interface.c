/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */
#include <ndis.h>
#include <netiodef.h>

#include "vr_interface.h"
#include "vr_packet.h"
#include "vr_windows.h"
#include "vr_mpls.h"
#include "vrouter.h"

#include "win_csum.h"
#include "win_packet.h"
#include "win_packet_raw.h"
#include "win_packet_impl.h"
#include "win_packet_splitting.h"
#include "windows_devices.h"
#include "windows_nbl.h"

static BOOLEAN physicalVifAdded;

static NDIS_MUTEX win_if_mutex;

void
win_if_lock(void)
{
    NDIS_WAIT_FOR_MUTEX(&win_if_mutex);
}

void
win_if_unlock(void)
{
    NDIS_RELEASE_MUTEX(&win_if_mutex);
}

BOOLEAN IsPacketPassthroughEnabled(void) {
    return !physicalVifAdded;
}

static int
win_if_add(struct vr_interface* vif)
{
    if (vif->vif_type == VIF_TYPE_STATS)
        return 0;

    if (vif->vif_name[0] == '\0')
        return -ENODEV;

    if (vif->vif_type == VIF_TYPE_PHYSICAL)
        physicalVifAdded = true;

    // Unlike FreeBSD/Linux, we don't have to register handlers here
    return 0;
}

static int
win_if_add_tap(struct vr_interface* vif)
{
    UNREFERENCED_PARAMETER(vif);
    // NOOP - no bridges on Windows
    return 0;
}

static int
win_if_del(struct vr_interface *vif)
{
    if (vif->vif_type == VIF_TYPE_PHYSICAL)
        physicalVifAdded = false;

    return 0;
}

static int
win_if_del_tap(struct vr_interface *vif)
{
    UNREFERENCED_PARAMETER(vif);
    // NOOP - no bridges on Windows; most *_drv_del function which call if_del_tap
    // also call if_del
    return 0;
}

// TODO: To be removed after WinTxPostprocess() went live.
static void
fix_ip_csum_at_offset(struct vr_packet *pkt, unsigned offset)
{
    struct vr_ip *iph;

    ASSERT(0 < offset);

    iph = (struct vr_ip *)(pkt_data(pkt) + offset);
    iph->ip_csum = vr_ip_csum(iph);
}

// TODO: To be removed after WinTxPostprocess() went live.
static void
zero_ip_csum_at_offset(struct vr_packet *pkt, unsigned offset)
{
    struct vr_ip *iph;

    ASSERT(0 < offset);

    iph = (struct vr_ip *)(pkt_data(pkt) + offset);
    iph->ip_csum = 0;
}

// TODO: To be removed after WinTxPostprocess() went live.
static bool fix_csum(struct vr_packet *pkt, unsigned offset)
{
    uint32_t csum;
    uint16_t size;
    uint8_t type;

    PWIN_PACKET winPacket = GetWinPacketFromVrPacket(pkt);
    PWIN_PACKET_RAW winPacketRaw = WinPacketToRawPacket(winPacket);
    PNET_BUFFER_LIST nbl = WinPacketRawToNBL(winPacketRaw);
    PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(nbl);

    void* packet_data_buffer = ExAllocatePoolWithTag(NonPagedPoolNx, NET_BUFFER_DATA_LENGTH(nb), VrAllocationTag);

    // Copy the packet. This function will not fail if ExAllocatePoolWithTag succeeded
    // So no need to clean it up
    // If ExAllocatePoolWithTag failed (packet_data_buffer== NULL),
    // this function will work okay if the data is contigous.
    uint8_t* packet_data = NdisGetDataBuffer(nb, NET_BUFFER_DATA_LENGTH(nb), packet_data_buffer, 1, 0);

    if (packet_data == NULL)
        // No need for free
        return false;

    if (pkt->vp_type == VP_TYPE_IP6 || pkt->vp_type == VP_TYPE_IP6OIP) {
        struct vr_ip6 *hdr = (struct vr_ip6*) (packet_data + offset);
        offset += sizeof(struct vr_ip6);
        size = ntohs(hdr->PayloadLength);

        type = hdr->NextHeader;
    } else {
        struct vr_ip *hdr = (struct vr_ip*) &packet_data[offset];
        offset += hdr->ip_hl * 4;
        size = ntohs(hdr->ip_len) - 4 * hdr->ip_hl;

        type = hdr->ip_proto;
    }

    uint8_t* payload = &packet_data[offset];
    csum = calc_csum((uint8_t*) payload, size);

    // This time it's the "real" packet. Header being contiguous is guaranteed, but nothing else
    if (type == VR_IP_PROTO_UDP) {
        struct vr_udp* udp = (struct vr_udp*) pkt_data_at_offset(pkt, offset);
        udp->udp_csum = htons(~(trim_csum(csum)));
    } else if (type == VR_IP_PROTO_TCP) {
        struct vr_tcp* tcp = (struct vr_tcp*) pkt_data_at_offset(pkt, offset);
        tcp->tcp_csum = htons(~(trim_csum(csum)));
    }

    if (packet_data_buffer)
        ExFreePool(packet_data_buffer);

    return true;
}

// TODO: To be removed after WinTxPostprocess() went live.
static void
fix_tunneled_csum(struct vr_packet *pkt)
{
    PWIN_PACKET winPacket = GetWinPacketFromVrPacket(pkt);
    PWIN_PACKET_RAW winPacketRaw = WinPacketToRawPacket(winPacket);
    PNET_BUFFER_LIST nbl = WinPacketRawToNBL(winPacketRaw);
    NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO settings;
    settings.Value = NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo);

    if (settings.Transmit.IpHeaderChecksum) {
        // Zero the outer checksum, it'll be offloaded
        zero_ip_csum_at_offset(pkt, sizeof(struct vr_eth));
        // Fix the inner checksum, it will not be offloaded
        fix_ip_csum_at_offset(pkt, pkt->vp_inner_network_h);
    } else {
        // Fix the outer checksum
        fix_ip_csum_at_offset(pkt, sizeof(struct vr_eth));
        // Inner checksum is OK
    }

    if (settings.Transmit.TcpChecksum) {
        // Calculate the header/data csum and turn off HW acceleration
        if (fix_csum(pkt, pkt->vp_inner_network_h)) {
            settings.Transmit.TcpChecksum = 0;
            settings.Transmit.TcpHeaderOffset = 0;
            NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo) = settings.Value;
        }
        // else try to offload it even though it's tunneled.
    }

    if (settings.Transmit.UdpChecksum) {
        // Calculate the header/data csum and turn off HW acceleration
        if (fix_csum(pkt, pkt->vp_inner_network_h)) {
            settings.Transmit.UdpChecksum = 0;
            NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo) = settings.Value;
        }
        // else try to offload it even though it's tunneled.
    }
}

// Fixes the IPv4 checksum and sets the correct info.Transmit value.
//
// TODO try to offload the checksums when doing fragmentation too.
// TODO: To be removed after WinTxPostprocess() went live.
static void
fix_ip_v4_csum(struct vr_packet *pkt) {
    PWIN_PACKET winPacket = GetWinPacketFromVrPacket(pkt);
    PWIN_PACKET_RAW winPacketRaw = WinPacketToRawPacket(winPacket);
    PNET_BUFFER_LIST nbl = WinPacketRawToNBL(winPacketRaw);
    NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO settings;
    settings.Value = NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo);

    if (pkt->vp_data != 0) {
        // This packet came to us in a tunnel (which is now unwrapped),
        // therefore .Receive is now the active field of the settings union
        // (although we don't look at it as it refers to the outer headers anyway).
        // We assume all the checksums are valid, so we need to explicitly disable
        // offloading (so the .Receive field isn't erroneously reinterpreted as .Transmit)
        NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo) = 0;
    } else {
        // This packet comes from a container or the agent,
        // therefore we should look at settings.Transmit.
        if (settings.Transmit.IpHeaderChecksum) {
            // If computation of IP checksum is about to be offloaded, its value
            // should be set to zero (because initial checksum's value is taken into
            // account when computing the checksum). However, dp-core doesn't care about
            // this specific case (e.g. vr_incremental_diff/vr_ip_incremental_csum are
            // called to incrementally "improve" checksum).
            zero_ip_csum_at_offset(pkt, sizeof(struct vr_eth));
        } else {
            // No offloading requested - checksum should be valid.
        }
    }
}

static NDIS_SWITCH_PORT_DESTINATION
VrInterfaceToDestination(struct vr_interface *vif)
{
    NDIS_SWITCH_PORT_DESTINATION destination = { 0 };

    destination.PortId = vif->vif_port;
    destination.NicIndex = vif->vif_nic;

    return destination;
}

static VOID
MarkNetBufferListAsSafe(PNET_BUFFER_LIST NetBufferList)
{
    PNDIS_SWITCH_FORWARDING_DETAIL_NET_BUFFER_LIST_INFO fwd;

    fwd = NET_BUFFER_LIST_SWITCH_FORWARDING_DETAIL(NetBufferList);
    fwd->IsPacketDataSafe = TRUE;
}

static int
__win_if_tx(struct vr_interface *vif, struct vr_packet *pkt)
{
    if (vr_pkt_type_is_overlay(pkt->vp_type)) {
        fix_tunneled_csum(pkt);
    } else if (pkt->vp_type == VP_TYPE_IP) {
        // There's no checksum in IPv6 header.
        fix_ip_v4_csum(pkt);
    }

    PWIN_PACKET winPacket = GetWinPacketFromVrPacket(pkt);
    PWIN_PACKET_RAW winPacketRaw = WinPacketToRawPacket(winPacket);

    PWIN_MULTI_PACKET fragmentedWinPacket = split_packet_if_needed(pkt);
    if (fragmentedWinPacket != NULL) {
        winPacketRaw = WinMultiPacketToRawPacket(fragmentedWinPacket);
    }

    PNET_BUFFER_LIST nbl = WinPacketRawToNBL(winPacketRaw);

    NDIS_SWITCH_PORT_DESTINATION newDestination = VrInterfaceToDestination(vif);
    VrSwitchObject->NdisSwitchHandlers.AddNetBufferListDestination(VrSwitchObject->NdisSwitchContext, nbl, &newDestination);

    MarkNetBufferListAsSafe(nbl);

    NdisAdvanceNetBufferListDataStart(nbl, pkt->vp_data, TRUE, NULL);

    ExFreePool(pkt);

    ASSERTMSG("Trying to pass non-leaf NBL to NdisFSendNetBufferLists", nbl->ChildRefCount == 0);

    NdisFSendNetBufferLists(VrSwitchObject->NdisFilterHandle,
        nbl,
        NDIS_DEFAULT_PORT_NUMBER,
        0);

    return 0;
}

static int
win_if_tx(struct vr_interface *vif, struct vr_packet* pkt)
{
    if (vif == NULL) {
        win_free_packet(pkt);
        return 0; // Sent into /dev/null
    }

    if (vif->vif_type == VIF_TYPE_AGENT)
        return pkt0_if_tx(vif, pkt);
    else
        return __win_if_tx(vif, pkt);
}

static int
win_if_rx(struct vr_interface *vif, struct vr_packet* pkt)
{
    // Since we are operating from virtual switch's PoV and not from OS's PoV, RXing is the same as TXing
    // On Linux, we receive the packet as an OS, but in Windows we are a switch to we simply push the packet to OS's networking stack
    // See vhost_tx for reference (it calls hif_ops->hif_rx)

    win_if_tx(vif, pkt);

    return 0;
}

static int
win_if_get_settings(struct vr_interface *vif, struct vr_interface_settings *settings)
{
    UNREFERENCED_PARAMETER(vif);
    UNREFERENCED_PARAMETER(settings);

    /* TODO: Implement */
    DbgPrint("%s(): dummy implementation called\n", __func__);

    return -EINVAL;
}

static unsigned int
win_if_get_mtu(struct vr_interface *vif)
{
    // vif_mtu is set correctly in win_register_nic
    return vif->vif_mtu;
}

static unsigned short
win_if_get_encap(struct vr_interface *vif)
{
    UNREFERENCED_PARAMETER(vif);

    /* TODO: Implement */
    DbgPrint("%s(): dummy implementation called\n", __func__);

    return VIF_ENCAP_TYPE_ETHER;
}

static struct vr_host_interface_ops win_host_interface_ops = {
    .hif_lock           = win_if_lock,
    .hif_unlock         = win_if_unlock,
    .hif_add            = win_if_add,
    .hif_del            = win_if_del,
    .hif_add_tap        = win_if_add_tap,
    .hif_del_tap        = win_if_del_tap,
    .hif_tx             = win_if_tx,
    .hif_rx             = win_if_rx,
    .hif_get_settings   = win_if_get_settings,
    .hif_get_mtu        = win_if_get_mtu,
    .hif_get_encap      = win_if_get_encap,
    .hif_stats_update   = NULL,
};

void
vr_host_vif_init(struct vrouter *router)
{
    UNREFERENCED_PARAMETER(router);
}

void
vr_host_interface_exit(void)
{
    /* Noop */
}

void
vhost_xconnect(void)
{
    struct vrouter *vrouter = vrouter_get(0);
    struct vr_interface *host_if;

    if (vrouter->vr_host_if != NULL) {
        host_if = vrouter->vr_host_if;
        vif_set_xconnect(host_if);

        if (host_if->vif_bridge != NULL)
            vif_set_xconnect(host_if->vif_bridge);
    }
}

void
vhost_remove_xconnect(void)
{
    struct vrouter *vrouter = vrouter_get(0);
    struct vr_interface *host_if;

    if (vrouter->vr_host_if != NULL) {
        host_if = vrouter->vr_host_if;
        vif_remove_xconnect(host_if);

        if (host_if->vif_bridge != NULL)
            vif_remove_xconnect(host_if->vif_bridge);
    }
}

struct vr_host_interface_ops *
vr_host_interface_init(void)
{
    NDIS_INIT_MUTEX(&win_if_mutex);

    return &win_host_interface_ops;
}
