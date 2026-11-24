// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "index_bridge_reader.h"

#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

namespace bridge {

std::string_view ProblemName(Problem problem) {
    switch (problem) {
    case Problem::None:
        return "none";
    case Problem::Missing:
        return "missing";
    case Problem::Unreadable:
        return "unreadable";
    case Problem::BadJson:
        return "bad_json";
    case Problem::SchemaMismatch:
        return "schema_mismatch";
    case Problem::NoRecords:
        return "no_records";
    case Problem::DuplicateRecord:
        return "duplicate_record";
    case Problem::TruncatedRecord:
        return "truncated_record";
    default:
        return "unknown";
    }
}

bool Read(const std::string& directory, std::vector<Record>& out, Problem& problem,
          std::string& error) {
    out.clear();
    // A failure must not leave a partial record set behind: a caller that ignores the return value
    // would otherwise consume records from a sidecar the reader rejected.
    problem = Problem::None;
    error.clear();

    const auto path = std::filesystem::path{directory} / kFileName;
    if (!std::filesystem::exists(path)) {
        problem = Problem::Missing;
        error = "no index bridge sidecar in the capture directory";
        return false;
    }

    std::ifstream file{path};
    if (!file) {
        problem = Problem::Unreadable;
        error = "cannot open the index bridge sidecar";
        return false;
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (const std::exception& e) {
        problem = Problem::BadJson;
        error = std::string{"the index bridge sidecar is not valid JSON: "} + e.what();
        return false;
    }

    try {
        const u32 version = json.at("schema_version").get<u32>();
        if (version != kSchemaVersion) {
            problem = Problem::SchemaMismatch;
            error = "the index bridge sidecar schema is not " + std::to_string(kSchemaVersion);
            return false;
        }

        const auto& records = json.at("records");
        if (records.empty()) {
            problem = Problem::NoRecords;
            error = "the index bridge sidecar carries no records";
            return false;
        }

        std::set<u64> seen;
        for (const auto& entry : records) {
            Record record{};
            record.provenance_id = entry.at("provenance_id").get<u64>();
            if (record.provenance_id == 0) {
                problem = Problem::TruncatedRecord;
                error = "a bridge record has no provenance id";
                return false;
            }
            // A duplicate provenance id means the sidecar cannot say which record is the draw.
            if (!seen.insert(record.provenance_id).second) {
                problem = Problem::DuplicateRecord;
                error = "the sidecar carries more than one record for provenance " +
                        std::to_string(record.provenance_id);
                out.clear();
                return false;
            }

            record.session_id = entry.value("session_id", 0ull);
            record.has_session_id = entry.value("has_session_id", false);
            record.execution_id = entry.value("execution_id", 0ull);
            record.has_execution_id = entry.value("has_execution_id", false);
            record.original_rhi_object = entry.value("original_rhi_object", 0ull);
            record.has_original_rhi_object = entry.value("has_original_rhi_object", false);
            record.resource_base = entry.value("resource_base", 0ull);
            record.has_resource_base = entry.value("has_resource_base", false);
            record.address_candidate_a = entry.value("address_candidate_a", 0ull);
            record.has_address_candidate_a = entry.value("has_address_candidate_a", false);
            record.address_candidate_b = entry.value("address_candidate_b", 0ull);
            record.has_address_candidate_b = entry.value("has_address_candidate_b", false);
            record.selected_slot = entry.value("selected_slot", "unknown");
            record.selected_guest_address = entry.value("selected_guest_address", 0ull);
            record.has_selected_guest_address =
                entry.value("has_selected_guest_address", false);
            record.index_element_width = entry.value("index_element_width", 0u);
            record.has_index_element_width = entry.value("has_index_element_width", false);
            record.allocation_size = entry.value("allocation_size", 0ull);
            record.has_allocation_size = entry.value("has_allocation_size", false);
            record.allocation_offset = entry.value("allocation_offset", 0ull);
            record.has_allocation_offset = entry.value("has_allocation_offset", false);
            record.resource_generation = entry.value("resource_generation", 0ull);
            record.has_resource_generation = entry.value("has_resource_generation", false);
            record.predicate_determinable = entry.value("predicate_determinable", false);
            record.address_evidence = entry.value("address_evidence", "none");
            out.push_back(std::move(record));
        }
    } catch (const std::exception& e) {
        problem = Problem::TruncatedRecord;
        error = std::string{"a bridge record is missing a required field: "} + e.what();
        out.clear();
        return false;
    }

    return true;
}

bool FindByProvenance(const std::vector<Record>& records, u64 provenance_id, Record& out,
                      bool& ambiguous) {
    ambiguous = false;
    bool found = false;
    for (const auto& record : records) {
        if (record.provenance_id != provenance_id) {
            continue;
        }
        if (found) {
            ambiguous = true;
            return false;
        }
        out = record;
        found = true;
    }
    return found;
}

} // namespace bridge