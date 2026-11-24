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
    # The reference must match the replay render extent, or the comparison is a configuration
    # mismatch rather than a rendering one.
    width, height, bpp = 640, 640, 4
    post = bytes([(x * 4) % 256 for x in range(width * height * bpp)])
    with open(os.path.join(out, 'reference_post.bin'), 'wb') as handle:
        handle.write(post)

    # The uniform block the vertex module declares: an identity MVP and a tint. The bytes are
    # authored here, and the fixture says so.
    uniform = bytearray(80)
    identity = [1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0,
                0.0, 0.0, 0.0, 1.0]
    struct.pack_into('<16f', uniform, 0, *identity)
    struct.pack_into('<4f', uniform, 64, 1.0, 0.5, 0.25, 1.0)

    # A quad covering the viewport, with UVs, and the two triangles that draw it. The vertex layout
    # is position then UV, which is what the replay's textured path expects.
    quad = [
        -1.0, -1.0, 0.0, 0.0, 1.0,
         1.0, -1.0, 0.0, 1.0, 1.0,
         1.0,  1.0, 0.0, 1.0, 0.0,
        -1.0,  1.0, 0.0, 0.0, 0.0,
    ]
    vertex_bytes = struct.pack('<20f', *quad)
    index_bytes = struct.pack('<6H', 0, 1, 2, 2, 3, 0)

    # A 4x4 texture with a known pattern, so a wrong sample is visible.
    tex_size = 4
    texels = bytearray(tex_size * tex_size * 4)
    for y in range(tex_size):
        for x in range(tex_size):
            i = (y * tex_size + x) * 4
            texels[i + 0] = (x * 64) % 256
            texels[i + 1] = (y * 64) % 256
            texels[i + 2] = 128
            texels[i + 3] = 255

    blob = bytes(uniform) + vertex_bytes + index_bytes
    uniform_offset = 0
    vertex_offset = len(uniform)
    index_offset = vertex_offset + len(vertex_bytes)

    with open(os.path.join(out, 'resources.bin'), 'wb') as handle:
        handle.write(blob)
    with open(os.path.join(out, 'textures.bin'), 'wb') as handle:
        handle.write(bytes(texels))

    package = {
        'schema_version': 4,
        'scene': 'synthetic-schema4-fixture',
        'provenance_id': 1,
        'session_id': 0x1111111111111111,
        'execution_id': 0x2222222222222222,
        'resources': [
            {'id': 1, 'clamped_size': 80, 'content_hash': 0,
             'blob_offset': uniform_offset, 'byte_length': 80, 'status': 'host_captured'},
            {'id': 2, 'clamped_size': len(vertex_bytes), 'content_hash': 0,
             'blob_offset': vertex_offset, 'byte_length': len(vertex_bytes),
             'status': 'host_captured'},
            {'id': 3, 'clamped_size': len(index_bytes), 'content_hash': 0,
             'blob_offset': index_offset, 'byte_length': len(index_bytes),
             'status': 'host_captured'},
        ],
        'shader_buffers': [],
        'bindings': [{
            'stage': 0,
            'resource_index': 0,
            'sharp_idx': 0,
            'vk_binding': 0,
            'vk_offset': 0,
            'vk_range': 80,
            'buffer_id': 1,
            'resource_ref': 1,
            'status': 'host_captured',
        }],
        'vertex_bindings': [{
            'binding': 0, 'stride': 20, 'resource_ref': 2, 'semantic': 0,
            'vk_format': 106, 'attr_offset': 0, 'element_count': 3,
        }],
        'index_binding': {
            'index_size': 2,
            'index_count': 6,
            'first_index': 0,
            'base_vertex': 0,
            'instance_count': 1,
            'first_instance': 0,
            'resource_ref': 3,
            'guest_base': 0,
            'guest_size': len(index_bytes),
        },
        'geometry': {
            'position_binding': 0,
            'position_components': 3,
            'is_three_dimensional': False,
            'bounds_min': [-1.0, -1.0, 0.0],
            'bounds_max': [1.0, 1.0, 0.0],
        },
        'textures': [{
            'id': 1,
            'vk_binding': 1,
            'image_format': 37,
            'view_format': 37,
            'width': tex_size,
            'height': tex_size,
            'block_columns': tex_size,
            'block_rows': tex_size,
            'block_texels_x': 1,
            'block_texels_y': 1,
            'bytes_per_block': 4,
            'transfer_id': 1,
            'sampler_id': 1,
            'sampler_verified': True,
            'subresources': [{
                'mip': 0, 'layer': 0, 'width': tex_size, 'height': tex_size,
                'row_pitch': tex_size * 4, 'blob_offset': 0,
                'byte_length': len(texels), 'content_hash': 0,
            }],
        }],
        'samplers': [{
            'id': 1, 'mag_filter': 0, 'min_filter': 0, 'mip_filter': 0,
            'address_u': 2, 'address_v': 2, 'address_w': 2,
            'lod_bias': 0.0, 'min_lod': 0.0, 'max_lod': 0.0,
            'anisotropy_enable': False, 'max_anisotropy': 1.0,
            'compare_enable': False, 'compare_op': 0, 'border_color': 0,
        }],
        'texture_coordinate': {
            'binding': 0, 'component_u': 0, 'component_v': 1, 'looks_like_uv': True,
        },
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
            'geometry_ready': True,
            'texture_binding_ready': True,
            'texture_contents_ready': True,
            'sampler_ready': True,
            'diagnostic_textured_replay_ready': True,
            'shader_binaries_ready': True,
            'shader_bindings_ready': True,
            'shader_parameters_ready': True,
            'pipeline_state_ready_original': True,
            'reference_output_ready': True,
            'original_shader_replay_ready': False,
        },
    }

    with open(os.path.join(out, 'package.json'), 'w', encoding='utf-8') as handle:
        json.dump(package, handle, indent=1)

    # A semantic tape beside the package, so the unified replay can be driven end to end. The draw
    # is authored here: this is a fixture, and the tape says so in its scene name.
    tape = {
        'schema_version': 1,
        'scene': 'synthetic-schema4-fixture',
        'provenance_id': package['provenance_id'],
        'image_sha256': '',
        'module_base': 0,
        'submission_id': 0,
        'packet_offset': 0,
        'packet_offset_known': False,
        'resources': [{
            'id': 1,
            'kind': 'index_buffer',
            'provenance': 'ue4_object',
            'package_resource_ref': 0,
            'guest_base': 0,
            'guest_size': 0,
            'element_size': 2,
            'index_domain': 'ue4_object',
        }],
        'operations': [
            {
                'kind': 'SetViewport',
                'evidence': 'runtime_confirmed',
                'sequence': 0,
                'command_capture_id': 0,
                'command_list': 0,
                'rhi_context': 0,
                'viewport': {'min_x': 0, 'min_y': 0, 'min_z': 0,
                             'max_x': 64, 'max_y': 64, 'max_z': 1},
                'color_target_count': 0,
                'color_targets': [],
                'depth_target': 0,
            },
            {
                'kind': 'BindVertexBuffers',
                'evidence': 'runtime_confirmed',
                'sequence': 1,
                'command_capture_id': 0,
                'command_list': 0,
                'rhi_context': 0,
                'viewport': {'min_x': 0, 'min_y': 0, 'min_z': 0,
                             'max_x': 0, 'max_y': 0, 'max_z': 0},
                'color_target_count': 0,
                'color_targets': [],
                'depth_target': 0,
                'vertex_resources': [0],
            },
            {
                'kind': 'DrawIndexed',
                'evidence': 'runtime_confirmed',
                'sequence': 2,
                'command_capture_id': 0,
                'command_list': 0,
                'rhi_context': 0,
                'viewport': {'min_x': 0, 'min_y': 0, 'min_z': 0,
                             'max_x': 0, 'max_y': 0, 'max_z': 0},
                'color_target_count': 1,
                'color_targets': [0],
                'depth_target': 0,
                'draw': {
                    'index_buffer_resource': 1,
                    'primitive_type': 0,
                    'base_vertex_index': 0,
                    'first_instance': 0,
                    'num_vertices': 0,
                    'start_index': 0,
                    'num_primitives': 2,
                    'num_instances': 1,
                },
                'derived': {
                    'index_count': 6,
                    'first_index': 0,
                    'vertex_offset': 0,
                    'instance_count': 1,
                    'index_byte_offset': 0,
                    'valid': True,
                },
            },
        ],
        'unresolved': [],
    }
    with open(os.path.join(out, 'tape.json'), 'w', encoding='utf-8') as handle:
        json.dump(tape, handle, indent=1)

    print(f'wrote {out}')
    print(f'  schema 4, {len(stages)} stages, {len(interfaces)} interfaces')
    print(f'  reference {width}x{height} {bpp} bpp, sha256 {sha256_hex(post)[:16]}...')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
