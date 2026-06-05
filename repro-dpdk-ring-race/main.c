/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026
 *
 * DPDK App: Reproducing Duplicate Address Reception due to Ring Race Condition
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
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_ring.h>

#define MBUF_POOL_NAME "RACE_MBUF_POOL"
#define RING_NAME "RACE_RING"
#define RING_SIZE 128
#define POOL_SIZE 8192
#define MBUF_CACHE_SIZE 0 // Disable cache to expose raw mempool contamination immediately
#define MBUF_DATA_ROOM 2048
#ifndef BURST_SIZE
#define BURST_SIZE 64
#endif
#define HISTORY_SIZE 65536

static uint32_t run_time_sec = 5;
static uint32_t interval_us = 0;

struct lcore_params {
  struct rte_mempool *mp;
  struct rte_ring *ring;
  volatile int stop;
};

/* Global atomic sequence counter */
static uint32_t global_seq = 1;

/* Sliding window history of processed sequence numbers to detect duplicate reads of stale slot contents */
static uint32_t processed_seqs[HISTORY_SIZE];
static uint32_t processed_idx = 0;
static bool processed_full = false;

static bool is_sequence_processed(uint32_t seq) {
  uint32_t limit = processed_full ? HISTORY_SIZE : processed_idx;
  for (uint32_t i = 0; i < limit; i++) {
    if (processed_seqs[i] == seq) {
      return true;
    }
  }
  return false;
}

static void record_processed_sequence(uint32_t seq) {
  processed_seqs[processed_idx] = seq;
  processed_idx = (processed_idx + 1) % HISTORY_SIZE;
  if (processed_idx == 0) {
    processed_full = true;
  }
}

/*
 * ============================================================
 *  Producer Core (Multiple instances run this concurrently)
 * ============================================================
 */
static int producer_lcore(void *arg) {
  struct lcore_params *p = arg;
  unsigned lcore_id = rte_lcore_id();
  struct rte_mbuf *bufs[BURST_SIZE];

  printf("Producer started on core %u.\n", lcore_id);

  while (!p->stop) {
    unsigned int n;
    unsigned int target_burst;
    if (BURST_SIZE <= 1) {
      target_burst = 1;
    } else {
      target_burst = ((lcore_id + global_seq) % (BURST_SIZE / 2)) + 1; // Variable size
    }
    for (n = 0; n < target_burst; n++) {
      bufs[n] = rte_pktmbuf_alloc(p->mp);
      if (bufs[n] == NULL) {
        break;
      }
      /* Assign a unique sequence number to distinguish new allocations from stale dequeues */
      uint32_t seq = __atomic_add_fetch(&global_seq, 1, __ATOMIC_RELAXED);
      bufs[n]->hash.usr = seq;
    }

    if (unlikely(n == 0)) {
      /* Mempool is temporarily exhausted. Sleep to let consumer free buffers and avoid CPU starvation. */
      rte_delay_us_block(100);
      continue;
    }

    /* Calling natural DPDK enqueue API based on build options */
#ifdef USE_MP_RING
    unsigned int sent = rte_ring_mp_enqueue_bulk(p->ring, (void **)bufs, n, NULL);
#else
    unsigned int sent = rte_ring_sp_enqueue_bulk(p->ring, (void **)bufs, n, NULL);
#endif

    if (unlikely(sent < n)) {
      /* In bulk mode, if enqueue fails, all 'n' elements must be freed */
      for (unsigned int j = 0; j < n; j++) {
        rte_pktmbuf_free(bufs[j]);
      }
    }

    if (interval_us > 0) {
      rte_delay_us_block(interval_us);
    } else {
      rte_pause();
    }
  }

  printf("Producer on core %u finished.\n", lcore_id);
  return 0;
}

/*
 * ============================================================
 *  Consumer Core (Single instance)
 * ============================================================
 */
static int consumer_lcore(void *arg) {
  struct lcore_params *p = arg;
  unsigned lcore_id = rte_lcore_id();
  struct rte_mbuf *bufs[BURST_SIZE];
  uint64_t start_tsc, end_tsc;
  uint64_t last_check_tsc;
  uint64_t check_interval_cycles = rte_get_timer_hz() / 10; /* 100ms */
  bool contaminated = false;
  uint64_t dup_count = 0;

  printf("Consumer started on core %u.\n", lcore_id);

  start_tsc = rte_get_timer_cycles();
  end_tsc = start_tsc + (uint64_t)run_time_sec * rte_get_timer_hz();
  last_check_tsc = start_tsc;

  while (rte_get_timer_cycles() < end_tsc && !p->stop) {
    /* Calling natural DPDK Single Consumer dequeue API */
    unsigned int n = rte_ring_sc_dequeue_burst(p->ring, (void **)bufs, BURST_SIZE, NULL);
    if (n == 0) {
      rte_pause();
      
      uint64_t now = rte_get_timer_cycles();
      if (now - last_check_tsc > check_interval_cycles) {
        last_check_tsc = now;
        unsigned int avail = rte_mempool_avail_count(p->mp);
        if (avail > p->mp->size) {
          printf("[PoolCheck] ⚠ ANOMALY DETECTED: avail(%u) > size(%u). Pool contaminated.\n",
                 avail, p->mp->size);
          contaminated = true;
          p->stop = 1;
          break;
        }
      }
      continue;
    }

    /* 1. Check for duplicate addresses within the same dequeued burst */
    for (unsigned int i = 0; i < n; i++) {
      for (unsigned int j = i + 1; j < n; j++) {
        if (bufs[i] == bufs[j] && bufs[i] != NULL) {
          dup_count++;
          printf("[Consumer] ★★★ DUPLICATE ADDRESS IN SAME BURST: %p (bufs[%u]==bufs[%u], total dup: %lu) ★★★\n",
                 bufs[i], i, j, dup_count);
        }
      }
    }

    /* 2. Check for sequence duplicates across historical runs & free */
    for (unsigned int i = 0; i < n; i++) {
      if (bufs[i] == NULL) continue;
      uint32_t seq = bufs[i]->hash.usr;
      if (is_sequence_processed(seq)) {
        dup_count++;
        printf("[Consumer] ★★★ DUPLICATE SEQUENCE DETECTED (STALE DEQUEUE): %p (seq: %u, total dup: %lu) ★★★\n",
               bufs[i], seq, dup_count);
      } else {
        record_processed_sequence(seq);
      }
      /* 
       * Freeing the duplicate buffers leads to double free,
       * causing mempool self-corruption.
       */
      rte_pktmbuf_free(bufs[i]);
    }

    uint64_t now = rte_get_timer_cycles();
    if (now - last_check_tsc > check_interval_cycles) {
      last_check_tsc = now;
      unsigned int avail = rte_mempool_avail_count(p->mp);
      unsigned int in_use = rte_mempool_in_use_count(p->mp);
      printf("[PoolCheck] avail=%u / in_use=%u / size=%u / dup_count=%lu\n",
             avail, in_use, p->mp->size, dup_count);
      if (avail > p->mp->size) {
        printf("[PoolCheck] ⚠ ANOMALY DETECTED: avail(%u) > size(%u). Pool contaminated.\n",
               avail, p->mp->size);
        contaminated = true;
        p->stop = 1;
        break;
      }
    }
  }

  p->stop = 1; /* Request producers to stop */

  unsigned int final_avail = rte_mempool_avail_count(p->mp);
  printf("\n[Final Check] avail=%u, size=%u, total_duplicates=%lu\n",
         final_avail, p->mp->size, dup_count);
  if (final_avail > p->mp->size || contaminated || dup_count > 0) {
    printf("  Verdict: ★ DUPLICATE DETECTED (Confirmed via Ring Race Condition) ★\n");
  } else {
    printf("  Verdict: ✓ OK or minor leakage (No race detected)\n");
  }

  printf("Consumer finished.\n");
  return 0;
}

int main(int argc, char **argv) {
  struct rte_mempool *mp;
  struct rte_ring *ring;
  struct lcore_params params;
  int ret;
  unsigned worker_cores[6];
  int num_workers = 6;
  unsigned lcore = -1;

  ret = rte_eal_init(argc, argv);
  if (ret < 0)
    rte_panic("EAL init failed\n");

  argc -= ret;
  argv += ret;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
      run_time_sec = atoi(argv[++i]);
    else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
      interval_us = atoi(argv[++i]);
  }

  printf("\n--- DPDK Ring Race Reproduction ---\n");
#ifdef USE_MP_RING
  printf("Mode: Multi-Producer Ring Enqueue (rte_ring_mp_enqueue_bulk)\n");
#else
  printf("Mode: Single-Producer Ring Enqueue (rte_ring_sp_enqueue_bulk) [WARNING: Race condition expected]\n");
#endif
  printf("Pool Cache=%d, Pool Size=%d\n\n", MBUF_CACHE_SIZE, POOL_SIZE);

  mp = rte_pktmbuf_pool_create(MBUF_POOL_NAME, POOL_SIZE, MBUF_CACHE_SIZE, 0,
                               MBUF_DATA_ROOM, rte_socket_id());
  if (!mp)
    rte_exit(EXIT_FAILURE, "MBUF pool create failed\n");

  /*
   * Initialize a default ring.
   */
#ifdef USE_MP_RING
  uint32_t ring_flags = RING_F_SC_DEQ; // MP/SC
#else
  uint32_t ring_flags = RING_F_SP_ENQ | RING_F_SC_DEQ; // SP/SC
#endif
  ring = rte_ring_create(RING_NAME, RING_SIZE, rte_socket_id(), ring_flags);
  if (!ring)
    rte_exit(EXIT_FAILURE, "Ring create failed\n");

  /*
   * PRE-FILL the entire ring buffer slots with valid mbuf pointers
   * to ensure stale flying dequeues read valid addresses instead of NULL.
   * This mimics a loaded ring state and forces duplicates to show up immediately
   * once a rollback occurs.
   */
  printf("Pre-filling ring with dummy packets...\n");
  void **ring_table = (void **)&ring[1];
  for (uint32_t i = 0; i < RING_SIZE; i++) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
    if (m == NULL) {
      rte_exit(EXIT_FAILURE, "Failed to allocate dummy packet during pre-fill\n");
    }
    m->hash.usr = 999999; /* Mark as dummy */
    ring_table[i] = m;
  }
  /* Set prod.tail and prod.head to RING_SIZE (queue is fully loaded) */
  ring->prod.head = RING_SIZE;
  ring->prod.tail = RING_SIZE;

  params.mp = mp;
  params.ring = ring;
  params.stop = 0;

  for (int i = 0; i < num_workers; i++) {
    lcore = rte_get_next_lcore(lcore, 1, 0);
    if (lcore == RTE_MAX_LCORE) {
      rte_exit(EXIT_FAILURE, "Need at least 7 lcores (1 master, 6 workers)\n");
    }
    worker_cores[i] = lcore;
  }

  for (int i = 0; i < num_workers; i++) {
    printf("Launching Producer on core %u\n", worker_cores[i]);
    rte_eal_remote_launch(producer_lcore, &params, worker_cores[i]);
  }

  consumer_lcore(&params);

  rte_eal_mp_wait_lcore();

  rte_eal_cleanup();
  return 0;
}
