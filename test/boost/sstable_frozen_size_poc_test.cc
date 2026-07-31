/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

// POC for SCYLLADB-2476: compare the on-the-wire/on-disk size of a mutation
// encoded as a `frozen_mutation` (the IDL `ser` format used inside raft_command /
// commitlog) vs. the SSTable "mx" row format (`sstable::data_size()`), which is
// what the ticket proposes reusing.
//
// The comparison intentionally uses `data_size()` (the Data.db logical size),
// which contains only the per-partition body (partition key + rows) and NOT the
// serialization header / column names (those live in Statistics.db). That mirrors
// the proposed design where the schema-derived header is amortized once per batch
// while `frozen_mutation` pays its framing + 32B of UUIDs per mutation.

#include <fmt/format.h>
#include <iostream>
#include <chrono>

#include "schema/schema_builder.hh"
#include "mutation/mutation.hh"
#include "mutation/frozen_mutation.hh"
#include "mutation/canonical_mutation.hh"
#include "sstables/sstables.hh"
#include "replica/memtable.hh"

#include "test/lib/scylla_test_case.hh"
#include "test/lib/sstable_test_env.hh"
#include "test/lib/sstable_utils.hh"
#include "test/lib/test_services.hh"

using namespace sstables;

namespace {

// A single int64 regular cell written at one timestamp.
mutation make_pair_mutation(schema_ptr s, int64_t pk, int64_t v, api::timestamp_type ts) {
    auto key = partition_key::from_single_value(*s, long_type->decompose(data_value(pk)));
    mutation m(s, key);
    m.set_clustered_cell(clustering_key::make_empty(), "v", data_value(v), ts);
    return m;
}

// (pk bigint, ck bigint, v bigint) PRIMARY KEY (pk, ck) — a typical SC tablet row.
mutation make_row_mutation(schema_ptr s, int64_t pk, int64_t ck, int64_t v, api::timestamp_type ts) {
    auto key = partition_key::from_single_value(*s, long_type->decompose(data_value(pk)));
    mutation m(s, key);
    auto ckey = clustering_key::from_single_value(*s, long_type->decompose(data_value(ck)));
    m.set_clustered_cell(ckey, "v", data_value(v), ts);
    return m;
}

uint64_t sstable_data_size(test_env& env, schema_ptr s, sstable_version_types v,
                           utils::chunked_vector<mutation> muts) {
    auto sst = make_sstable_containing([&] { return env.make_sstable(s, v); }, std::move(muts), validate::no).get();
    return sst->data_size();
}

void report(const char* scenario, test_env& env, schema_ptr s,
            std::function<mutation(int64_t /*pk*/)> gen) {
    const auto v = get_highest_sstable_version();

    // Single mutation.
    auto m0 = gen(0);
    const uint64_t frozen = freeze(m0).representation().size();
    const uint64_t canonical = canonical_mutation(m0).representation().size();

    utils::chunked_vector<mutation> one;
    one.push_back(gen(0));
    const uint64_t sst1 = sstable_data_size(env, s, v, std::move(one));

    // N distinct partitions in one sstable -> amortized per-partition body size.
    constexpr int64_t N = 1000;
    utils::chunked_vector<mutation> many;
    many.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        many.push_back(gen(i));
    }
    const uint64_t sstN = sstable_data_size(env, s, v, std::move(many));
    const double per_part = double(sstN) / double(N);

    std::cout << "\n================  " << scenario << "  ================\n";
    std::cout << fmt::format("  frozen_mutation representation : {:>7} bytes  (per mutation)\n", frozen);
    std::cout << fmt::format("  canonical_mutation             : {:>7} bytes  (per mutation, +column_mapping)\n", canonical);
    std::cout << fmt::format("  sstable data_size, 1 partition : {:>7} bytes\n", sst1);
    std::cout << fmt::format("  sstable data_size, {} parts   : {:>7} bytes  => {:.1f} bytes/partition (amortized)\n",
                            N, sstN, per_part);
    std::cout << fmt::format("  ------------------------------------------------\n");
    std::cout << fmt::format("  reduction (frozen -> sstable/part)    : {:.1f}x  ({:.1f}% smaller)\n",
                            double(frozen) / per_part, 100.0 * (1.0 - per_part / double(frozen)));
    std::cout << fmt::format("  reduction (frozen -> sstable 1-part)  : {:.1f}x  ({:.1f}% smaller)\n",
                            double(frozen) / double(sst1), 100.0 * (1.0 - double(sst1) / double(frozen)));
    std::cout << std::endl;
}

} // anonymous namespace

SEASTAR_TEST_CASE(frozen_vs_sstable_size_poc) {
    return test_env::do_with_async([] (test_env& env) {
        const api::timestamp_type ts = 1500000000000000; // fixed, arbitrary

        // Scenario 1: exactly the ticket's case — a pair of int64 values.
        {
            auto s = schema_builder(this_smp_shard_count(), "ks", "pair")
                .with_column("pk", long_type, column_kind::partition_key)
                .with_column("v", long_type)
                .build();
            report("pair<int64,int64>  (pk bigint, v bigint)", env, s,
                   [&] (int64_t pk) { return make_pair_mutation(s, pk, pk + 1, ts); });
        }

        // Scenario 2: typical SC tablet row with a clustering key.
        {
            auto s = schema_builder(this_smp_shard_count(), "ks", "row")
                .with_column("pk", long_type, column_kind::partition_key)
                .with_column("ck", long_type, column_kind::clustering_key)
                .with_column("v", long_type)
                .build();
            report("row  (pk bigint, ck bigint, v bigint)", env, s,
                   [&] (int64_t pk) { return make_row_mutation(s, pk, pk * 10, pk + 1, ts); });
        }
    });
}

// Measures the CPU cost of frozen_mutation serialization (freeze) and
// deserialization (unfreeze) per op. This is the cost the sstable-format change
// would REPLACE on the SC write path — so it bounds the achievable CPU saving.
SEASTAR_TEST_CASE(frozen_serde_cpu_cost) {
    return test_env::do_with_async([] (test_env&) {
        using clk = std::chrono::steady_clock;
        const api::timestamp_type ts = 1500000000000000;
        auto s = schema_builder(this_smp_shard_count(), "ks", "pair")
            .with_column("pk", long_type, column_kind::partition_key)
            .with_column("v", long_type)
            .build();

        constexpr int N = 500000;

        // freeze cost
        uint64_t sink = 0;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            auto m = make_pair_mutation(s, i, i + 1, ts);
            auto fm = freeze(m);
            sink += fm.representation().size();
        }
        auto t1 = clk::now();
        // unfreeze cost (pre-freeze once, unfreeze in loop)
        auto m0 = make_pair_mutation(s, 42, 43, ts);
        auto fm0 = freeze(m0);
        auto t2 = clk::now();
        for (int i = 0; i < N; ++i) {
            auto m = fm0.unfreeze(s);
            sink += m.partition().static_row().size();
        }
        auto t3 = clk::now();

        // isolate mutation construction cost (subtracted from freeze loop)
        auto t4 = clk::now();
        for (int i = 0; i < N; ++i) {
            auto m = make_pair_mutation(s, i, i + 1, ts);
            sink += m.partition().static_row().size();
        }
        auto t5 = clk::now();

        auto ns = [&](clk::time_point a, clk::time_point b) {
            return double(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()) / N;
        };
        const double make_ns = ns(t4, t5);
        std::cout << "\n================  frozen_mutation serde CPU cost (pair<int64,int64>)  ================\n";
        std::cout << fmt::format("  make_mutation           : {:8.1f} ns/op\n", make_ns);
        std::cout << fmt::format("  freeze (incl. make)     : {:8.1f} ns/op\n", ns(t0, t1));
        std::cout << fmt::format("  freeze (make subtracted): {:8.1f} ns/op\n", ns(t0, t1) - make_ns);
        std::cout << fmt::format("  unfreeze                : {:8.1f} ns/op\n", ns(t2, t3));
        std::cout << fmt::format("  ---> freeze+unfreeze    : {:8.1f} ns/op  (the CPU the sstable codec would replace)\n",
                                (ns(t0, t1) - make_ns) + ns(t2, t3));
        std::cout << "  (compare to ~86 us of cluster CPU per SC write measured at 252k ops/s)\n";
        std::cout << "  sink=" << sink << std::endl;
    });
}
