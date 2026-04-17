/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024
 */

#include <stdlib.h>

#include <rte_mbuf.h>
#include <ethdev_driver.h>
#include <ethdev_vdev.h>
#include <rte_malloc.h>
#include <bus_vdev_driver.h>
#include <rte_kvargs.h>
#include <rte_ring.h>

#define ETH_LOOP_RING_SIZE_ARG "size"
#define ETH_LOOP_DEFAULT_RING_SIZE 1024

static const char* valid_arguments[] = {ETH_LOOP_RING_SIZE_ARG, NULL};

/*
 * Each loop_queue holds a shared rte_ring.
 * TX enqueues mbuf pointers into it; RX dequeues from the same ring.
 * This is the "loopback": tx_burst → ring → rx_burst, zero-copy.
 */
struct loop_queue
{
    struct rte_ring* rng;
    uint16_t port_id;
    uint64_t rx_pkts;
    uint64_t tx_pkts;
};

struct pmd_internals
{
    uint16_t port_id;
    struct loop_queue rx_queues[1];
    struct loop_queue tx_queues[1];
    struct rte_ether_addr eth_addr;
};

static struct rte_eth_link pmd_link = {
    .link_speed = RTE_ETH_SPEED_NUM_10G,
    .link_duplex = RTE_ETH_LINK_FULL_DUPLEX,
    .link_status = RTE_ETH_LINK_DOWN,
    .link_autoneg = RTE_ETH_LINK_FIXED,
};

RTE_LOG_REGISTER_DEFAULT(eth_loop_logtype, NOTICE);
#define RTE_LOGTYPE_ETH_LOOP eth_loop_logtype

#define PMD_LOG(level, ...) RTE_LOG_LINE_PREFIX(level, ETH_LOOP, "%s(): ", __func__, __VA_ARGS__)

/* --- Data path --- */

static uint16_t eth_loop_tx(void* q, struct rte_mbuf** bufs, uint16_t nb_bufs)
{
    struct loop_queue* lq = q;
    void** ptrs = (void*)&bufs[0];
    const uint16_t nb_tx = (uint16_t)rte_ring_enqueue_burst(lq->rng, ptrs, nb_bufs, NULL);

    lq->tx_pkts += nb_tx;
    return nb_tx;
}

static uint16_t eth_loop_rx(void* q, struct rte_mbuf** bufs, uint16_t nb_bufs)
{
    struct loop_queue* lq = q;
    void** ptrs = (void*)&bufs[0];
    const uint16_t nb_rx = (uint16_t)rte_ring_dequeue_burst(lq->rng, ptrs, nb_bufs, NULL);
    unsigned int i;

    for (i = 0; i < nb_rx; i++)
        bufs[i]->port = lq->port_id;

    lq->rx_pkts += nb_rx;
    return nb_rx;
}

/* --- Device lifecycle callbacks --- */

static int eth_dev_configure(struct rte_eth_dev* dev __rte_unused)
{
    return 0;
}

static int eth_dev_start(struct rte_eth_dev* dev)
{
    uint16_t i;

    dev->data->dev_link.link_status = RTE_ETH_LINK_UP;

    for (i = 0; i < dev->data->nb_rx_queues; i++)
        dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
    for (i = 0; i < dev->data->nb_tx_queues; i++)
        dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;

    return 0;
}

static int eth_dev_stop(struct rte_eth_dev* dev)
{
    uint16_t i;

    dev->data->dev_link.link_status = RTE_ETH_LINK_DOWN;

    for (i = 0; i < dev->data->nb_rx_queues; i++)
        dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
    for (i = 0; i < dev->data->nb_tx_queues; i++)
        dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;

    return 0;
}

static int eth_dev_close(struct rte_eth_dev* dev)
{
    struct pmd_internals* internals;

    if (rte_eal_process_type() != RTE_PROC_PRIMARY)
        return 0;

    internals = dev->data->dev_private;

    rte_ring_free(internals->rx_queues[0].rng);

    dev->data->mac_addrs = NULL;

    return 0;
}

/* --- Queue setup --- */

static int eth_rx_queue_setup(struct rte_eth_dev* dev, uint16_t rx_queue_id, uint16_t nb_rx_desc __rte_unused,
                              unsigned int socket_id __rte_unused, const struct rte_eth_rxconf* rx_conf __rte_unused,
                              struct rte_mempool* mb_pool __rte_unused)
{
    struct pmd_internals* internals = dev->data->dev_private;

    if (rx_queue_id != 0)
        return -EINVAL;

    internals->rx_queues[rx_queue_id].port_id = dev->data->port_id;
    dev->data->rx_queues[rx_queue_id] = &internals->rx_queues[rx_queue_id];

    return 0;
}

static int eth_tx_queue_setup(struct rte_eth_dev* dev, uint16_t tx_queue_id, uint16_t nb_tx_desc __rte_unused,
                              unsigned int socket_id __rte_unused, const struct rte_eth_txconf* tx_conf __rte_unused)
{
    struct pmd_internals* internals = dev->data->dev_private;

    if (tx_queue_id != 0)
        return -EINVAL;

    dev->data->tx_queues[tx_queue_id] = &internals->tx_queues[tx_queue_id];

    return 0;
}

/* --- Device info --- */

static int eth_dev_info(struct rte_eth_dev* dev __rte_unused, struct rte_eth_dev_info* dev_info)
{
    dev_info->max_mac_addrs = 1;
    dev_info->max_rx_pktlen = (uint32_t)-1;
    dev_info->max_rx_queues = 1;
    dev_info->max_tx_queues = 1;
    dev_info->min_rx_bufsize = 0;
    dev_info->rx_offload_capa = RTE_ETH_RX_OFFLOAD_SCATTER;
    dev_info->tx_offload_capa = RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

    return 0;
}

/* --- Statistics --- */

static int eth_stats_get(struct rte_eth_dev* dev, struct rte_eth_stats* stats, struct eth_queue_stats* qstats)
{
    const struct pmd_internals* internals = dev->data->dev_private;

    stats->ipackets = internals->rx_queues[0].rx_pkts;
    stats->opackets = internals->tx_queues[0].tx_pkts;

    if (qstats != NULL) {
        qstats->q_ipackets[0] = internals->rx_queues[0].rx_pkts;
        qstats->q_opackets[0] = internals->tx_queues[0].tx_pkts;
    }

    return 0;
}

static int eth_stats_reset(struct rte_eth_dev* dev)
{
    struct pmd_internals* internals = dev->data->dev_private;

    internals->rx_queues[0].rx_pkts = 0;
    internals->tx_queues[0].tx_pkts = 0;

    return 0;
}

/* --- Misc callbacks --- */

static int eth_link_update(struct rte_eth_dev* dev __rte_unused, int wait_to_complete __rte_unused)
{
    return 0;
}

static int eth_promiscuous_enable(struct rte_eth_dev* dev __rte_unused)
{
    return 0;
}

static int eth_promiscuous_disable(struct rte_eth_dev* dev __rte_unused)
{
    return 0;
}

static int eth_mac_addr_set(struct rte_eth_dev* dev, struct rte_ether_addr* addr)
{
    struct pmd_internals* internals = dev->data->dev_private;

    rte_ether_addr_copy(addr, &internals->eth_addr);
    return 0;
}

/* --- eth_dev_ops --- */

static const struct eth_dev_ops ops = {
    .dev_close = eth_dev_close,
    .dev_start = eth_dev_start,
    .dev_stop = eth_dev_stop,
    .dev_configure = eth_dev_configure,
    .dev_infos_get = eth_dev_info,
    .rx_queue_setup = eth_rx_queue_setup,
    .tx_queue_setup = eth_tx_queue_setup,
    .link_update = eth_link_update,
    .stats_get = eth_stats_get,
    .stats_reset = eth_stats_reset,
    .promiscuous_enable = eth_promiscuous_enable,
    .promiscuous_disable = eth_promiscuous_disable,
    .mac_addr_set = eth_mac_addr_set,
};

/* --- Device creation --- */

static int eth_dev_loop_create(struct rte_vdev_device* vdev, unsigned int ring_size)
{
    struct rte_eth_dev_data* data;
    struct pmd_internals* internals;
    struct rte_eth_dev* eth_dev;
    struct rte_ring* rng;
    char rng_name[RTE_RING_NAMESIZE];
    const char* name = rte_vdev_device_name(vdev);

    if (vdev->device.numa_node == SOCKET_ID_ANY)
        vdev->device.numa_node = rte_socket_id();

    PMD_LOG(INFO, "Creating loop ethdev on numa socket %u", vdev->device.numa_node);

    /* Create the shared loopback ring */
    snprintf(rng_name, sizeof(rng_name), "LOOP_%s", name);
    rng = rte_ring_create(rng_name, ring_size, vdev->device.numa_node, RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (rng == NULL) {
        PMD_LOG(ERR, "Failed to create ring %s", rng_name);
        return -ENOENT;
    }

    eth_dev = rte_eth_vdev_allocate(vdev, sizeof(*internals));
    if (eth_dev == NULL) {
        rte_ring_free(rng);
        return -ENOMEM;
    }

    internals = eth_dev->data->dev_private;
    internals->port_id = eth_dev->data->port_id;

    /* Same ring for TX and RX: the core loopback mechanism */
    internals->tx_queues[0].rng = rng;
    internals->tx_queues[0].port_id = internals->port_id;
    internals->rx_queues[0].rng = rng;
    internals->rx_queues[0].port_id = internals->port_id;

    rte_eth_random_addr(internals->eth_addr.addr_bytes);

    data = eth_dev->data;
    data->nb_rx_queues = 1;
    data->nb_tx_queues = 1;
    data->dev_link = pmd_link;
    data->mac_addrs = &internals->eth_addr;
    data->promiscuous = 1;
    data->dev_flags |= RTE_ETH_DEV_AUTOFILL_QUEUE_XSTATS;

    eth_dev->dev_ops = &ops;
    eth_dev->rx_pkt_burst = eth_loop_rx;
    eth_dev->tx_pkt_burst = eth_loop_tx;

    rte_eth_dev_probing_finish(eth_dev);
    return 0;
}

/* --- Argument parsing --- */

static int get_ring_size_arg(const char* key __rte_unused, const char* value, void* extra_args)
{
    unsigned int* ring_size = extra_args;

    if (value == NULL || extra_args == NULL)
        return -EINVAL;

    *ring_size = (unsigned int)strtoul(value, NULL, 0);
    if (*ring_size == 0)
        return -EINVAL;

    return 0;
}

/* --- Vdev probe/remove --- */

static int rte_pmd_loop_probe(struct rte_vdev_device* vdev)
{
    const char *name, *params;
    unsigned int ring_size = ETH_LOOP_DEFAULT_RING_SIZE;
    struct rte_kvargs* kvlist = NULL;
    int ret;

    if (vdev == NULL)
        return -EINVAL;

    name = rte_vdev_device_name(vdev);
    params = rte_vdev_device_args(vdev);

    PMD_LOG(INFO, "Initializing pmd_loop for %s", name);

    if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
        struct rte_eth_dev* eth_dev;

        eth_dev = rte_eth_dev_attach_secondary(name);
        if (eth_dev == NULL) {
            PMD_LOG(ERR, "Failed to probe %s", name);
            return -1;
        }
        eth_dev->dev_ops = &ops;
        eth_dev->device = &vdev->device;
        eth_dev->rx_pkt_burst = eth_loop_rx;
        eth_dev->tx_pkt_burst = eth_loop_tx;
        rte_eth_dev_probing_finish(eth_dev);
        return 0;
    }

    if (params != NULL) {
        kvlist = rte_kvargs_parse(params, valid_arguments);
        if (kvlist == NULL)
            return -1;

        ret = rte_kvargs_process(kvlist, ETH_LOOP_RING_SIZE_ARG, &get_ring_size_arg, &ring_size);
        if (ret < 0)
            goto free_kvlist;
    }

    ret = eth_dev_loop_create(vdev, ring_size);

free_kvlist:
    rte_kvargs_free(kvlist);
    return ret;
}

static int rte_pmd_loop_remove(struct rte_vdev_device* vdev)
{
    struct rte_eth_dev* eth_dev;

    if (vdev == NULL)
        return -EINVAL;

    eth_dev = rte_eth_dev_allocated(rte_vdev_device_name(vdev));
    if (eth_dev == NULL)
        return 0;

    eth_dev_close(eth_dev);
    rte_eth_dev_release_port(eth_dev);

    return 0;
}

static struct rte_vdev_driver pmd_loop_drv = {
    .probe = rte_pmd_loop_probe,
    .remove = rte_pmd_loop_remove,
};

RTE_PMD_REGISTER_VDEV(net_loop, pmd_loop_drv);
RTE_PMD_REGISTER_PARAM_STRING(net_loop, "size=<int>");
