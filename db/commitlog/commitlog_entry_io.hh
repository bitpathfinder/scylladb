/*
 * Copyright 2016-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#pragma once

#include "utils/assert.hh"
#include <optional>

#include "commitlog_entry.hh"
#include "commitlog_types.hh"
#include "mutation/frozen_mutation.hh"
#include "schema/schema_fwd.hh"
#include "replay_position.hh"

namespace detail {

    using buffer_type = fragmented_temporary_buffer;
    using base_iterator = typename std::vector<temporary_buffer<char>>::const_iterator;

    static constexpr auto sector_overhead_size = sizeof(uint32_t) + sizeof(db::segment_id_type);

    // iterator adaptor to enable splitting normal
    // frag-buffer temporary buffer objects into 
    // sub-disk-page sized chunks.
    class sector_split_iterator {
        base_iterator _iter, _end;
        char* _ptr;
        size_t _size;
        size_t _sector_size;

    public:
        sector_split_iterator(const sector_split_iterator&) noexcept;
        sector_split_iterator(base_iterator i, base_iterator e, size_t sector_size);
        sector_split_iterator(base_iterator i, base_iterator e, size_t sector_size, size_t overhead);
        sector_split_iterator();

        char* get_write() const {
            return _ptr;
        }
        size_t size() const {
            return _size;
        }
        char* begin() {
            return _ptr;
        }
        char* end() {
            return _ptr + _size;
        }
        const char* begin() const {
            return _ptr;
        }
        const char* end() const {
            return _ptr + _size;
        }

        bool operator==(const sector_split_iterator& rhs) const {
            return _iter == rhs._iter && _ptr == rhs._ptr;
        }

        auto& operator*() const {
            return *this;
        }
        auto* operator->() const {
            return this;
        }
        sector_split_iterator& operator++();
        sector_split_iterator operator++(int);
    };

    enum commitlog_entry_serialization_format : uint8_t { mutation, variant };
}

class commitlog_mutation_entry_writer {
public:
    using force_sync = db::commitlog_force_sync;
private:
    schema_ptr _schema;
    const frozen_mutation& _mutation;
    size_t _size = std::numeric_limits<size_t>::max();
    force_sync _sync;
    bool _with_schema = true;
    detail::commitlog_entry_serialization_format _commitlog_format = detail::commitlog_entry_serialization_format::mutation;

private:
    template<typename Output>
    void serialize(Output&) const;
    void compute_size();
public:
    commitlog_mutation_entry_writer(schema_ptr s, const frozen_mutation& fm, force_sync sync)
        : _schema(std::move(s)), _mutation(fm), _sync(sync)
    {}

    void set_with_schema(bool value) {
        if (std::exchange(_with_schema, value) != value || _size == std::numeric_limits<size_t>::max()) {
            compute_size();
        }
    }

    bool with_schema() const {
        return _with_schema;
    }

    void set_use_variant_commitlog_entry_format(bool value) {
        const auto new_format = value ? detail::commitlog_entry_serialization_format::variant : detail::commitlog_entry_serialization_format::mutation;
        const auto previous_format = std::exchange(_commitlog_format, new_format);
        if (previous_format != new_format || _size == std::numeric_limits<size_t>::max()) {
            compute_size();
        }
    }

    bool use_variant_commitlog_entry_format() const {
        return _commitlog_format == detail::commitlog_entry_serialization_format::variant;
    }

    schema_ptr schema() const {
        return _schema;
    }

    size_t size() const {
        SCYLLA_ASSERT(_size != std::numeric_limits<size_t>::max());
        return _size;
    }

    size_t mutation_size() const {
        return _mutation.representation().size();
    }

    force_sync sync() const {
        return _sync;
    }

    using ostream = typename seastar::memory_output_stream<detail::sector_split_iterator>;
    void write(ostream& out) const;
};


// Mutation entry reader for hints commitlog (reads raw mutation_entry format).
class commitlog_mutation_entry_reader {
    mutation_entry _me;
public:
    commitlog_mutation_entry_reader(const fragmented_temporary_buffer& buffer);
    const std::optional<column_mapping>& get_column_mapping() const { return _me.mapping(); }
    const frozen_mutation& mutation() const & { return _me.mutation(); }
    frozen_mutation&& mutation() && { return std::move(_me).mutation(); }
};

// Writer for Raft log entries to the database commit log using the commitlog_entry format.
class commitlog_raft_log_entry_writer {
public:
protected:
    raft_commit_log_entry _item;
    std::size_t _size = std::numeric_limits<std::size_t>::max();

    template <typename Output>
    void serialize(Output& out) const;
    void compute_size();
public:
    explicit commitlog_raft_log_entry_writer(raft_commit_log_entry item)
        : _item(std::move(item)) {
        compute_size();
    }

    size_t size() const {
        SCYLLA_ASSERT(_size != std::numeric_limits<size_t>::max());
        return _size;
    }

    using ostream = typename seastar::memory_output_stream<detail::sector_split_iterator>;
    void write(ostream& out) const;
    const raft_commit_log_entry& get_log_entry() const {
        return _item;
    }
};

class commitlog_entry_reader {
    commitlog_entry _entry;

public:
    explicit commitlog_entry_reader(const fragmented_temporary_buffer& buffer,
            detail::commitlog_entry_serialization_format format = detail::commitlog_entry_serialization_format::mutation);

    const commitlog_entry& entry() const& {
        return _entry;
    }
    commitlog_entry&& entry() && {
        return std::move(_entry);
    }
};

