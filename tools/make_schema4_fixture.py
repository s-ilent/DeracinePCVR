#!/usr/bin/env python3
"""Builds a schema-4 test package for the unified reader.

This is a fixture, not a capture. It exists so the reader's schema-4 path, sidecar length checks,
SHA-256 verification and descriptor-interface parsing can be exercised without a game run. Every
field is authored here, and the package says so in its scene name.

The shader modules are the committed SC-11R test shaders, so the effective bytes are real SPIR-V.

Usage: python make_schema4_fixture.py <output-dir>
"""

import hashlib
import json
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SHADER_DIR = os.path.join(HERE, 'geometry_replay', 'test_shaders')


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    out = argv[0]
    os.makedirs(out, exist_ok=True)

    # --- Shader stages -------------------------------------------------------------------------
    stages = []
    for logical, hardware, name in ((0, 0, 'sc11r_test.vert.spv'), (4, 1, 'sc11r_test.frag.spv')):
        path = os.path.join(SHADER_DIR, name)
        with open(path, 'rb') as handle:
            data = handle.read()
        sidecar = f'stage_{logical}.spv'
        with open(os.path.join(out, sidecar), 'wb') as handle:
            handle.write(data)
        stages.append({
            'logical_stage': logical,
            'hardware_stage': hardware,
            'program_hash': 0x1234567890ABCDEF + logical,
            'permutation_index': 0,
            'permutation_hash': 0,
            'patched': False,
            'effective_sha256': sha256_hex(data),
            'generated_sha256': sha256_hex(data),
            'entry_point': 'main',
            'effective_byte_length': len(data),
            'gcn_byte_length': 0,
            'effective_file': sidecar,
            'gcn_file': '',
        })

    # The permutation hash is HashCombine(program, index), recomputed here so the fixture carries a
    # value the validator can check rather than one it must trust.
    def hash_combine(seed, value):
        return (seed ^ (value + 0x9E3779B9 + (seed << 12) + (seed >> 4))) & 0xFFFFFFFFFFFFFFFF

    for stage in stages:
        stage['permutation_hash'] = hash_combine(stage['program_hash'],
                                                 stage['permutation_index'])

    # --- Descriptor interfaces -----------------------------------------------------------------
    interfaces = []
    for stage in stages:
        interfaces.append({
            'program_hash': stage['program_hash'],
            'permutation_index': stage['permutation_index'],
            'permutation_hash': stage['permutation_hash'],
            'hardware_stage': stage['hardware_stage'],
            'logical_stage': stage['logical_stage'],
            'patched': stage['patched'],
            'push_constant_offset': 0,
            'push_constant_size': 16,
            'push_constant_stage_flags': 1 if stage['logical_stage'] == 0 else 16,
            'unified_binding_count': 1,
            'descriptors': [{
                'set': 0,
                'binding': 0,
                'descriptor_class': 0,
                'count': 1,
                'stage_flags': 1 if stage['logical_stage'] == 0 else 16,
                'guest_resource_index': 0xFFFFFFFF,
                'written': True,
            }],
        })

    # --- Reference image -----------------------------------------------------------------------
    width, height, bpp = 64, 64, 4
    post = bytes([(x * 4) % 256 for x in range(width * height * bpp)])
    with open(os.path.join(out, 'reference_post.bin'), 'wb') as handle:
        handle.write(post)

    package = {
        'schema_version': 4,
        'scene': 'synthetic-schema4-fixture',
        'provenance_id': 1,
        'session_id': 0x1111111111111111,
        'execution_id': 0x2222222222222222,
        'resources': [],
        'shader_buffers': [],
        'vertex_bindings': [],
        'index_binding': {
            'index_size': 2,
            'index_count': 0,
            'first_index': 0,
            'base_vertex': 0,
            'instance_count': 0,
            'first_instance': 0,
            'resource_ref': 0,
            'guest_base': 0,
            'guest_size': 0,
        },
        'textures': [],
        'samplers': [],
        'shader_stages': stages,
        'shader_interfaces': interfaces,
        'pipeline_state': {
            'has_topology': True,
            'topology': 3,
            'cull_mode': 0,
            'front_face': 0,
            'polygon_mode': 0,
            'depth_test': False,
            'depth_write': False,
            'depth_compare_op': 0,
            'color_formats': [37],
            'depth_format': 0,
            'samples': 1,
            'blend_enable': [0],
            'write_mask': [15],
            'viewport_x': 0.0,
            'viewport_y': 0.0,
            'viewport_width': 64.0,
            'viewport_height': 64.0,
            'viewport_min_depth': 0.0,
            'viewport_max_depth': 1.0,
            'push_constant_offset': 0,
            'push_constant_size': 16,
            'push_constant_bytes': [0] * 16,
        },
        'reference_output': {
            'width': width,
            'height': height,
            'format': 37,
            'bytes_per_pixel': bpp,
            'pre_sha256': '',
            'post_sha256': sha256_hex(post),
            'pre_file': '',
            'post_file': 'reference_post.bin',
            'has_pre': False,
        },
        'readiness': {
            'geometry_ready': False,
            'texture_binding_ready': False,
            'texture_contents_ready': False,
            'sampler_ready': False,
            'diagnostic_textured_replay_ready': False,
            'shader_binaries_ready': True,
            'shader_bindings_ready': True,
            'shader_parameters_ready': True,
            'pipeline_state_ready_original': True,
            'reference_output_ready': True,
            'original_shader_replay_ready': False,
        },
    }

    # An empty geometry blob. The fixture has no geometry, and the reader requires the sidecar to
    # exist rather than treating its absence as an empty resource set.
    with open(os.path.join(out, 'resources.bin'), 'wb') as handle:
        handle.write(b'')

    with open(os.path.join(out, 'package.json'), 'w', encoding='utf-8') as handle:
        json.dump(package, handle, indent=1)

    print(f'wrote {out}')
    print(f'  schema 4, {len(stages)} stages, {len(interfaces)} interfaces')
    print(f'  reference {width}x{height} {bpp} bpp, sha256 {sha256_hex(post)[:16]}...')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
