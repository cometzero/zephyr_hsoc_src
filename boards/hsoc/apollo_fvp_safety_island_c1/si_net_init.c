/*
 * SPDX-FileCopyrightText: <text>Copyright 2026 Arm Limited and/or its
 * affiliates <open-source-office@arm.com></text>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/util.h>
#include <errno.h>

LOG_MODULE_REGISTER(si_net_init);

static int clear_ipv4_addr(struct net_if *iface)
{
	struct net_if_ipv4 *ipv4 = NULL;
	struct in_addr *existing_ip;
	int ret;

	if (!net_if_flag_is_set(iface, NET_IF_IPV4)) {
		return 0;
	}

	ret = net_if_config_ipv4_get(iface, &ipv4);
	if (ret < 0) {
		return ret;
	}

	if (ipv4 == NULL) {
		net_if_config_ipv4_put(iface);
		return -EIO;
	}

	ARRAY_FOR_EACH(ipv4->unicast, i) {
		if (ipv4->unicast[i].ipv4.is_used) {
			existing_ip = &ipv4->unicast[i].ipv4.address.in_addr;

			if (!net_if_ipv4_addr_rm(iface, existing_ip)) {
				ret = -EADDRNOTAVAIL;
			}
		}
	}

	if (net_if_config_ipv4_put(iface) < 0 && ret == 0) {
		ret = -EIO;
	}

	return ret;
}

static int set_ipv4_addr(struct net_if *iface)
{
	struct in_addr addr;
	struct in_addr netmask;
	struct net_if_addr *ifaddr;
	int ret;

	if (sizeof(CONFIG_NET_CONFIG_MY_IPV4_ADDR) == 1) {
		return -ENODATA;
	}

	ret = net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_ADDR, &addr);
	if (ret < 0) {
		return ret;
	}

	if (!net_if_flag_is_set(iface, NET_IF_IPV4)) {
		net_if_flag_set(iface, NET_IF_IPV4);
	}

	ifaddr = net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	if (ifaddr == NULL) {
		return -ENOSPC;
	}

	if (sizeof(CONFIG_NET_CONFIG_MY_IPV4_NETMASK) > 1 &&
	    net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_NETMASK, &netmask) == 0) {
		net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);
	}

	return 0;
}

static int si_net_init(void)
{
	struct net_if *cfg_iface = net_if_get_by_index(CONFIG_SI_NET_INIT_IFACE_INDEX);
	struct net_if *base_iface = NULL;
	struct net_if *clear_iface;
	int ret;

	if (cfg_iface == NULL) {
		LOG_ERR("Config iface index %d not found", CONFIG_SI_NET_INIT_IFACE_INDEX);
		return -ENODEV;
	}

	clear_iface = net_if_get_by_index(CONFIG_SI_NET_INIT_CLEAR_IFACE_INDEX);

	if (clear_iface != NULL) {
		ret = clear_ipv4_addr(clear_iface);
		if (ret < 0) {
			LOG_ERR("Failed to clear IPv4 on iface %d (%d)",
				CONFIG_SI_NET_INIT_CLEAR_IFACE_INDEX, ret);
			return ret;
		}
	}

	base_iface = clear_iface;

	if (IS_ENABLED(CONFIG_NET_VLAN) && CONFIG_NET_CONFIG_MY_VLAN_ID > 0) {
		struct net_if *vlan_iface = NULL;

		if (base_iface == NULL) {
			base_iface = cfg_iface;
		}

		ret = net_eth_vlan_enable(base_iface, CONFIG_NET_CONFIG_MY_VLAN_ID);
		if (ret < 0) {
			LOG_ERR("Failed to enable VLAN %d (%d)", CONFIG_NET_CONFIG_MY_VLAN_ID, ret);
			return ret;
		}

		vlan_iface = net_eth_get_vlan_iface(base_iface, CONFIG_NET_CONFIG_MY_VLAN_ID);
		if (vlan_iface == NULL) {
			LOG_ERR("VLAN iface %d not found", CONFIG_NET_CONFIG_MY_VLAN_ID);
			return -ENODEV;
		}

		cfg_iface = vlan_iface;
	}

	/* Ensure default iface matches configured IPv4/VLAN interface */
	net_if_set_default(cfg_iface);

	ret = net_if_up(cfg_iface);
	if (ret < 0) {
		LOG_ERR("Failed to bring iface up (%d)", ret);
		return ret;
	}

	ret = set_ipv4_addr(cfg_iface);
	if (ret < 0) {
		LOG_ERR("Failed to set IPv4 addr (%d)", ret);
		return ret;
	}

	LOG_INF("Network interface configured");

	return 0;
}

SYS_INIT(si_net_init, APPLICATION, CONFIG_SI_NET_INIT_PRIORITY);
