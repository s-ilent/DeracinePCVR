// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/capture_artifacts.h"

#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

namespace Core::CaptureArtifacts {
namespace {

using json = nlohmann::json;

/// Reads a file into a parsed JSON document.
[[nodiscard]] bool LoadJson(const std::string& path, json& out, FieldProblem& problem,
                            std::string& error) {
    std::ifstream file{path};
    if (!file) {
        problem = FieldProblem::Unreadable;
        error = "cannot open " + path;
        return false;
    }
    try {
        file >> out;
    } catch (const std::exception& e) {
        problem = FieldProblem::Unreadable;
        error = std::string{"not valid JSON: "} + e.what();
        return false;
    }
    if (!out.is_object()) {
        problem = FieldProblem::Unreadable;
        error = "the artifact is not a JSON object";
        return false;
    }
    return true;
}

/// Rejects a document that repeats an identity-critical key.
///
/// nlohmann keeps the last value for a duplicate key, which would silently accept a document whose
/// identity is ambiguous. The raw text is scanned for the key at the top level of the object.
[[nodiscard]] bool HasDuplicateTopLevelKey(const std::string& path, std::string_view key) {
    std::ifstream file{path};
    if (!file) {
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
    const std::string needle = "\"" + std::string{key} + "\"";
    std::size_t count = 0;
    std::size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        ++count;
        at += needle.size();
    }
    return count > 1;
}

/// Reads an unsigned integer field, distinguishing every failure mode.
[[nodiscard]] bool ReadU64(const json& object, std::string_view key, u64& out,
                           FieldProblem& problem, std::string& error, bool required) {
    const auto it = object.find(std::string{key});
    if (it == object.end()) {
        if (required) {
            problem = FieldProblem::MissingField;
            error = "missing field \"" + std::string{key} + "\"";
            return false;
        }
        return true;
    }
    if (!it->is_number_unsigned() && !it->is_number_integer()) {
        problem = FieldProblem::WrongType;
        error = "field \"" + std::string{key} + "\" is not an integer";
        return false;
    }
    if (it->is_number_integer() && it->get<std::int64_t>() < 0) {
        problem = FieldProblem::MalformedNumber;
        error = "field \"" + std::string{key} + "\" is negative";
        return false;
    }
    if (it->is_number_float()) {
        problem = FieldProblem::MalformedNumber;
        error = "field \"" + std::string{key} + "\" is fractional";
        return false;
    }
    try {
        out = it->get<u64>();
    } catch (const std::exception& e) {
        problem = FieldProblem::MalformedNumber;
        error = "field \"" + std::string{key} + "\" is out of range: " + e.what();
        return false;
    }
    return true;
}

[[nodiscard]] bool ReadU32(const json& object, std::string_view key, u32& out,
                           FieldProblem& problem, std::string& error, bool required) {
    u64 wide = 0;
    if (!ReadU64(object, key, wide, problem, error, required)) {
        return false;
    }
    if (wide > 0xFFFFFFFFull) {
        problem = FieldProblem::MalformedNumber;
        error = "field \"" + std::string{key} + "\" exceeds 32 bits";
        return false;
    }
    out = static_cast<u32>(wide);
    return true;
}

[[nodiscard]] bool ReadBool(const json& object, std::string_view key, bool& out,
                            FieldProblem& problem, std::string& error) {
    const auto it = object.find(std::string{key});
    if (it == object.end()) {
        return true;
    }
    if (!it->is_boolean()) {
        problem = FieldProblem::WrongType;
        error = "field \"" + std::string{key} + "\" is not a boolean";
        return false;
    }
    out = it->get<bool>();
    return true;
}

[[nodiscard]] bool ReadString(const json& object, std::string_view key, std::string& out,
                              FieldProblem& problem, std::string& error) {
    const auto it = object.find(std::string{key});
    if (it == object.end()) {
        return true;
    }
    if (!it->is_string()) {
        problem = FieldProblem::WrongType;
        error = "field \"" + std::string{key} + "\" is not a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

} // namespace

std::string_view FieldProblemName(FieldProblem problem) noexcept {
    switch (problem) {
    case FieldProblem::None:
        return "none";
    case FieldProblem::Unreadable:
        return "unreadable";
    case FieldProblem::MissingObject:
        return "missing_object";
    case FieldProblem::MissingField:
        return "missing_field";
    case FieldProblem::WrongType:
        return "wrong_type";
    case FieldProblem::MalformedNumber:
        return "malformed_number";
    case FieldProblem::DuplicateKey:
        return "duplicate_key";
    case FieldProblem::SchemaMismatch:
        return "schema_mismatch";
    default:
        return "unknown";
    }
}

bool ReadPackageMetadata(const std::string& path, PackageMetadata& out, FieldProblem& problem,
                         std::string& error) {
    problem = FieldProblem::None;
    error.clear();
    out = PackageMetadata{};

    json doc;
    if (!LoadJson(path, doc, problem, error)) {
        return false;
    }

    // A duplicate identity-critical key makes the document ambiguous, so it is refused rather than
    // resolved by position.
    for (const std::string_view key : {"provenance_id", "session_id", "index_binding"}) {
        if (HasDuplicateTopLevelKey(path, key)) {
            problem = FieldProblem::DuplicateKey;
            error = "the package repeats the key \"" + std::string{key} + "\"";
            return false;
        }
    }

    if (!ReadU32(doc, "schema_version", out.schema_version, problem, error, true)) {
        return false;
    }
    if (!ReadU64(doc, "provenance_id", out.provenance_id, problem, error, true)) {
        return false;
    }
    out.has_provenance_id = true;
    if (!ReadU64(doc, "session_id", out.session_id, problem, error, false)) {
        return false;
    }
    out.has_session_id = doc.contains("session_id");

    // The index binding is read from its own object. A vertex binding is never consulted, because
    // the two carry a field with the same name and the first match in the file is a vertex address.
    const auto index_it = doc.find("index_binding");
    if (index_it == doc.end() || index_it->is_null()) {
        out.index.present = false;
    } else {
        if (!index_it->is_object()) {
            problem = FieldProblem::WrongType;
            error = "index_binding is not an object";
            return false;
        }
        out.index.present = true;
        const auto& index = *index_it;
        if (!ReadU64(index, "guest_base", out.index.guest_base, problem, error, true)) {
            return false;
        }
        out.index.has_guest_base = true;
        if (!ReadU64(index, "guest_size", out.index.guest_size, problem, error, false)) {
            return false;
        }
        out.index.has_guest_size = index.contains("guest_size");
        if (!ReadU32(index, "index_size", out.index.index_size, problem, error, false)) {
            return false;
        }
        out.index.has_index_size = index.contains("index_size");
        if (!ReadU32(index, "index_count", out.index.index_count, problem, error, false)) {
            return false;
        }
        out.index.has_index_count = index.contains("index_count");
    }

    // The first vertex binding is read only so a test can prove it is not confused with the index
    // address. It is never used for identity.
    const auto vertex_it = doc.find("vertex_bindings");
    if (vertex_it != doc.end() && vertex_it->is_array() && !vertex_it->empty() &&
        (*vertex_it)[0].is_object()) {
        if (!ReadU64((*vertex_it)[0], "guest_base", out.first_vertex_guest_base, problem, error,
                     false)) {
            return false;
        }
        out.has_first_vertex_guest_base = (*vertex_it)[0].contains("guest_base");
    }

    return true;
}

bool ReadBridgeRecords(const std::string& path, std::vector<BridgeRecord>& out,
                       FieldProblem& problem, std::string& error) {
    problem = FieldProblem::None;
    error.clear();
    out.clear();

    json doc;
    if (!LoadJson(path, doc, problem, error)) {
        return false;
    }

    const auto records_it = doc.find("records");
    if (records_it == doc.end()) {
        problem = FieldProblem::MissingField;
        error = "the bridge sidecar has no records array";
        return false;
    }
    if (!records_it->is_array()) {
        problem = FieldProblem::WrongType;
        error = "the bridge records field is not an array";
        return false;
    }

    for (const auto& entry : *records_it) {
        if (!entry.is_object()) {
            problem = FieldProblem::WrongType;
            error = "a bridge record is not an object";
            return false;
        }
        BridgeRecord record{};
        if (!ReadU64(entry, "provenance_id", record.provenance_id, problem, error, true)) {
            return false;
        }
        if (!ReadU64(entry, "session_id", record.session_id, problem, error, false)) {
            return false;
        }
        record.has_session_id = entry.contains("session_id");
        if (!ReadU64(entry, "execution_id", record.execution_id, problem, error, false)) {
            return false;
        }
        if (!ReadU64(entry, "original_rhi_object", record.original_rhi_object, problem, error,
                     false)) {
            return false;
        }
        record.has_original_rhi_object = entry.contains("original_rhi_object");
        if (!ReadU64(entry, "selected_guest_address", record.selected_guest_address, problem,
                     error, false)) {
            return false;
        }
        record.has_selected_guest_address = entry.contains("selected_guest_address");
        if (!ReadU32(entry, "index_element_width", record.index_element_width, problem, error,
                     false)) {
            return false;
        }
        record.has_index_element_width = entry.contains("index_element_width");
        if (!ReadU64(entry, "allocation_size", record.allocation_size, problem, error, false)) {
            return false;
        }
        record.has_allocation_size = entry.contains("allocation_size");
        if (!ReadU64(entry, "allocation_offset", record.allocation_offset, problem, error,
                     false)) {
            return false;
        }
        record.has_allocation_offset = entry.contains("allocation_offset");
        if (!ReadU64(entry, "resource_generation", record.resource_generation, problem, error,
                     false)) {
            return false;
        }
        record.has_resource_generation = entry.contains("resource_generation");
        if (!ReadBool(entry, "predicate_determinable", record.predicate_determinable, problem,
                      error)) {
            return false;
        }
        if (!ReadString(entry, "selected_slot", record.selected_slot, problem, error)) {
            return false;
        }
        if (!ReadString(entry, "address_evidence", record.address_evidence, problem, error)) {
            return false;
        }
        out.push_back(std::move(record));
    }

    return true;
}

bool SelectBridgeRecord(const std::vector<BridgeRecord>& records, u64 session_id,
                        u64 provenance_id, BridgeRecord& out, bool& ambiguous,
                        std::string& error) {
    ambiguous = false;
    error.clear();
    bool found = false;
    for (const auto& record : records) {
        if (record.provenance_id != provenance_id) {
            continue;
        }
        // A record from another session is not this draw, even if the provenance id agrees.
        if (session_id != 0 && record.has_session_id && record.session_id != session_id) {
            continue;
        }
        if (found) {
            ambiguous = true;
            error = "more than one bridge record matches the selected session and provenance";
            return false;
        }
        out = record;
        found = true;
    }
    if (!found) {
        error = "no bridge record matches the selected session and provenance";
    }
    return found;
}

} // namespace Core::CaptureArtifacts
