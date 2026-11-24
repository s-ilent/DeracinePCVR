#include "common/shader_compile.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "common/vk_bootstrap.h"

namespace harness {

namespace {

// The harness shells out to glslangValidator, which ships with the Vulkan SDK.
// This avoids linking a shader compiler into the test binaries.
std::string FindGlslang() {
    const char* sdk = std::getenv("VULKAN_SDK");
    if (sdk != nullptr) {
        std::string p = std::string(sdk) + "/Bin/glslangValidator.exe";
        std::ifstream f(p);
        if (f.good()) return p;
        p = std::string(sdk) + "/bin/glslangValidator";
        std::ifstream f2(p);
        if (f2.good()) return p;
    }
    return "glslangValidator";
}

}  // namespace

bool GlslCompilerAvailable() {
    const std::string exe = FindGlslang();
    const std::string cmd = "\"" + exe + "\" --version > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

bool CompileGlsl(const char* source, VkShaderStageFlagBits stage, const char* entryPoint,
                 std::vector<uint32_t>& spirv, std::string& error) {
    const std::string exe = FindGlslang();

    // Use a deterministic temp directory rather than tmpnam, which can return
    // paths the shell cannot open on Windows.
    char tmpDir[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, tmpDir);
    if (n == 0 || n >= MAX_PATH) {
        error = "GetTempPath failed";
        return false;
    }
    static std::atomic<unsigned> counter{0};
    const unsigned id = counter.fetch_add(1);
    std::string inPath = std::string(tmpDir) + "harness_shader_" + std::to_string(id) + ".comp";
    std::string outPath = std::string(tmpDir) + "harness_shader_" + std::to_string(id) + ".spv";

    {
        std::ofstream f(inPath);
        if (!f.good()) {
            error = "cannot write temp shader";
            return false;
        }
        f << source;
    }

    const char* stageFlag = "comp";
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT: stageFlag = "vert"; break;
        case VK_SHADER_STAGE_FRAGMENT_BIT: stageFlag = "frag"; break;
        case VK_SHADER_STAGE_COMPUTE_BIT: stageFlag = "comp"; break;
        case VK_SHADER_STAGE_GEOMETRY_BIT: stageFlag = "geom"; break;
        default: break;
    }

    std::ostringstream cmd;
    cmd << "\"" << exe << "\" -V --target-env vulkan1.3 -S " << stageFlag
        << " -e " << entryPoint << " -o \"" << outPath << "\" \"" << inPath << "\"";

    // Run through cmd.exe so redirection and quoting behave predictably.
    std::string full = "cmd.exe /C \"" + cmd.str() + " 2>&1\"";
    std::string output;
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = writePipe;
            si.hStdError = writePipe;
            si.hStdInput = nullptr;
            PROCESS_INFORMATION pi{};
            std::vector<char> mutableCmd(full.begin(), full.end());
            mutableCmd.push_back('\0');
            if (CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(writePipe);
                char buf[512];
                DWORD got = 0;
                while (ReadFile(readPipe, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
                    buf[got] = '\0';
                    output += buf;
                }
                WaitForSingleObject(pi.hProcess, 30000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            } else {
                CloseHandle(readPipe);
                CloseHandle(writePipe);
            }
        }
    }

    std::ifstream spv(outPath, std::ios::binary);
    if (!spv.good()) {
        error = "glslang produced no output: " + output;
        std::remove(inPath.c_str());
        return false;
    }
    spv.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(spv.tellg());
    spv.seekg(0, std::ios::beg);
    spirv.resize(size / 4);
    spv.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(size));

    std::remove(inPath.c_str());
    std::remove(outPath.c_str());

    if (spirv.empty()) {
        error = "empty SPIR-V: " + output;
        return false;
    }
    return true;
}

VkShaderModule CreateShaderModule(VulkanContext& ctx, const std::vector<uint32_t>& spirv,
                                  std::string& error) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx.device(), &ci, nullptr, &mod) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed";
        return VK_NULL_HANDLE;
    }
    return mod;
}

}  // namespace harness
