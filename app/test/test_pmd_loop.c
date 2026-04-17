/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024
 */

#include "test.h"
#include <string.h>
#include <rte_ethdev.h>
#include <rte_bus_vdev.h>
#include <rte_mbuf.h>

#define SOCKET0 0
#define NB_MBUF 512
#define RING_SIZE 256

static struct rte_mempool *mp;
static int loop_port = -1;

static int
test_loop_setup(void)
{
	struct rte_eth_conf conf;
	int ret;

	mp = rte_pktmbuf_pool_create("loop_mp", NB_MBUF, 32, 0,
			RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	TEST_ASSERT(mp != NULL, "Failed to create mempool");

	ret = rte_vdev_init("net_loop_test", NULL);
	TEST_ASSERT(ret == 0, "Failed to create net_loop_test vdev: %d", ret);

	ret = rte_eth_dev_get_port_by_name("net_loop_test", (uint16_t *)&loop_port);
	TEST_ASSERT(ret == 0, "Failed to find loop port: %d", ret);

	memset(&conf, 0, sizeof(conf));
	ret = rte_eth_dev_configure(loop_port, 1, 1, &conf);
	TEST_ASSERT(ret == 0, "Failed to configure port %d: %d", loop_port, ret);

	ret = rte_eth_rx_queue_setup(loop_port, 0, RING_SIZE, SOCKET0, NULL, mp);
	TEST_ASSERT(ret == 0, "Failed to setup RX queue: %d", ret);

	ret = rte_eth_tx_queue_setup(loop_port, 0, RING_SIZE, SOCKET0, NULL);
	TEST_ASSERT(ret == 0, "Failed to setup TX queue: %d", ret);

	ret = rte_eth_dev_start(loop_port);
	TEST_ASSERT(ret == 0, "Failed to start port %d: %d", loop_port, ret);

	return TEST_SUCCESS;
}

static void
test_loop_cleanup(void)
{
	if (loop_port >= 0) {
		rte_eth_dev_stop(loop_port);
		rte_eth_dev_close(loop_port);
		loop_port = -1;
	}
	rte_vdev_uninit("net_loop_test");
	if (mp != NULL) {
		rte_mempool_free(mp);
		mp = NULL;
	}
}

static int
test_loop_configure(void)
{
	struct rte_eth_link link;
	int ret;

	ret = rte_eth_link_get(loop_port, &link);
	TEST_ASSERT(ret == 0, "Failed to get link: %d", ret);
	TEST_ASSERT(link.link_status == RTE_ETH_LINK_UP,
			"Link is not UP after dev_start");
	TEST_ASSERT(link.link_speed == RTE_ETH_SPEED_NUM_10G,
			"Unexpected link speed");

	return TEST_SUCCESS;
}

static int
test_loop_tx_rx(void)
{
	struct rte_mbuf *tx_bufs[RING_SIZE];
	struct rte_mbuf *rx_bufs[RING_SIZE];
	unsigned int i;
	uint16_t nb_tx, nb_rx;

	/* Allocate and fill TX packets */
	for (i = 0; i < RING_SIZE / 2; i++) {
		tx_bufs[i] = rte_pktmbuf_alloc(mp);
		TEST_ASSERT(tx_bufs[i] != NULL, "Failed to alloc mbuf %u", i);
	}

	/* TX burst */
	nb_tx = rte_eth_tx_burst(loop_port, 0, tx_bufs, RING_SIZE / 2);
	TEST_ASSERT(nb_tx == RING_SIZE / 2,
			"TX burst failed: sent %u, expected %u",
			nb_tx, RING_SIZE / 2);

	/* RX burst */
	nb_rx = rte_eth_rx_burst(loop_port, 0, rx_bufs, RING_SIZE);
	TEST_ASSERT(nb_rx == RING_SIZE / 2,
			"RX burst failed: received %u, expected %u",
			nb_rx, RING_SIZE / 2);

	/* Verify pointers match (zero-copy) */
	for (i = 0; i < nb_rx; i++) {
		TEST_ASSERT(rx_bufs[i] == tx_bufs[i],
				"RX mbuf %u does not match TX", i);
		rte_pktmbuf_free(rx_bufs[i]);
	}

	return TEST_SUCCESS;
}

static int
test_loop_stats(void)
{
	struct rte_mbuf *pbuf;
	struct rte_eth_stats stats;
	int ret;

	/* Reset stats first */
	ret = rte_eth_stats_reset(loop_port);
	TEST_ASSERT(ret == 0, "Failed to reset stats: %d", ret);

	/* Verify stats are zero */
	ret = rte_eth_stats_get(loop_port, &stats);
	TEST_ASSERT(ret == 0, "Failed to get stats: %d", ret);
	TEST_ASSERT(stats.ipackets == 0, "ipackets not 0 after reset");
	TEST_ASSERT(stats.opackets == 0, "opackets not 0 after reset");

	/* Send and receive 1 packet */
	pbuf = rte_pktmbuf_alloc(mp);
	TEST_ASSERT(pbuf != NULL, "Failed to alloc mbuf");

	ret = rte_eth_tx_burst(loop_port, 0, &pbuf, 1);
	TEST_ASSERT(ret == 1, "TX burst of 1 failed");

	ret = rte_eth_rx_burst(loop_port, 0, &pbuf, 1);
	TEST_ASSERT(ret == 1, "RX burst of 1 failed");

	rte_pktmbuf_free(pbuf);

	/* Verify stats updated */
	ret = rte_eth_stats_get(loop_port, &stats);
	TEST_ASSERT(ret == 0, "Failed to get stats: %d", ret);
	TEST_ASSERT(stats.ipackets == 1, "ipackets expected 1, got %lu",
			stats.ipackets);
	TEST_ASSERT(stats.opackets == 1, "opackets expected 1, got %lu",
			stats.opackets);

	/* Reset and verify zero again */
	rte_eth_stats_reset(loop_port);
	ret = rte_eth_stats_get(loop_port, &stats);
	TEST_ASSERT(ret == 0, "Failed to get stats after reset: %d", ret);
	TEST_ASSERT(stats.ipackets == 0, "ipackets not 0 after second reset");
	TEST_ASSERT(stats.opackets == 0, "opackets not 0 after second reset");

	return TEST_SUCCESS;
}

static struct unit_test_suite loop_pmd_suite = {
	.suite_name = "Loop PMD Unit Test Suite",
	.setup = test_loop_setup,
	.teardown = test_loop_cleanup,
	.unit_test_cases = {
		TEST_CASE(test_loop_configure),
		TEST_CASE(test_loop_tx_rx),
		TEST_CASE(test_loop_stats),
		TEST_CASES_END()
	}
};

static int
test_loop_pmd(void)
{
	return unit_test_suite_runner(&loop_pmd_suite);
}

REGISTER_FAST_TEST(loop_pmd_autotest, true, true, test_loop_pmd);
