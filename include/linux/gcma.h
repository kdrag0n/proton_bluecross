/*
 * gcma.h - Guaranteed Contiguous Memory Allocator
 *
 * GCMA aims for contiguous memory allocation with success and fast
 * latency guarantee.
 * It reserves large amount of memory and let it be allocated to
 * contiguous memory requests.
 *
 * Copyright (C) 2014  LG Electronics Inc.,
 * Copyright (C) 2014  Minchan Kim <minchan@kernel.org>
 * Copyright (C) 2014-2015  SeongJae Park <sj38.park@gmail.com>
 */

#ifndef _LINUX_GCMA_H
#define _LINUX_GCMA_H

struct gcma;

int gcma_init(unsigned long start_pfn, unsigned long size,
	      struct gcma **res_gcma);
int gcma_alloc_contig(struct gcma *gcma,
		      unsigned long start_pfn, unsigned long size);
void gcma_free_contig(struct gcma *gcma,
		      unsigned long start_pfn, unsigned long size);

#endif /* _LINUX_GCMA_H */
