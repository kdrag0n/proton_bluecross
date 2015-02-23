/*
 * gcma.h - Guaranteed Contiguous Memory Allocator
 *
 * GCMA aims for contiguous memory allocation with success and fast
 * latency guarantee.
 * It reserves large amount of memory and let it be allocated to
 * contiguous memory requests. Because system memory space efficiency could be
 * degraded if reserved area being idle, GCMA let the reserved area could be
 * used by other clients with lower priority.
 * We call those lower priority clients as second-class clients. In this
 * context, contiguous memory requests are first-class clients, of course.
 *
 * With this idea, gcma withdraw pages being used for second-class clients and
 * gives them to first-class clients if they required. Because latency
 * and success of first-class clients depend on speed and availability of
 * withdrawing, GCMA restricts only easily discardable memory could be used for
 * second-class clients.
 *
 * To support various second-class clients, GCMA provides interface and
 * backend of discardable memory. Any candiates satisfying with discardable
 * memory could be second-class client of GCMA using the interface.
 *
 * Currently, GCMA uses cleancache and write-through mode frontswap as
 * second-class clients.
 *
 * Copyright (C) 2014  LG Electronics Inc.,
 * Copyright (C) 2014  Minchan Kim <minchan@kernel.org>
 * Copyright (C) 2014-2015  SeongJae Park <sj38.park@gmail.com>
 */

#ifndef _LINUX_GCMA_H
#define _LINUX_GCMA_H

struct gcma;

#ifndef CONFIG_GCMA

inline int gcma_init(unsigned long start_pfn, unsigned long size,
		     struct gcma **res_gcma)
{
	return 0;
}

inline int gcma_alloc_contig(struct gcma *gcma,
			     unsigned long start, unsigned long end)
{
	return 0;
}

void gcma_free_contig(struct gcma *gcma,
		      unsigned long pfn, unsigned long nr_pages) { }

#else

int gcma_init(unsigned long start_pfn, unsigned long size,
	      struct gcma **res_gcma);
int gcma_alloc_contig(struct gcma *gcma,
		      unsigned long start_pfn, unsigned long size);
void gcma_free_contig(struct gcma *gcma,
		      unsigned long start_pfn, unsigned long size);

#endif

#endif /* _LINUX_GCMA_H */
