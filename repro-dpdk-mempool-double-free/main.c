/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024
 *
 * DPDK App: Reproducing Duplicate Address Reception due to Double Free
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>

#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_pause.h>
#include <rte_per_lcore.h>
#include <rte_ring.h>

/* --- Configuration --- */

#define MBUF_POOL_NAME "PC_MBUF_POOL"
#define RING_NAME "PC_RING"
#define RING_SIZE 256
#define POOL_SIZE 64
#define MBUF_CACHE_SIZE 32
#define MBUF_DATA_ROOM 2048
#define BURST_SIZE 4

/* Default timing settings */
static uint32_t run_time_sec = 5;
static uint32_t interval_ms = 200;

struct lcore_params {
  struct rte_mempool *mp;
  struct rte_ring *ring;
};

/*
 * ============================================================
 *  Producer Core
 * ============================================================
 */
static int producer_lcore(void *arg) {
  struct lcore_params *p = arg;
  unsigned lcore_id = rte_lcore_id();
  struct rte_mbuf *bufs[BURST_SIZE];
  unsigned int n, sent;
  uint64_t start_tsc, end_tsc;
  int iteration = 0;

  printf(
      "Producer started on core %u. (Duration: %us, Interval: %ums)\n",
      lcore_id, run_time_sec, interval_ms);

  start_tsc = rte_get_timer_cycles();
  end_tsc = start_tsc + (uint64_t)run_time_sec * rte_get_timer_hz();

  while (rte_get_timer_cycles() < end_tsc) {
    iteration++;

    /* Step 1: Alloc one and inject double free */
    struct rte_mbuf *victim = rte_pktmbuf_alloc(p->mp);
    if (victim == NULL) {
      rte_pause();
      continue;
    }

    printf("[Producer] Core %u: ★ Injecting Double Free (victim: %p) ★\n", lcore_id,
           victim);

    /* First free */
    rte_pktmbuf_free(victim);
    /* Second free (Double Free) */
    rte_pktmbuf_free(victim);

    /* Step 2: Alloc BURST_SIZE mbufs -> Duplicate addresses may appear */
    for (n = 0; n < BURST_SIZE; n++) {
      bufs[n] = rte_pktmbuf_alloc(p->mp);
      if (bufs[n] == NULL)
        break;
      bufs[n]->hash.usr = 1; /* Mark as in-use */
    }

    if (unlikely(n == 0)) {
      rte_pause();
      continue;
    }

    /* Check for duplicates before enqueueing */
    for (unsigned int i = 0; i < n; i++) {
      for (unsigned int j = i + 1; j < n; j++) {
        if (bufs[i] == bufs[j]) {
          printf("[Producer] Core %u: ▶▶▶ DUPLICATE ADDRESS DETECTED in alloc! "
                 "bufs[%u]==bufs[%u]==%p ◀◀◀\n",
                 lcore_id, i, j, bufs[i]);
        }
      }
    }

    sent = rte_ring_mp_enqueue_burst(p->ring, (void **)bufs, n, NULL);
    printf("[Producer] Core %u: Alloc %u, Enqueue %u [iter %d]\n", lcore_id, n,
           sent, iteration);

    if (unlikely(sent < n)) {
      for (unsigned int j = sent; j < n; j++) {
        rte_pktmbuf_free(bufs[j]);
      }
    }

    rte_delay_ms(interval_ms);
  }

  printf("Producer finished.\n");
  return 0;
}

/*
 * ============================================================
 *  Consumer Core
 * ============================================================
 */
static int consumer_lcore(void *arg) {
  struct lcore_params *p = arg;
  unsigned lcore_id = rte_lcore_id();
  struct rte_mbuf *bufs[BURST_SIZE];
  unsigned int n;
  uint64_t start_tsc, end_tsc;
  int iteration = 0;

  printf(
      "Consumer started on core %u. (Duration: %us, Interval: %ums)\n",
      lcore_id, run_time_sec, interval_ms);

  start_tsc = rte_get_timer_cycles();
  end_tsc = start_tsc + (uint64_t)run_time_sec * rte_get_timer_hz();

  while (rte_get_timer_cycles() < end_tsc) {
    n = rte_ring_sc_dequeue_bulk(p->ring, (void **)bufs, BURST_SIZE, NULL);
    if (n == 0) {
      rte_pause();
      continue;
    }

    iteration++;
    printf("[Consumer] Core %u: Received %u packets [iter %d]\n", lcore_id, n,
           iteration);

    /* Check for duplicates */
    for (unsigned int i = 0; i < n; i++) {
      for (unsigned int j = i + 1; j < n; j++) {
        if (bufs[i] == bufs[j]) {
          printf("\n  ★★★ DUPLICATE ADDRESS DETECTED in Consumer: bufs[%u]==bufs[%u]==%p ★★★\n\n",
                 i, j, bufs[i]);
        }
      }
    }

    /* Free with raw rte_pktmbuf_free (duplicates will return to pool as is) */
    for (unsigned int j = 0; j < n; j++) {
      rte_pktmbuf_free(bufs[j]);
    }

    /* Pool status check */
    {
      unsigned int avail = rte_mempool_avail_count(p->mp);
      unsigned int in_use = rte_mempool_in_use_count(p->mp);
      unsigned int pool_size = p->mp->size;
      if (avail > pool_size) {
        printf("[PoolCheck] ⚠ ANOMALY DETECTED: avail(%u) > size(%u). "
               "Pool contaminated by double free.\n",
               avail, pool_size);
      } else {
        printf("[PoolCheck] OK: avail=%u / in_use=%u / size=%u\n", avail,
               in_use, pool_size);
      }
    }

    rte_delay_ms(interval_ms);
  }

  /* Final check */
  {
    unsigned int avail = rte_mempool_avail_count(p->mp);
    unsigned int in_use = rte_mempool_in_use_count(p->mp);
    unsigned int pool_size = p->mp->size;
    printf("\n[Final Check] avail=%u, in_use=%u, size=%u\n", avail, in_use,
           pool_size);
    if (avail > pool_size)
      printf("  Verdict: ★ POOL CONTAMINATED (Double Free confirmed)\n");
    else
      printf("  Verdict: ✓ OK or minor leakage\n");
  }

  printf("Consumer finished.\n");
  return 0;
}

int main(int argc, char **argv) {
  struct rte_mempool *mp;
  struct rte_ring *ring;
  struct lcore_params params;
  int ret;
  unsigned worker_core_id;

  ret = rte_eal_init(argc, argv);
  if (ret < 0)
    rte_panic("EAL init failed\n");

  argc -= ret;
  argv += ret;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
      run_time_sec = atoi(argv[++i]);
    else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
      interval_ms = atoi(argv[++i]);
  }

  printf("\n--- Double Free Reproduction (English Version) ---\n");
  printf("Pool Cache=%d, Pool Size=%d\n\n", MBUF_CACHE_SIZE, POOL_SIZE);

  mp = rte_pktmbuf_pool_create(MBUF_POOL_NAME, POOL_SIZE, MBUF_CACHE_SIZE, 0,
                               MBUF_DATA_ROOM, rte_socket_id());
  if (!mp)
    rte_exit(EXIT_FAILURE, "MBUF pool create failed\n");

  ring = rte_ring_create(RING_NAME, RING_SIZE, rte_socket_id(), 0);
  if (!ring)
    rte_exit(EXIT_FAILURE, "Ring create failed\n");

  params.mp = mp;
  params.ring = ring;

  worker_core_id = rte_get_next_lcore(-1, 1, 0);
  rte_eal_remote_launch(producer_lcore, &params, worker_core_id);
  consumer_lcore(&params);
  rte_eal_mp_wait_lcore();

  rte_eal_cleanup();
  return 0;
}
