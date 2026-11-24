#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Rebuilds the SC-11R test shader fixtures.
#
# The .spv files are committed so the tests need no shader compiler at run time. This script exists
# so they can be regenerated and re-validated when the sources change.
#
# Requires glslc and spirv-val, which ship with the Vulkan SDK.

set -euo pipefail
cd "$(dirname "$0")"

glslc -fshader-stage=vert sc11r_test.vert -o sc11r_test.vert.spv
glslc -fshader-stage=frag sc11r_test.frag -o sc11r_test.frag.spv

spirv-val sc11r_test.vert.spv
spirv-val sc11r_test.frag.spv

echo "test shaders rebuilt and validated"
