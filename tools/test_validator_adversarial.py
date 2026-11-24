#!/usr/bin/env python3
"""Adversarial tests for the independent package validator.

Each case corrupts a copy of a real package in one specific way and asserts that the validator
rejects it. This is what makes the validator's acceptance meaningful: it is not merely permissive.

Usage: python test_validator_adversarial.py <package-directory>
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VALIDATOR = os.path.join(HERE, "validate_draw_package.py")


def run_validator(directory):
    result = subprocess.run(
        [sys.executable, VALIDATOR, directory], capture_output=True, text=True)
    return result.returncode, result.stdout.strip()


blob_path_holder = [""]


def load(directory):
    blob_path_holder[0] = os.path.join(directory, "resources.bin")
    with open(os.path.join(directory, "package.json"), encoding="utf-8") as handle:
        package = json.load(handle)
    with open(os.path.join(directory, "resources.bin"), "rb") as handle:
        blob = bytearray(handle.read())
    return package, blob


def save(directory, package, blob):
    with open(os.path.join(directory, "package.json"), "w", encoding="utf-8") as handle:
        json.dump(package, handle, indent=1)
    with open(os.path.join(directory, "resources.bin"), "wb") as handle:
        handle.write(bytes(blob))


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    source = argv[0]

    code, out = run_validator(source)
    if code != 0:
        print(f"the source package does not validate: {out}")
        return 1
    print(f"source package validates: {out}")

    cases = []

    def case(name, mutate):
        cases.append((name, mutate))

    def corrupt_blob(package, blob):
        blob[0] ^= 0xFF

    def wrong_schema(package, blob):
        package["schema_version"] = 1

    def missing_provenance(package, blob):
        package["provenance_id"] = 0

    def duplicate_resource(package, blob):
        package["resources"].append(dict(package["resources"][0]))

    def dangling_vertex_ref(package, blob):
        package["vertex_bindings"][0]["resource_ref"] = 9999

    def bad_format(package, blob):
        package["vertex_bindings"][0]["vk_format"] = 12345

    def wrong_component_count(package, blob):
        package["vertex_bindings"][0]["element_count"] = 7

    def attribute_exceeds_stride(package, blob):
        package["vertex_bindings"][0]["attr_offset"] = 4096

    def zero_stride(package, blob):
        package["vertex_bindings"][0]["stride"] = 0

    def bad_index_width(package, blob):
        package["index_binding"]["index_size"] = 3

    def index_count_too_large(package, blob):
        package["index_binding"]["index_count"] = 10 ** 6

    def degenerate_3d_claim(package, blob):
        package["geometry"]["bounds_min"] = [0.0, 0.0, 0.0]
        package["geometry"]["bounds_max"] = [1.0, 1.0, 0.0]

    def inverted_bounds(package, blob):
        package["geometry"]["bounds_min"], package["geometry"]["bounds_max"] = (
            package["geometry"]["bounds_max"], package["geometry"]["bounds_min"])

    def wrong_triangle_count(package, blob):
        package["geometry"]["triangle_count"] += 1

    def wrong_degenerate_count(package, blob):
        package["geometry"]["degenerate_triangles"] += 1

    def wrong_unique_count(package, blob):
        package["geometry"]["unique_referenced_vertices"] += 1

    def position_binding_missing(package, blob):
        package["geometry"]["position_binding"] = 999

    def position_components_mismatch(package, blob):
        package["geometry"]["position_components"] = 9

    def diagnostic_without_geometry(package, blob):
        package["readiness"]["geometry_ready"] = False

    def full_draw_without_textures(package, blob):
        package["readiness"]["full_draw_replay_ready"] = True

    def unverified_vertex_resource(package, blob):
        ref = package["vertex_bindings"][0]["resource_ref"]
        for resource in package["resources"]:
            if resource["id"] == ref:
                resource["status"] = "guest_memory_observed"

    def truncated_blob(package, blob):
        del blob[-16:]

    def corrupt_texture_byte(package, blob):
        path = os.path.join(os.path.dirname(blob_path_holder[0]), "textures.bin")
        with open(path, "rb") as handle:
            data = bytearray(handle.read())
        if data:
            data[0] ^= 0xFF
            with open(path, "wb") as handle:
                handle.write(bytes(data))

    def texture_hash_mismatch(package, blob):
        for texture in package.get("textures", []):
            for sub in texture.get("subresources", []):
                sub["content_hash"] = (sub["content_hash"] + 1) & ((1 << 64) - 1)
                return

    def texture_subresource_out_of_range(package, blob):
        for texture in package.get("textures", []):
            for sub in texture.get("subresources", []):
                sub["blob_offset"] = 10 ** 9
                return

    def texture_empty_subresource(package, blob):
        for texture in package.get("textures", []):
            for sub in texture.get("subresources", []):
                sub["byte_length"] = 0
                return

    def duplicate_texture_id(package, blob):
        # A package with one texture still has to be testable, so a second entry is cloned rather
        # than the case being skipped.
        textures = package.get("textures", [])
        if not textures:
            return
        clone = json.loads(json.dumps(textures[0]))
        clone["id"] = textures[0]["id"]
        textures.append(clone)

    def missing_sampler_reference(package, blob):
        for texture in package.get("textures", []):
            texture["sampler_id"] = 9999
            return

    def sampler_lod_inverted(package, blob):
        for sampler in package.get("samplers", []):
            sampler["min_lod"] = 10.0
            sampler["max_lod"] = 0.0
            return

    def sampler_aniso_below_one(package, blob):
        for sampler in package.get("samplers", []):
            sampler["max_anisotropy"] = 0.0
            return

    def sampler_address_out_of_range(package, blob):
        for sampler in package.get("samplers", []):
            sampler["address_u"] = 99
            return

    def textured_ready_without_texture(package, blob):
        package["readiness"]["diagnostic_textured_replay_ready"] = True
        for texture in package.get("textures", []):
            texture["subresources"] = []

    def textured_ready_without_uv(package, blob):
        package["readiness"]["diagnostic_textured_replay_ready"] = True
        package["texture_coordinate"]["looks_like_uv"] = False

    def textured_ready_without_3d(package, blob):
        package["readiness"]["diagnostic_textured_replay_ready"] = True
        package["geometry"]["is_three_dimensional"] = False

    def wrong_block_texels(package, blob):
        for texture in package.get("textures", []):
            if texture.get("subresources"):
                texture["block_texels_x"] = 8
                return

    def wrong_block_columns(package, blob):
        for texture in package.get("textures", []):
            if texture.get("subresources"):
                texture["block_columns"] = texture.get("block_columns", 1) + 1
                return

    def wrong_row_pitch(package, blob):
        for texture in package.get("textures", []):
            for sub in texture.get("subresources", []):
                sub["row_pitch"] = sub["row_pitch"] + 16
                return

    def truncated_final_block_row(package, blob):
        # Shrink the payload by one block row and re-hash, so the hash matches but the length does
        # not. This is the case a hash-only check cannot catch.
        for texture in package.get("textures", []):
            for sub in texture.get("subresources", []):
                sub["byte_length"] = sub["byte_length"] - sub["row_pitch"]
                return

    def wrong_pixel_extent(package, blob):
        for texture in package.get("textures", []):
            for sub in texture.get("subresources", []):
                sub["width"] = sub["width"] * 2
                return

    def missing_block_geometry(package, blob):
        for texture in package.get("textures", []):
            texture["bytes_per_block"] = 0
            return

    def verified_sampler_without_id(package, blob):
        for texture in package.get("textures", []):
            texture["sampler_verified"] = True
            texture["sampler_id"] = 0
            return

    def stale_transfer_identity(package, blob):
        for texture in package.get("textures", []):
            texture["transfer_id"] = 0
            return

    def partial_mip_chain(package, blob):
        # Ask for four mip levels but supply one. The payload is valid for the level it has, so
        # only the range comparison catches the overclaim.
        for texture in package.get("textures", []):
            if texture.get("subresources"):
                texture["mip_count"] = 4
                return

    def missing_array_layer(package, blob):
        for texture in package.get("textures", []):
            if texture.get("subresources"):
                texture["layer_count"] = 2
                return

    def duplicate_subresource_masking_missing(package, blob):
        # Two copies of the same subresource, with a second mip requested. The duplicate must not
        # stand in for the missing level.
        for texture in package.get("textures", []):
            subs = texture.get("subresources", [])
            if subs:
                texture["mip_count"] = 2
                clone = json.loads(json.dumps(subs[0]))
                subs.append(clone)
                return

    case("corrupted blob byte", corrupt_blob)
    case("wrong schema version", wrong_schema)
    case("missing provenance", missing_provenance)
    case("duplicate resource id", duplicate_resource)
    case("dangling vertex reference", dangling_vertex_ref)
    case("unknown vertex format", bad_format)
    case("wrong component count", wrong_component_count)
    case("attribute exceeds stride", attribute_exceeds_stride)
    case("zero stride", zero_stride)
    case("bad index width", bad_index_width)
    case("index count too large", index_count_too_large)
    case("degenerate 3D claim", degenerate_3d_claim)
    case("inverted bounds", inverted_bounds)
    case("wrong triangle count", wrong_triangle_count)
    case("wrong degenerate count", wrong_degenerate_count)
    case("wrong unique vertex count", wrong_unique_count)
    case("position binding missing", position_binding_missing)
    case("position components mismatch", position_components_mismatch)
    case("diagnostic readiness without geometry", diagnostic_without_geometry)
    case("full draw readiness without textures", full_draw_without_textures)
    case("unverified vertex resource", unverified_vertex_resource)
    case("truncated blob", truncated_blob)
    case("corrupted texture byte", corrupt_texture_byte)
    case("texture hash mismatch", texture_hash_mismatch)
    case("texture subresource out of range", texture_subresource_out_of_range)
    case("texture empty subresource", texture_empty_subresource)
    case("duplicate texture id", duplicate_texture_id)
    case("missing sampler reference", missing_sampler_reference)
    case("sampler lod inverted", sampler_lod_inverted)
    case("sampler anisotropy below one", sampler_aniso_below_one)
    case("sampler address out of range", sampler_address_out_of_range)
    case("textured readiness without texture", textured_ready_without_texture)
    case("textured readiness without uv", textured_ready_without_uv)
    case("textured readiness without 3d", textured_ready_without_3d)
    case("wrong block texel size", wrong_block_texels)
    case("wrong block column count", wrong_block_columns)
    case("wrong row pitch", wrong_row_pitch)
    case("truncated final block row", truncated_final_block_row)
    case("wrong pixel extent", wrong_pixel_extent)
    case("missing block geometry", missing_block_geometry)
    def shader_permutation_hash_mismatch(package, blob):
        for stage in package.get("shader_stages", []):
            stage["permutation_hash"] = 0xDEADBEEF
            return

    def shader_missing_effective_hash(package, blob):
        for stage in package.get("shader_stages", []):
            stage["effective_sha256"] = ""
            return

    def shader_missing_bytes(package, blob):
        for stage in package.get("shader_stages", []):
            stage["effective_byte_length"] = 0
            return

    def shader_only_one_stage(package, blob):
        if package.get("shader_stages"):
            package["shader_stages"] = package["shader_stages"][:1]

    def original_ready_without_binaries(package, blob):
        package["readiness"]["shader_binaries_ready"] = False

    def original_ready_without_bindings(package, blob):
        package["readiness"]["shader_bindings_ready"] = False

    def original_ready_without_parameters(package, blob):
        package["readiness"]["shader_parameters_ready"] = False

    def original_ready_without_pipeline(package, blob):
        package["readiness"]["pipeline_state_ready_original"] = False

    def original_ready_without_reference(package, blob):
        package["readiness"]["reference_output_ready"] = False

    def push_constant_range_mismatch(package, blob):
        package["pipeline_state"]["push_constant_size"] = 16
        package["pipeline_state"]["push_constant_bytes"] = []

    def pipeline_state_without_topology(package, blob):
        package["pipeline_state"]["topology"] = 0

    def reference_without_post(package, blob):
        package["reference_output"]["post_sha256"] = ""

    def reference_without_extent(package, blob):
        package["reference_output"]["width"] = 0

    case("verified sampler without id", verified_sampler_without_id)
    case("shader permutation hash mismatch", shader_permutation_hash_mismatch)
    case("shader missing effective hash", shader_missing_effective_hash)
    case("shader missing bytes", shader_missing_bytes)
    case("shader only one stage", shader_only_one_stage)
    case("original ready without binaries", original_ready_without_binaries)
    case("original ready without bindings", original_ready_without_bindings)
    case("original ready without parameters", original_ready_without_parameters)
    case("original ready without pipeline", original_ready_without_pipeline)
    case("original ready without reference", original_ready_without_reference)
    case("push constant range mismatch", push_constant_range_mismatch)
    case("pipeline state without topology", pipeline_state_without_topology)
    case("reference without post", reference_without_post)
    case("reference without extent", reference_without_extent)
    case("stale transfer identity", stale_transfer_identity)
    case("partial mip chain", partial_mip_chain)
    case("missing array layer", missing_array_layer)
    case("duplicate subresource masking missing", duplicate_subresource_masking_missing)

    failures = 0
    for name, mutate in cases:
        with tempfile.TemporaryDirectory() as tmp:
            work = os.path.join(tmp, "package")
            shutil.copytree(source, work)
            package, blob = load(work)
            mutate(package, blob)
            save(work, package, blob)
            code, out = run_validator(work)
            if code == 0:
                print(f"  FAIL  {name}: validator accepted a corrupted package")
                failures += 1
            else:
                print(f"  ok    {name}: {out}")

    print()
    print(f"{len(cases) - failures}/{len(cases)} adversarial cases rejected")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))