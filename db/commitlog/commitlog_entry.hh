/*
 * Copyright 2016-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */
#pragma once
#include <optional>
#include "mutation/frozen_mutation.hh"
#include "schema/schema.hh"

class commitlog_entry {
    std::optional<column_mapping> _mapping;
    frozen_mutation _mutation;
public:
    commitlog_entry(std::optional<column_mapping> mapping, frozen_mutation&& mutation)
        : _mapping(std::move(mapping)), _mutation(std::move(mutation)) { }
    const std::optional<column_mapping>& mapping() const { return _mapping; }
    const frozen_mutation& mutation() const & { return _mutation; }
    frozen_mutation&& mutation() && { return std::move(_mutation); }
};
