// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/counters.h>

#include <arch/ops.h>
#include <fbl/alloc_checker.h>
#include <fbl/mutex.h>
#include <kernel/auto_lock.h>
#include <kernel/cmdline.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>
#include <lib/console.h>
#include <lk/init.h>
#include <platform.h>
#include <stdlib.h>
#include <string.h>

#include "counters_private.h"

// kernel.ld uses this and fills in the descriptor table size after it and then
// places the sorted descriptor table after that (and then pads to page size),
// so as to fully populate the counters::DescriptorVmo layout.
__USED __SECTION(".kcounter.desc.header") static const uint64_t vmo_header[] = {
    counters::DescriptorVmo::kMagic,
    SMP_MAX_CPUS,
};
static_assert(sizeof(vmo_header) ==
              offsetof(counters::DescriptorVmo, descriptor_table_size));

// This counter gets a constant value just as a sanity check.
KCOUNTER(magic, "kernel.counters.magic");

struct watched_counter_t {
    list_node node;
    const counters::Descriptor* desc;
    // TODO(cpu): add min, max.
};

static fbl::Mutex watcher_lock;
static list_node watcher_list = LIST_INITIAL_VALUE(watcher_list);
static thread_t* watcher_thread;

static bool prefix_match(const char* pre, const char* str) {
    return strncmp(pre, str, strlen(pre)) == 0;
}

// Binary search the sorted counter descriptors.
// We rely on SORT_BY_NAME() in the linker script for this to work.
static const counters::Descriptor* upper_bound(
    const char* val, const counters::Descriptor* first,
    const counters::Descriptor* last) {
    if (first >= last) {
        return last;
    }

    const counters::Descriptor* it;
    size_t step;
    auto count = last - first;

    while (count > 0) {
        step = count / 2;
        it = first + step;

        if (strcmp(it->name, val) < 0) {
            first = ++it;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

static void counters_init(unsigned level) {
    // Wire the memory defined in the .bss section to the counters.
    for (size_t ix = 0; ix != SMP_MAX_CPUS; ++ix) {
        percpu[ix].counters = CounterArena().CpuData(ix);
    }
    magic.Add(counters::DescriptorVmo::kMagic);
}

// Collapse values to only non-zero ones and sort.
void counters_clean_up_values(const uint64_t* values_in, uint64_t* values_out, size_t* count_out) {
    assert(values_in != values_out);

    *count_out = 0;
    for (size_t i = 0; i < SMP_MAX_CPUS; ++i) {
        if (values_in[i] > 0) {
            values_out[(*count_out)++] = values_in[i];
        }
    }

    qsort(values_out, *count_out, sizeof(uint64_t), [](const void* a, const void* b) {
        return (*((uint64_t*)a) > *((uint64_t*)b)) - (*((uint64_t*)a) < *((uint64_t*)b));
    });
}

static constexpr uint64_t DOT8_SHIFT = 8;

// This calculation tries to match what sheets.google.com uses for QUARTILE()
// and PERCENTAGE(), however this uses 56.8 rather than floating point.
// https://en.wikipedia.org/wiki/Percentile#The_linear_interpolation_between_closest_ranks_method
// This is just a linear interpolation between the items bracketing that
// percentage. The input value array must be sorted on entry to this function.
uint64_t counters_get_percentile(const uint64_t* values, size_t count, uint64_t percentage_dot8) {
    assert(count >= 2);

    uint64_t target_dot8 = (count - 1) * percentage_dot8;
    uint64_t low_index = target_dot8 >> DOT8_SHIFT;
    uint64_t high_index = low_index + 1;
    uint64_t fraction_dot8 = target_dot8 & 0xff;

    uint64_t delta = values[high_index] - values[low_index];
    return ((values[low_index] << DOT8_SHIFT) + fraction_dot8 * delta);
}

bool counters_has_outlier(const uint64_t* values_in) {
    uint64_t values[SMP_MAX_CPUS];
    size_t count;
    counters_clean_up_values(values_in, values, &count);
    if (count < 2) {
        return false;
    }

    // If there's a value that's an outlier per
    // https://en.wikipedia.org/wiki/Outlier#Tukey's_fences, then we deem it
    // worth outputting the per-core values. This is not perfect, but it is
    // somewhat tricky to determine outliers for small data sets. We typically
    // have something like 4 cores here, and e.g. calculating standard deviation
    // is not useful for so few values.
    const uint64_t q1_dot8 = counters_get_percentile(values, count, /*0.25*/ 64);
    const uint64_t q3_dot8 = counters_get_percentile(values, count, /*0.75*/ 192);
    const uint64_t k_dot8 = /*1.5*/ 384;
    const uint64_t q_delta_dot8 = q3_dot8 - q1_dot8;
    const int64_t low_dot8 =
        q1_dot8 - static_cast<int64_t>(((k_dot8 * q_delta_dot8) >> DOT8_SHIFT));
    const int64_t high_dot8 =
        q3_dot8 + static_cast<int64_t>(((k_dot8 * q_delta_dot8) >> DOT8_SHIFT));

    for (size_t i = 0; i < count; ++i) {
        if ((static_cast<int64_t>(values[i]) << DOT8_SHIFT) < low_dot8 ||
            (static_cast<int64_t>(values[i]) << DOT8_SHIFT) > high_dot8) {
            return true;
        }
    }

    return false;
}

static void dump_counter(const counters::Descriptor& desc, bool verbose) {
    size_t counter_index = &desc - CounterDesc().begin();

    uint64_t summary = 0;
    uint64_t values[SMP_MAX_CPUS];

    if (desc.type == counters::Type::kMax) {
        for (size_t ix = 0; ix != SMP_MAX_CPUS; ++ix) {
            // This value is not atomically consistent, therefore is just
            // an approximation. TODO(cpu): for ARM this might need some magic.
            values[ix] = percpu[ix].counters[counter_index];
            if (values[ix] > summary) {
                summary = values[ix];
            }
        }
    } else {
        for (size_t ix = 0; ix != SMP_MAX_CPUS; ++ix) {
            // This value is not atomically consistent, therefore is just
            // an approximation. TODO(cpu): for ARM this might need some magic.
            values[ix] = percpu[ix].counters[counter_index];
            summary += values[ix];
        }
    }

    printf("[%.2zu] %s = %lu\n", counter_index, desc.name, summary);
    if (summary == 0u) {
        return;
    }

    // Print the per-core counts if verbose (-v) is set and it's not zero, or if
    // a value for one of the cores indicates it's an outlier.
    if (verbose || counters_has_outlier(values)) {
        printf("     ");
        for (size_t ix = 0; ix != SMP_MAX_CPUS; ++ix) {
            if (values[ix] > 0) {
                printf("[%zu:%lu]", ix, values[ix]);
            }
        }
        printf("\n");
    }
}

static void dump_all_counters(bool verbose) {
    printf("%zu counters available:\n", CounterDesc().size());
    for (const auto& desc : CounterDesc()) {
        dump_counter(desc, verbose);
    }
}

static int watcher_thread_fn(void* arg) {
    bool verbose = static_cast<bool>(reinterpret_cast<uintptr_t>(arg));
    while (true) {
        {
            fbl::AutoLock lock(&watcher_lock);
            if (list_is_empty(&watcher_list)) {
                watcher_thread = nullptr;
                return 0;
            }

            watched_counter_t* wc;
            list_for_every_entry (&watcher_list, wc, watched_counter_t, node) {
                dump_counter(*wc->desc, verbose);
            }
        }

        thread_sleep_relative(ZX_SEC(2));
    }
}

static int view_counter(int argc, const cmd_args* argv) {
    bool verbose = false;
    if (argc == 3) {
        if (strcmp(argv[1].str, "-v") == 0) {
            verbose = true;
            argc--;
            argv++;
        }
    }

    if (argc == 2) {
        if (strcmp(argv[1].str, "all") == 0) {
            dump_all_counters(verbose);
        } else {
            int num_results = 0;
            auto name = argv[1].str;
            auto desc = upper_bound(name,
                                    CounterDesc().begin(), CounterDesc().end());
            while (desc != CounterDesc().end()) {
                if (!prefix_match(name, desc->name)) {
                    break;
                }
                dump_counter(*desc, verbose);
                ++num_results;
                ++desc;
            }
            if (num_results == 0) {
                printf("counter '%s' not found, try all\n", name);
            } else {
                printf("%d counters found\n", num_results);
            }
        }
    } else {
        printf(
            "counters view [-v] <counter-name>\n"
            "counters view [-v] <counter-prefix>\n"
            "counters view [-v] all\n");
        return 1;
    }

    return 0;
}

static int watch_counter(int argc, const cmd_args* argv) {
    bool verbose = false;
    if (argc == 3) {
        if (strcmp(argv[1].str, "-v") == 0) {
            verbose = true;
            argc--;
            argv++;
        }
    }

    if (argc == 2) {
        if (strcmp(argv[1].str, "stop") == 0) {
            fbl::AutoLock lock(&watcher_lock);
            watched_counter_t* wc;
            while ((wc = list_remove_head_type(
                        &watcher_list, watched_counter_t, node)) != nullptr) {
                delete wc;
            }
            // The thread exits itself it there are no counters.
            return 0;
        }

        size_t counter_id = argv[1].u;
        auto range = CounterDesc().size() - 1;
        if (counter_id > range) {
            printf("counter id must be in the 0 to %zu range\n", range);
            return 1;
        } else if ((counter_id == 0) && (strlen(argv[1].str) > 1)) {
            // Parsing a name as a number.
            printf("counter ids are numbers\n");
            return 1;
        }

        fbl::AllocChecker ac;
        auto wc = new (&ac) watched_counter_t{
            LIST_INITIAL_CLEARED_VALUE, CounterDesc().begin() + counter_id};
        if (!ac.check()) {
            printf("no memory for counter\n");
            return 1;
        }

        {
            fbl::AutoLock lock(&watcher_lock);
            list_add_head(&watcher_list, &wc->node);
            if (watcher_thread == nullptr) {
                watcher_thread = thread_create(
                    "counter-watcher", watcher_thread_fn,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(verbose)), LOW_PRIORITY);
                if (watcher_thread == nullptr) {
                    printf("no memory for watcher thread\n");
                    return 1;
                }
                thread_detach_and_resume(watcher_thread);
            }
        }

    } else {
        printf(
            "counters watch [-v] <counter-id>\n"
            "counters watch stop\n");
    }

    return 0;
}

static int cmd_counters(int argc, const cmd_args* argv, uint32_t flags) {
    if (argc > 1) {
        if (strcmp(argv[1].str, "view") == 0) {
            return view_counter(argc - 1, &argv[1]);
        }
        if (strcmp(argv[1].str, "watch") == 0) {
            return watch_counter(argc - 1, &argv[1]);
        }
    }

    printf(
        "inspect system counters:\n"
        "  counters view [-v] <name>\n"
        "  counters watch [-v] <id>\n");
    return 0;
}

LK_INIT_HOOK(kcounters, counters_init, LK_INIT_LEVEL_PLATFORM_EARLY);

STATIC_COMMAND_START
STATIC_COMMAND("counters", "view system counters", &cmd_counters)
STATIC_COMMAND_END(mem_tests);
