// Exercise the layer's provenance joins without a GPU or guest payload.
#include "vr_evidence_layer.cc"
#include <gtest/gtest.h>

namespace {
void *test_owner = nullptr;
void *test_request_owner = nullptr;
int test_eye = -1;
std::uintptr_t next_handle = 0x1000;
void *InitializerOwner() { return test_owner; }
bool CurrentEye(void **owner, int *index, void **framebuffer) {
  *owner = test_request_owner;
  *index = test_eye;
  *framebuffer = reinterpret_cast<void *>(0x8000);
  return test_request_owner != nullptr;
}
void ConsumeEye() { test_request_owner = nullptr; }
VKAPI_ATTR VkResult VKAPI_CALL CreateImageStub(VkDevice,
                                               const VkImageCreateInfo *,
                                               const VkAllocationCallbacks *,
                                               VkImage *image) {
  *image = reinterpret_cast<VkImage>(++next_handle);
  return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateFramebufferStub(
    VkDevice, const VkFramebufferCreateInfo *, const VkAllocationCallbacks *,
    VkFramebuffer *framebuffer) {
  *framebuffer = reinterpret_cast<VkFramebuffer>(++next_handle);
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL RenderPassStub(VkCommandBuffer,
                                          const VkRenderPassBeginInfo *,
                                          VkSubpassContents) {}
VKAPI_ATTR void VKAPI_CALL DestroyImageStub(VkDevice, VkImage,
                                            const VkAllocationCallbacks *) {}
VKAPI_ATTR void VKAPI_CALL DestroyFramebufferStub(
    VkDevice, VkFramebuffer, const VkAllocationCallbacks *) {}

class VrEvidenceProvenance : public ::testing::Test {
protected:
  void *key = this;
  VkDevice device = reinterpret_cast<VkDevice>(&key);
  VkCommandBuffer command = reinterpret_cast<VkCommandBuffer>(&key);
  void SetUp() override {
    g_active = true;
    g_images.clear();
    g_image_views.clear();
    g_framebuffer_images.clear();
    g_dispatches.clear();
    g_eyes[0] = {};
    g_eyes[1] = {};
    g_initializing_object = InitializerOwner;
    g_current_eye = CurrentEye;
    g_consume_eye = ConsumeEye;
    test_owner = nullptr;
    test_request_owner = nullptr;
    auto &dispatch = g_dispatches[key];
    dispatch.device = device;
    dispatch.create_image = CreateImageStub;
    dispatch.create_framebuffer = CreateFramebufferStub;
    dispatch.cmd_begin_render_pass = RenderPassStub;
    dispatch.destroy_image = DestroyImageStub;
    dispatch.destroy_framebuffer = DestroyFramebufferStub;
  }
  VkImage Image() {
    VkImageCreateInfo info{};
    info.extent = {500, 500, 1};
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image;
    EXPECT_EQ(LayerCreateImage(device, &info, nullptr, &image), VK_SUCCESS);
    return image;
  }
  VkFramebuffer Framebuffer(VkImage image) {
    VkImageView view = reinterpret_cast<VkImageView>(++next_handle);
    g_image_views[view] = image;
    VkFramebufferCreateInfo info{};
    info.attachmentCount = 1;
    info.pAttachments = &view;
    VkFramebuffer framebuffer;
    EXPECT_EQ(LayerCreateFramebuffer(device, &info, nullptr, &framebuffer),
              VK_SUCCESS);
    return framebuffer;
  }
  void Render(VkFramebuffer framebuffer, void *owner, int eye) {
    test_request_owner = owner;
    test_eye = eye;
    VkRenderPassBeginInfo info{};
    info.framebuffer = framebuffer;
    LayerCmdBeginRenderPass(command, &info, VK_SUBPASS_CONTENTS_INLINE);
  }
};

TEST_F(VrEvidenceProvenance,
       IgnoresSameSizeTargetsOutsideInitializerAndWrongOwner) {
  void *owner = reinterpret_cast<void *>(0x1234);
  auto unrelated = Image();
  Render(Framebuffer(unrelated), owner, 0);
  EXPECT_FALSE(g_eyes[0].valid);
  EXPECT_TRUE(g_images.empty());
  test_owner = owner;
  auto image = Image();
  auto framebuffer = Framebuffer(image);
  Render(framebuffer, reinterpret_cast<void *>(0x5678), 0);
  EXPECT_FALSE(g_eyes[0].valid);
  Render(framebuffer, owner, 0);
  EXPECT_TRUE(g_eyes[0].valid);
  EXPECT_EQ(g_eyes[0].image.image, image);
}

TEST_F(VrEvidenceProvenance,
       EyeIndexComesFromGetterAndDestroyedHandlesAreForgotten) {
  test_owner = reinterpret_cast<void *>(0x1234);
  auto right = Image();
  auto left = Image();
  auto right_fb = Framebuffer(right);
  auto left_fb = Framebuffer(left);
  Render(right_fb, test_owner, 1);
  Render(left_fb, test_owner, 0);
  EXPECT_EQ(g_eyes[0].image.image, left);
  EXPECT_EQ(g_eyes[1].image.image, right);
  LayerDestroyImage(device, left, nullptr);
  EXPECT_FALSE(g_eyes[0].valid);
  EXPECT_FALSE(g_eyes[1].valid);
  Render(left_fb, test_owner, 0);
  EXPECT_FALSE(g_eyes[0].valid);
  LayerDestroyFramebuffer(device, right_fb, nullptr);
  EXPECT_EQ(g_framebuffer_images.count(right_fb), 0u);
}
} // namespace
