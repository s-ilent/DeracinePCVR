// Runtime GLSL to SPIR-V compilation via the Vulkan SDK's shaderc/glslang.
// Falls back to loading a prebuilt .spv if the compiler is unavailable.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace harness {

class VulkanContext;

// Compiles GLSL source to SPIR-V. 'stage' is a VkShaderStageFlagBits.
// Returns false and fills 'error' when compilation is unavailable or fails.
bool CompileGlsl(const char* source, VkShaderStageFlagBits stage, const char* entryPoint,
                 std::vector<uint32_t>& spirv, std::string& error);

// True when a GLSL compiler was found at build time.
bool GlslCompilerAvailable();

// Creates a shader module from SPIR-V.
VkShaderModule CreateShaderModule(VulkanContext& ctx, const std::vector<uint32_t>& spirv,
                                  std::string& error);

}  // namespace harness
