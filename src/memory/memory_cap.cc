//--------------------------------------------------------------------------
// Copyright (C) 2016-2022 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// memory_cap.cc author Joel Cornett <jocornet@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "memory_cap.h"

#include <malloc.h>
#include <sys/resource.h>

#include <atomic>
#include <cassert>
#include <vector>

#include "log/messages.h"
#include "main/snort_config.h"
#include "main/snort_types.h"
#include "main/thread.h"
#include "time/periodic.h"
#include "trace/trace_api.h"
#include "utils/stats.h"

#include "heap_interface.h"
#include "memory_config.h"
#include "memory_module.h"

using namespace snort;

namespace memory
{

// -----------------------------------------------------------------------------
// private
// -----------------------------------------------------------------------------

static std::vector<MemoryCounts> pkt_mem_stats;

static MemoryConfig config;
static size_t limit = 0;

static std::atomic<bool> over_limit { false };
static std::atomic<uint64_t> current_epoch { 0 };

static THREAD_LOCAL uint64_t last_dealloc = 0;
static THREAD_LOCAL uint64_t start_dealloc = 0;
static THREAD_LOCAL uint64_t start_epoch = 0;

static HeapInterface* heap = nullptr;
static PruneHandler pruner;

static void epoch_check(void*)
{
    uint64_t epoch, total;
    heap->get_process_total(epoch, total);

    current_epoch = epoch;

    bool prior = over_limit;
    over_limit = limit and total > limit;

    if ( prior != over_limit )
        trace_logf(memory_trace, nullptr, "Epoch=%lu, memory=%lu (%s)\n", epoch, total, over_limit?"over":"under");

    MemoryCounts& mc = memory::MemoryCap::get_mem_stats();

    if ( total > mc.max_in_use )
        mc.max_in_use = total;

    mc.cur_in_use = total;
    mc.epochs++;
}

// -----------------------------------------------------------------------------
// public
// -----------------------------------------------------------------------------

void MemoryCap::set_heap_interface(HeapInterface* h)
{ heap = h; }

void MemoryCap::set_pruner(PruneHandler p)
{ pruner = p; }

void MemoryCap::setup(const MemoryConfig& c, unsigned n, PruneHandler ph)
{
    assert(!is_packet_thread());

    pkt_mem_stats.resize(n);
    config = c;

    if ( !heap )
        heap = HeapInterface::get_instance();

    if ( !config.enabled )
        return;

    if ( !pruner )
        pruner = ph;

    limit = config.cap * config.threshold / 100;
    over_limit = false;
    current_epoch = 0;

    Periodic::register_handler(epoch_check, nullptr, 0, config.interval);
    heap->main_init();

    MemoryCounts& mc = memory::MemoryCap::get_mem_stats();
#ifdef UNIT_TEST
    mc = { };
#endif

    epoch_check(nullptr);
    mc.start_up_use = mc.cur_in_use;
}

void MemoryCap::cleanup()
{
    pkt_mem_stats.resize(0);
    delete heap;
    heap = nullptr;
}

void MemoryCap::thread_init()
{
    if ( config.enabled )
        heap->thread_init();

    start_dealloc = 0;
    start_epoch = 0;
}

MemoryCounts& MemoryCap::get_mem_stats()
{
    // main thread stats do not overlap with packet threads
    if ( !is_packet_thread() )
        return pkt_mem_stats[0];

    auto id = get_instance_id();
    return pkt_mem_stats[id];
}

void MemoryCap::free_space()
{
    assert(is_packet_thread());

    MemoryCounts& mc = memory::MemoryCap::get_mem_stats();
    heap->get_thread_allocs(mc.allocated, mc.deallocated);

    if ( !over_limit and !start_dealloc )
        return;

    if ( !start_dealloc )
    {
        if ( current_epoch == start_epoch )
            return;

        start_dealloc = last_dealloc = mc.deallocated;
        start_epoch = current_epoch;
        mc.reap_cycles++;
    }

    mc.pruned += (mc.deallocated - last_dealloc);
    last_dealloc = mc.deallocated;

    if ( mc.deallocated - start_dealloc  >= config.prune_target )
    {
        start_dealloc = 0;
        return;
    }

    ++mc.reap_attempts;

    if ( pruner() )
        return;

    ++mc.reap_failures;
}

// called at startup and shutdown
void MemoryCap::print(bool verbose, bool init)
{
    if ( !config.enabled )
        return;

    MemoryCounts& mc = get_mem_stats();

    if ( init and (verbose or mc.start_up_use) )
    {
        LogLabel("memory");
        LogCount("pruning threshold", limit);
        LogCount("start up use", mc.start_up_use);
    }

    if ( limit and (mc.max_in_use > limit) )
        LogCount("process over limit", mc.max_in_use - limit);

    if ( verbose )
    {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        LogCount("max rss", ru.ru_maxrss * 1024);
    }
}

} // namespace memory

