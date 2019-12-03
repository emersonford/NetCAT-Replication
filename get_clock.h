/* vim: set noet: */
/*
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id$
 *
 * Author: Michael S. Tsirkin <mst@mellanox.co.il>
 */

#ifndef GET_CLOCK_H
#define GET_CLOCK_H

#include <stdint.h>

#if defined (__x86_64__) || defined(__i386__)
/* Note: only x86 CPUs which have rdtsc instruction are supported. */
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles()
{
	unsigned low, high;
	unsigned long long val;
	asm volatile ("rdtsc" : "=a" (low), "=d" (high));
	val = high;
	val = (val << 32) | low;
	return val;
}

/**
 * Source: https://github.com/google/highwayhash/blob/master/highwayhash/tsc_timer.h
 *
 * Accurately measures the cycle start and cycle end count.
 */
	static __inline __attribute__((always_inline))
uint64_t start_tsc()
{
	uint64_t t;
	__asm__ __volatile__(
			"lfence\n\t"
			"rdtsc\n\t"
			"shl $32, %%rdx\n\t"
			"or %%rdx, %0\n\t"
			"lfence"
			: "=a"(t)
			:
			// "memory" avoids reordering. rdx = TSC >> 32.
			// "cc" = flags modified by SHL.
			: "rdx", "memory", "cc");

	return t;
}

	static __inline __attribute__((always_inline))
uint64_t stop_tsc()
{
	uint64_t t;
	__asm__ __volatile__(
			"rdtscp\n\t"
			"shl $32, %%rdx\n\t"
			"or %%rdx, %0\n\t"
			"lfence"
			: "=a"(t)
			:
			// "memory" avoids reordering. rcx = TSC_AUX. rdx = TSC >> 32.
			// "cc" = flags modified by SHL.
			: "rcx", "rdx", "memory", "cc");

	return t;
}

#elif defined(__PPC__) || defined(__PPC64__)
/* Note: only PPC CPUs which have mftb instruction are supported. */
/* PPC64 has mftb */
typedef unsigned long cycles_t;
static inline cycles_t get_cycles()
{
	cycles_t ret;

	__asm__ __volatile__ ("\n\t isync" "\n\t mftb %0" : "=r"(ret));
	return ret;
}
#elif defined(__ia64__)
/* Itanium2 and up has ar.itc (Itanium1 has errata) */
typedef unsigned long cycles_t;
static inline cycles_t get_cycles()
{
	cycles_t ret;

	asm volatile ("mov %0=ar.itc" : "=r" (ret));
	return ret;
}
#elif defined(__ARM_ARCH_7A__)
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles(void)
{
	cycles_t        clk;
	asm volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r" (clk));
	return clk;
}
#elif defined(__s390x__) || defined(__s390__)
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles(void)
{
	cycles_t        clk;
	asm volatile("stck %0" : "=Q" (clk) : : "cc");
	return clk >> 2;
}
#elif defined(__sparc__) && defined(__arch64__)
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles(void)
{
	cycles_t v;
	asm volatile ("rd %%tick, %0" : "=r" (v) : );
	return v;
}
#elif defined(__aarch64__)

typedef unsigned long cycles_t;
static inline cycles_t get_cycles()
{
	cycles_t cval;
	asm volatile("isb" : : : "memory");
	asm volatile("mrs %0, cntvct_el0" : "=r" (cval));
	return cval;
}

#else
#warning get_cycles not implemented for this architecture: attempt asm/timex.h
#include <asm/timex.h>
#endif

extern double get_cpu_mhz(int);

#endif
