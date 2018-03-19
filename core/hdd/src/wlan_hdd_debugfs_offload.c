
/*
 * Copyright (c) 2018 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: wlan_hdd_debugfs_offload.c
 *
 * WLAN Host Device Driver implementation to update
 * debugfs with offload information
 */

#include <wlan_hdd_debugfs_csr.h>
#include <wlan_hdd_main.h>
#include <cds_sched.h>
#include <wma_api.h>
#include "qwlan_version.h"
#include "wmi_unified_param.h"
#include "wlan_hdd_request_manager.h"

/**
 * wlan_hdd_mc_addr_list_info_debugfs() - Populate mc addr list info
 * @hdd_ctx: pointer to hdd context
 * @adapter: pointer to adapter
 * @buf: output buffer to hold mc addr list info
 * @buf_avail_len: available buffer length
 *
 * Return: No.of bytes populated by this function in buffer
 */
static ssize_t
wlan_hdd_mc_addr_list_info_debugfs(hdd_context_t *hdd_ctx,
				   hdd_adapter_t *adapter, uint8_t *buf,
				   ssize_t buf_avail_len)
{
	ssize_t length = 0;
	int ret_val;
	uint8_t i;
	t_multicast_add_list *mc_addr_list;

	if (!hdd_ctx->config->fEnableMCAddrList) {
		ret_val = scnprintf(buf, buf_avail_len,
				    "\nMC addr ini is disabled\n");
		if (ret_val > 0)
			length = ret_val;
		return length;
	}

	mc_addr_list = &adapter->mc_addr_list;

	if (mc_addr_list->mc_cnt == 0) {
		ret_val = scnprintf(buf, buf_avail_len,
				    "\nMC addr list is empty\n");
		if (ret_val > 0)
			length = ret_val;
		return length;
	}

	ret_val = scnprintf(buf, buf_avail_len,
			    "\nMC addr list with mc_cnt = %u\n",
			    mc_addr_list->mc_cnt);
	if (ret_val <= 0)
		return length;
	length += ret_val;

	for (i = 0; i < mc_addr_list->mc_cnt; i++) {
		if (length >= buf_avail_len) {
			hdd_err("No sufficient buf_avail_len");
			return buf_avail_len;
		}

		ret_val = scnprintf(buf + length, buf_avail_len - length,
				    MAC_ADDRESS_STR"\n",
				    MAC_ADDR_ARRAY(mc_addr_list->addr[i]));
		if (ret_val <= 0)
			return length;
		length += ret_val;
	}

	if (length >= buf_avail_len) {
		hdd_err("No sufficient buf_avail_len");
		return buf_avail_len;
	}
	ret_val = scnprintf(buf + length, buf_avail_len - length,
			    "mc_filter_applied = %u\n",
			    mc_addr_list->isFilterApplied);
	if (ret_val <= 0)
		return length;

	length += ret_val;
	return length;
}

/**
 * wlan_hdd_arp_offload_info_debugfs() - Populate arp offload info
 * @hdd_ctx: pointer to hdd context
 * @adapter: pointer to adapter
 * @buf: output buffer to hold arp offload info
 * @buf_avail_len: available buffer length
 *
 * Return: No.of bytes populated by this function in buffer
 */
static ssize_t
wlan_hdd_arp_offload_info_debugfs(hdd_context_t *hdd_ctx,
				   hdd_adapter_t *adapter, uint8_t *buf,
				   ssize_t buf_avail_len)
{
	ssize_t length = 0;
	int ret_val;
	struct hdd_arp_offload_info *offload;

	qdf_spin_lock(&adapter->arp_offload_info_lock);
	offload = &adapter->arp_offload_info;
	qdf_spin_unlock(&adapter->arp_offload_info_lock);

	if (offload->offload == false)
		ret_val = scnprintf(buf, buf_avail_len,
			    "ARP OFFLOAD: DISABLED\n");
	else
		ret_val = scnprintf(buf, buf_avail_len,
			    "ARP OFFLOAD: ENABLED (%u.%u.%u.%u)\n",
			    offload->ipv4[0],
			    offload->ipv4[1],
			    offload->ipv4[2],
			    offload->ipv4[3]);

	if (ret_val <= 0)
		return length;
	length = ret_val;

	return length;
}

#ifdef WLAN_NS_OFFLOAD
/**
 * wlan_hdd_ns_offload_info_debugfs() - Populate ns offload info
 * @hdd_ctx: pointer to hdd context
 * @adapter: pointer to adapter
 * @buf: output buffer to hold ns offload info
 * @buf_avail_len: available buffer length
 *
 * Return: No.of bytes populated by this function in buffer
 */
static ssize_t
wlan_hdd_ns_offload_info_debugfs(hdd_context_t *hdd_ctx,
				   hdd_adapter_t *adapter, uint8_t *buf,
				   ssize_t buf_avail_len)
{
	ssize_t length = 0;
	int ret_val;
	struct hdd_ns_offload_info *offload;
	tSirNsOffloadReq *ns_info;
	uint32_t i;

	qdf_spin_lock(&adapter->ns_offload_info_lock);
	offload = &adapter->ns_offload_info;
	qdf_spin_unlock(&adapter->ns_offload_info_lock);

	ret_val = scnprintf(buf, buf_avail_len,
			    "\n********* NS OFFLOAD DETAILS *******\n");
	if (ret_val <= 0)
		return length;
	length += ret_val;

	if (length >= buf_avail_len) {
		hdd_err("No sufficient buf_avail_len");
		return buf_avail_len;
	}

	if (offload->offload != true) {
		ret_val = scnprintf(buf + length, buf_avail_len - length,
				    "NS offload is not enabled\n");
		if (ret_val <= 0)
			return length;
		length += ret_val;

		return length;
	}

	ret_val = scnprintf(buf + length, buf_avail_len - length,
			    "NS offload enabled, %u ns addresses offloaded\n",
			    offload->num_ns_offload_count);
	if (ret_val <= 0)
		return length;
	length += ret_val;

	ns_info = &offload->nsOffloadInfo;
	for (i = 0; i < offload->num_ns_offload_count; i++) {
		if (length >= buf_avail_len) {
			hdd_err("No sufficient buf_avail_len");
			return buf_avail_len;
		}

		ret_val = scnprintf(buf + length, buf_avail_len - length,
				"%u. " IPV6_MAC_ADDRESS_STR " %s\n", i + 1,
				IPV6_MAC_ADDR_ARRAY(ns_info->targetIPv6Addr[i]),
				(ns_info->target_ipv6_addr_ac_type[i] ==
				 SIR_IPV6_ADDR_AC_TYPE) ?
				 " (ANY CAST)" : " (UNI CAST)");
		if (ret_val <= 0)
			return length;
		length += ret_val;
	}

	return length;
}
#else
/**
 * wlan_hdd_ns_offload_info_debugfs() - Populate ns offload info
 * @hdd_ctx: pointer to hdd context
 * @adapter: pointer to adapter
 * @buf: output buffer to hold ns offload info
 * @buf_avail_len: available buffer length
 *
 * Return: No.of bytes populated by this function in buffer
 */
static ssize_t
wlan_hdd_ns_offload_info_debugfs(hdd_context_t *hdd_ctx,
				   hdd_adapter_t *adapter, uint8_t *buf,
				   ssize_t buf_avail_len)
{
	return 0;
}
#endif

/**
 * wlan_hdd_apf_info_debugfs() - Populate apf offload info
 * @hdd_ctx: pointer to hdd context
 * @adapter: pointer to adapter
 * @buf: output buffer to hold apf offload info
 * @buf_avail_len: available buffer length
 *
 * Return: No.of bytes populated by this function in buffer
 */
static ssize_t
wlan_hdd_apf_info_debugfs(hdd_context_t *hdd_ctx,
				   hdd_adapter_t *adapter, uint8_t *buf,
				   ssize_t buf_avail_len)
{
	ssize_t length = 0;
	int ret_val;
	bool apf_enabled;

	apf_enabled = adapter->apf_enabled;

	ret_val = scnprintf(buf, buf_avail_len,
			    "\n APF OFFLOAD DETAILS, offload_applied: %u\n",
			    apf_enabled);
	if (ret_val <= 0)
		return length;
	length = ret_val;

	return length;
}

ssize_t
wlan_hdd_debugfs_update_filters_info(hdd_context_t *hdd_ctx,
				     hdd_adapter_t *adapter,
				     uint8_t *buf, ssize_t buf_avail_len)
{
	ssize_t len;
	int ret_val;
	hdd_station_ctx_t *hdd_sta_ctx;

	len = wlan_hdd_current_time_info_debugfs(buf, buf_avail_len);

	if (len >= buf_avail_len) {
		hdd_err("No sufficient buf_avail_len");
		return buf_avail_len;
	}

	if (adapter->device_mode != QDF_STA_MODE) {
		ret_val = scnprintf(buf + len, buf_avail_len - len,
				    "Interface is not operating in STA mode\n");
		if (ret_val <= 0)
			return len;

		len += ret_val;
		return len;
	}

	hdd_sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(adapter);
	if (hdd_sta_ctx->conn_info.connState != eConnectionState_Associated) {
		ret_val = scnprintf(buf + len, buf_avail_len - len,
				    "\nSTA is not connected\n");
		if (ret_val <= 0)
			return len;

		len += ret_val;
		return len;
	}

	len += wlan_hdd_mc_addr_list_info_debugfs(hdd_ctx, adapter, buf + len,
						  buf_avail_len - len);

	if (len >= buf_avail_len) {
		hdd_err("No sufficient buf_avail_len");
		return buf_avail_len;
	}
	len += wlan_hdd_arp_offload_info_debugfs(hdd_ctx, adapter, buf + len,
						 buf_avail_len - len);

	if (len >= buf_avail_len) {
		hdd_err("No sufficient buf_avail_len");
		return buf_avail_len;
	}
	len += wlan_hdd_ns_offload_info_debugfs(hdd_ctx, adapter, buf + len,
						buf_avail_len - len);

	if (len >= buf_avail_len) {
		hdd_err("No sufficient buf_avail_len");
		return buf_avail_len;
	}
	len += wlan_hdd_apf_info_debugfs(hdd_ctx, adapter, buf + len,
					 buf_avail_len - len);

	return len;
}
