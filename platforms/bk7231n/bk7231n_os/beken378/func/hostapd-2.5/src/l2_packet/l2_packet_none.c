/*
 * WPA Supplicant - Layer2 packet handling example with dummy functions
 * Copyright (c) 2003-2005, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 *
 * This file can be used as a starting point for layer2 packet implementation.
 */

#include "includes.h"

#include "common.h"
#include "eloop.h"
#include "l2_packet.h"
#include "fake_socket.h"
#include "mac.h"
#include "rw_msdu.h"
#include "uart_pub.h"

#define L2_RX_TMP_BUF_LEN     512

/* Above the LMAC TX_TIMEOUT watchdog (2 s for AC_BE/VI), which escalates to
 * txl_reset() and does confirm the descriptors, so a real hardware stall is
 * still rescued; below the AP's ~3 s 4-way handshake window, so a lost
 * confirmation cannot outlive the exchange it belongs to. */
#define EAPOL_TX_CONFIRM_TIMEOUT_MS   2500

struct l2_packet_data {
	char ifname[17];
	u8 own_addr[ETH_ALEN];
    u8 vif_index;
	void (*rx_callback)(void *ctx, const u8 *src_addr,
			    const u8 *buf, size_t len);
	void *rx_callback_ctx;
	int l2_hdr; /* whether to include layer 2 (Ethernet) header data
		     * buffers */
	int fd;
	/* One TX control block per interface instead of one per send. The TX
	 * path keeps a pointer to it in S_TYPE_ST->args and may confirm after
	 * we have stopped waiting, so the block has to outlive the call: on the
	 * stack it is a use-after-return, and a per-send allocation has to be
	 * abandoned on every timeout for the same reason. Reusing one block
	 * bounds that to a single allocation. Idea from @renren3221 in
	 * openshwprojects/OpenBK7231T_App#2175.
	 *
	 * It is heap-allocated rather than embedded because l2_packet_deinit()
	 * frees the l2 instance, and a confirmation may still be pending. */
	struct ieee80211_tx_cb *tx_cb;
	int tx_pending;		/* confirmation still owed for the last send */
};


int l2_packet_get_own_addr(struct l2_packet_data *l2, u8 *addr)
{
	os_memcpy(addr, l2->own_addr, ETH_ALEN);
	return 0;
}

/**
 * send L2 packet
 * return: <0 if error, else 0.
 */
int __l2_packet_send(struct l2_packet_data *l2, const u8 *dst_addr, u16 proto,
		   const u8 *buf, size_t len, int sync)
{
	int data_len, ret = 0;
	struct l2_ethhdr *eth;
	S_TYPE_PTR type_ptr;
	unsigned char *data_buf = NULL;
	struct ieee80211_tx_cb *cb = NULL;	//control block

	type_ptr = os_zalloc(sizeof(S_TYPE_ST));
	if (!type_ptr) {
		return -1;
	}

	data_len = len + sizeof(struct l2_ethhdr);
	data_buf = os_zalloc(data_len);
	if (data_buf == NULL) {
		os_free(type_ptr);
		return -1;
	}

	eth = (struct l2_ethhdr *) data_buf;
	os_memcpy(eth->h_dest, dst_addr, ETH_ALEN);
	os_memcpy(eth->h_source, l2->own_addr, ETH_ALEN);
	eth->h_proto = host_to_be16(proto);
	os_memcpy(eth+1, buf, len);

	type_ptr->type = HOSTAPD_DATA;
	type_ptr->vif_index = l2->vif_index;
	type_ptr->sync = sync;

	if (sync) {
		if (l2->tx_cb == NULL) {
			l2->tx_cb = os_zalloc(sizeof(*l2->tx_cb));
			if (l2->tx_cb == NULL) {
				os_free(data_buf);
				os_free(type_ptr);
				return -1;
			}
#if CFG_SUPPORT_ALIOS
			rtos_init_semaphore(&l2->tx_cb->sema, 0);
#else
			rtos_init_semaphore(&l2->tx_cb->sema, 1);
#endif
		} else if (l2->tx_pending) {
			/* Drop a confirmation left over from a send that timed
			 * out, so it cannot release this one instead. */
			while (rtos_get_semaphore(&l2->tx_cb->sema, 0) == kNoErr)
				;
		}
		cb = l2->tx_cb;
		cb->result = -1;
		type_ptr->args = (void *)cb;
	}

	/* Ownership of type_ptr (and of cb through it) transfers to the TX path
	 * only once the frame has actually been queued. */
	if (fsocket_send(l2->fd, data_buf, data_len, type_ptr) <= 0) {
		os_free(type_ptr);
		os_free(data_buf);
		return -1;
	}

	os_free(data_buf);

	if (!sync) {
		return 0;
	}

	l2->tx_pending = 1;

	ret = rtos_get_semaphore(&cb->sema, EAPOL_TX_CONFIRM_TIMEOUT_MS);
	if (ret != kNoErr) {
		/* The frame is still queued and still points at cb, so the block
		 * stays alive and tx_pending stays set. The next send drains the
		 * stale confirmation before reusing it. */
		bk_printf("%s: send failed\r\n", __func__);
		return -1;
	}

	l2->tx_pending = 0;
	ret = cb->result;
	bk_printf("%s: ret %d\r\n", __func__, ret);

	return ret;
}


int l2_packet_send(struct l2_packet_data *l2, const u8 *dst_addr, u16 proto,
		   const u8 *buf, size_t len)
{
	return __l2_packet_send(l2, dst_addr, proto, buf, len, 0);
}

int l2_packet_send_sync(struct l2_packet_data *l2, const u8 *dst_addr, u16 proto,
		   const u8 *buf, size_t len)
{
	return __l2_packet_send(l2, dst_addr, proto, buf, len, 1);
}

static void l2_packet_receive(int sock, void *eloop_ctx, void *sock_ctx)
{
	int len;
	unsigned char *buf;
	struct l2_ethhdr *hdr;
	struct l2_packet_data *l2 = eloop_ctx;

	buf = os_malloc(L2_RX_TMP_BUF_LEN);
	ASSERT(buf);
	
	len = fsocket_recv(sock, buf, L2_RX_TMP_BUF_LEN, 0);
	if (len < 0) {
		wpa_printf(MSG_ERROR, "recv: %s", strerror(errno));
		goto recv_exit;
	}
	
	hdr = (struct l2_ethhdr *) buf;
	l2->rx_callback(l2->rx_callback_ctx, 
						hdr->h_source, 
						buf + sizeof(struct l2_ethhdr), 
						len - sizeof(struct l2_ethhdr));

recv_exit:
	if(buf)
	{
		os_free(buf);
	}
	
	return;
}

extern UINT8 rwm_mgmt_vif_mac2idx(void *mac);
struct l2_packet_data * l2_packet_init(
	const char *ifname, const u8 *own_addr, unsigned short protocol,
	void (*rx_callback)(void *ctx, const u8 *src_addr,
			    const u8 *buf, size_t len),
	void *rx_callback_ctx, int l2_hdr)
{
	struct l2_packet_data *l2;

	l2 = os_zalloc(sizeof(struct l2_packet_data));
	if (l2 == NULL)
		return NULL;
	os_strlcpy(l2->ifname, ifname, sizeof(l2->ifname));
	os_memcpy(l2->own_addr, own_addr, ETH_ALEN);

    l2->vif_index = rwm_mgmt_vif_mac2idx((void*)own_addr);
    if(l2->vif_index == 0xff) {
        os_printf("not found vif_index in l2_packet_init\r\n");
    }
    
	l2->rx_callback = rx_callback;
	l2->rx_callback_ctx = rx_callback_ctx;
	l2->l2_hdr = l2_hdr;

	/*
	 * TODO: open connection for receiving frames
	 */
    protocol += l2->vif_index;
	l2->fd = fsocket_init(PF_PACKET, SOCK_RAW, protocol);
	if (l2->fd >= 0)
		eloop_register_read_sock(l2->fd, l2_packet_receive, l2, NULL);

	return l2;
}

struct l2_packet_data * l2_packet_init_bridge(
	const char *br_ifname, const char *ifname, const u8 *own_addr,
	unsigned short protocol,
	void (*rx_callback)(void *ctx, const u8 *src_addr,
			    const u8 *buf, size_t len),
	void *rx_callback_ctx, int l2_hdr)
{
	return l2_packet_init(br_ifname, own_addr, protocol, rx_callback,
			      rx_callback_ctx, l2_hdr);
}

void l2_packet_deinit(struct l2_packet_data *l2)
{
	if (l2 == NULL)
		return;

	if (l2->fd >= 0) 
	{
		eloop_unregister_read_sock(l2->fd);
		/* TODO: close connection */
		fsocket_close(l2->fd);
	}

	if (l2->tx_cb != NULL) {
		if (l2->tx_pending) {
			/* A confirmation is still owed and the TX path still
			 * points at this block, so it has to stay. One block
			 * abandoned per interface teardown is the price of not
			 * letting a late completion write into freed memory. */
			bk_printf("%s: tx confirm still pending, keeping cb\r\n",
				  __func__);
		} else {
			rtos_deinit_semaphore(&l2->tx_cb->sema);
			os_free(l2->tx_cb);
		}
	}

	os_free(l2);
}

int l2_packet_get_ip_addr(struct l2_packet_data *l2, char *buf, size_t len)
{
	/* TODO: get interface IP address */
	return -1;
}

void l2_packet_notify_auth_start(struct l2_packet_data *l2)
{
	/* This function can be left empty */
}

int l2_packet_set_packet_filter(struct l2_packet_data *l2,
				enum l2_packet_filter_type type)
{
	return -1;
}
