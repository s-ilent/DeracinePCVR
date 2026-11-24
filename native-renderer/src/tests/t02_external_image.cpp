// t02_external_image: create Vulkan images outside NRI, import them through
// the public wrapping API, and verify contents plus ownership semantics.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/nri_adoption.h"
#include "common/vk_bootstrap.h"

namespace {

// Fills a color image with a deterministic per-layer pattern via a clear.
void FillPattern(harness::VulkanContext& vk, VkCommandPool pool, const harness::Image& img,
                 uint32_t layer, float r, float g, float b, float a) {
    const float rgba[4] = {r, g, b, a};
    harness::ClearColor(vk, pool, img, rgba, layer);
}

}  // namespace

int main() {
    harness::TestLog log("t02_external_image");

    harness::BootstrapConfig cfg;
    cfg.enableValidation = true;
    cfg.enableSyncValidation = true;

    harness::VulkanContext vk;
    std::string error;
    if (!vk.Initialize(cfg, error)) {
        log.Fail("Vulkan init: " + error);
        return log.Finish();
    }

    harness::NriContext nri;
    if (!nri.Initialize(vk, true, error)) {
        log.Fail("NRI init: " + error);
        return log.Finish();
    }
    log.Pass("device and NRI wrapper ready");

    VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());
    if (pool == VK_NULL_HANDLE) {
        log.Fail("command pool creation failed");
        return log.Finish();
    }

    // ---- Case 1: single-layer color image ----
    {
        harness::ImageDesc d;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.width = 32;
        d.height = 32;
        d.layerNum = 1;
        harness::Image img;
        if (!harness::CreateImage(vk, d, img, error)) {
            log.Fail("case1 create: " + error);
        } else {
            FillPattern(vk, pool, img, 0, 1.0f, 0.0f, 0.0f, 1.0f);

            nri::Texture* tex = nullptr;
            if (!nri.WrapTexture(img.image, d.format, VK_IMAGE_TYPE_2D, d.usage, d.width,
                                 d.height, 1, 1, 1, 1, tex, error)) {
                log.Fail("case1 wrap: " + error);
            } else {
                log.Pass("case1: single-layer color image wrapped");

                // Verify NRI reports the same native image.
                const uint64_t native = nri.core().GetTextureNativeObject(tex);
                if (native == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(img.image))) {
                    log.Pass("case1: NRI texture handle matches the VkImage");
                } else {
                    log.Fail("case1: NRI texture handle differs");
                }

                // Verify contents survived the wrap.
                std::vector<unsigned char> px;
                if (harness::ReadbackColor(vk, pool, img, 0, px, error)) {
                    const unsigned char expect[4] = {255, 0, 0, 255};
                    if (harness::PixelNear(px.data(), expect, 2)) {
                        log.Pass("case1: readback is red as written");
                    } else {
                        log.Fail("case1: readback mismatch");
                    }
                } else {
                    log.Fail("case1 readback: " + error);
                }

                // Destroy the wrapper; the VkImage must still be usable.
                nri.DestroyWrappedTexture(tex);
                log.Pass("case1: NRI wrapper destroyed");

                std::vector<unsigned char> px2;
                if (harness::ReadbackColor(vk, pool, img, 0, px2, error)) {
                    log.Pass("case1: VkImage still readable after wrapper destruction");
                } else {
                    log.Fail("case1: VkImage unusable after wrapper destruction: " + error);
                }
            }
            harness::DestroyImage(vk, img);
        }
    }

    // ---- Case 2: two-layer stereo color image ----
    {
        harness::ImageDesc d;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.width = 32;
        d.height = 32;
        d.layerNum = 2;
        harness::Image img;
        if (!harness::CreateImage(vk, d, img, error)) {
            log.Fail("case2 create: " + error);
        } else {
            FillPattern(vk, pool, img, 0, 0.0f, 1.0f, 0.0f, 1.0f);  // layer 0 green
            FillPattern(vk, pool, img, 1, 0.0f, 0.0f, 1.0f, 1.0f);  // layer 1 blue

            nri::Texture* tex = nullptr;
            if (!nri.WrapTexture(img.image, d.format, VK_IMAGE_TYPE_2D, d.usage, d.width,
                                 d.height, 1, 1, 2, 1, tex, error)) {
                log.Fail("case2 wrap: " + error);
            } else {
                log.Pass("case2: two-layer stereo image wrapped");

                std::vector<unsigned char> l0, l1;
                const bool ok0 = harness::ReadbackColor(vk, pool, img, 0, l0, error);
                const bool ok1 = harness::ReadbackColor(vk, pool, img, 1, l1, error);
                const unsigned char green[4] = {0, 255, 0, 255};
                const unsigned char blue[4] = {0, 0, 255, 255};
                if (ok0 && harness::PixelNear(l0.data(), green, 2)) {
                    log.Pass("case2: layer 0 is green");
                } else {
                    log.Fail("case2: layer 0 mismatch");
                }
                if (ok1 && harness::PixelNear(l1.data(), blue, 2)) {
                    log.Pass("case2: layer 1 is blue");
                } else {
                    log.Fail("case2: layer 1 mismatch");
                }
                nri.DestroyWrappedTexture(tex);
            }
            harness::DestroyImage(vk, img);
        }
    }

    // ---- Case 3: combined depth/stencil image ----
    {
        const VkFormat dsFmt = harness::PickDepthStencilFormat(vk, true);
        if (dsFmt == VK_FORMAT_UNDEFINED) {
            log.Unsupported("case3: no sampled combined depth/stencil format");
        } else {
            harness::ImageDesc d;
            d.format = dsFmt;
            d.width = 32;
            d.height = 32;
            d.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            harness::Image img;
            if (!harness::CreateImage(vk, d, img, error)) {
                log.Fail("case3 create: " + error);
            } else {
                harness::ClearDepthStencil(vk, pool, img, 0.25f, 0x5A, 0);

                nri::Texture* tex = nullptr;
                if (!nri.WrapTexture(img.image, d.format, VK_IMAGE_TYPE_2D, d.usage, d.width,
                                     d.height, 1, 1, 1, 1, tex, error)) {
                    log.Fail("case3 wrap: " + error);
                } else {
                    log.Pass("case3: combined depth/stencil image wrapped");
                    std::vector<float> depth;
                    if (harness::ReadbackDepth(vk, pool, img, 0, depth, error)) {
                        if (depth[0] > 0.24f && depth[0] < 0.26f) {
                            log.Pass("case3: depth readback is 0.25");
                        } else {
                            log.Fail("case3: depth readback " + std::to_string(depth[0]));
                        }
                    } else {
                        log.Fail("case3 depth readback: " + error);
                    }
                    std::vector<unsigned char> sten;
                    if (harness::ReadbackStencil(vk, pool, img, 0, sten, error)) {
                        if (sten[0] == 0x5A) {
                            log.Pass("case3: stencil readback is 0x5A");
                        } else {
                            log.Fail("case3: stencil readback 0x" + std::to_string(sten[0]));
                        }
                    } else {
                        log.Fail("case3 stencil readback: " + error);
                    }
                    nri.DestroyWrappedTexture(tex);
                }
                harness::DestroyImage(vk, img);
            }
        }
    }

    // ---- Case 4: multisampled color image ----
    {
        const auto& m = vk.manifest();
        if (!(m.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
            log.Unsupported("case4: 4x MSAA color not supported");
        } else {
            harness::ImageDesc d;
            d.format = VK_FORMAT_R8G8B8A8_UNORM;
            d.width = 32;
            d.height = 32;
            d.samples = VK_SAMPLE_COUNT_4_BIT;
            d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            harness::Image img;
            if (!harness::CreateImage(vk, d, img, error)) {
                log.Fail("case4 create: " + error);
            } else {
                nri::Texture* tex = nullptr;
                if (!nri.WrapTexture(img.image, d.format, VK_IMAGE_TYPE_2D, d.usage, d.width,
                                     d.height, 1, 1, 1, 4, tex, error)) {
                    log.Fail("case4 wrap: " + error);
                } else {
                    log.Pass("case4: 4x MSAA image wrapped");
                    nri.DestroyWrappedTexture(tex);
                }
                harness::DestroyImage(vk, img);
            }
        }
    }

    // ---- Case 5: sRGB and linear views over the same image ----
    // FINDING: a view whose format differs from the image format requires
    // VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT at image creation. Without it the
    // validation layer rejects the view (VUID-VkImageViewCreateInfo-image-12397).
    // This is a real constraint for any host that wants sRGB/linear aliasing.
    {
        harness::ImageDesc d;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.width = 16;
        d.height = 16;
        d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        d.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        harness::Image img;
        if (!harness::CreateImage(vk, d, img, error)) {
            log.Fail("case5 create: " + error);
        } else {
            FillPattern(vk, pool, img, 0, 0.5f, 0.5f, 0.5f, 1.0f);

            VkImageView linear = harness::CreateView(vk, img.image, VK_FORMAT_R8G8B8A8_UNORM,
                                                     VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                                                     VK_IMAGE_VIEW_TYPE_2D, error);
            VkImageView srgb = harness::CreateView(vk, img.image, VK_FORMAT_R8G8B8A8_SRGB,
                                                   VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                                                   VK_IMAGE_VIEW_TYPE_2D, error);
            if (linear != VK_NULL_HANDLE && srgb != VK_NULL_HANDLE) {
                log.Pass("case5: linear and sRGB views over one MUTABLE_FORMAT image");
            } else {
                log.Unsupported("case5: sRGB view not permitted even with MUTABLE_FORMAT");
            }
            if (linear) vkDestroyImageView(vk.device(), linear, nullptr);
            if (srgb) vkDestroyImageView(vk.device(), srgb, nullptr);
            harness::DestroyImage(vk, img);
        }
    }

    // ---- Case 5b: the negative case, proving the constraint is real ----
    {
        harness::ImageDesc d;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.width = 8;
        d.height = 8;
        d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // Deliberately NO MUTABLE_FORMAT.
        harness::Image img;
        if (!harness::CreateImage(vk, d, img, error)) {
            log.Fail("case5b create: " + error);
        } else {
            // Declare the exact VUID this negative test expects. The mechanism
            // fails the run if the diagnostic never fires, and never suppresses
            // anything else. Validation messages arrive asynchronously, so the
            // verdict is deferred to AllExpectationsMet() at the end.
            const uint64_t token =
                harness::BeginExpectedDiagnostic("VUID-VkImageViewCreateInfo-image-12397");
            std::string ignored;
            VkImageView bad = harness::CreateView(vk, img.image, VK_FORMAT_R8G8B8A8_SRGB,
                                                  VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                                                  VK_IMAGE_VIEW_TYPE_2D, ignored);
            if (bad != VK_NULL_HANDLE) vkDestroyImageView(vk.device(), bad, nullptr);
            harness::EndExpectedDiagnostic(token);
            // Establish the boundary so the expectation is definitively closed
            // before any later test could produce the same VUID.
            harness::SynchronizeDiagnostics();
            if (harness::AllExpectationsMet()) {
                log.Pass("case5b: expected VUID-VkImageViewCreateInfo-image-12397 fired");
            } else {
                log.Fail("case5b: expected diagnostic did not fire");
            }
            harness::DestroyImage(vk, img);
        }
    }

    // ---- Case 6: repeated wrap/teardown ----
    {
        harness::ImageDesc d;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.width = 16;
        d.height = 16;
        harness::Image img;
        if (!harness::CreateImage(vk, d, img, error)) {
            log.Fail("case6 create: " + error);
        } else {
            int ok = 0;
            for (int i = 0; i < 32; ++i) {
                nri::Texture* tex = nullptr;
                if (nri.WrapTexture(img.image, d.format, VK_IMAGE_TYPE_2D, d.usage, d.width,
                                    d.height, 1, 1, 1, 1, tex, error)) {
                    nri.DestroyWrappedTexture(tex);
                    ++ok;
                }
            }
            if (ok == 32) {
                log.Pass("case6: 32 wrap/destroy cycles succeeded");
            } else {
                log.Fail("case6: only " + std::to_string(ok) + "/32 cycles succeeded");
            }
            harness::DestroyImage(vk, img);
        }
    }

    vkDestroyCommandPool(vk.device(), pool, nullptr);
    nri.Shutdown();
    vk.Shutdown();

    const uint32_t unexpected = harness::UnexpectedErrorCount();
    if (unexpected == 0) {
        log.Pass("zero unexpected validation errors");
    } else {
        log.Fail(std::to_string(unexpected) + " unexpected validation errors");
        for (const auto& msg : harness::GlobalValidationCounters().messages) log.Info(msg);
    }
    if (!harness::AllExpectationsMet()) {
        log.Fail("a declared expected diagnostic never fired");
    }

    return log.Finish();
}
