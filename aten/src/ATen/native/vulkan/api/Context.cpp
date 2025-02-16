#include <ATen/native/vulkan/api/Context.h>
#include <ATen/vulkan/Context.h>
#include <ATen/native/vulkan/ops/Copy.h>

#include <sstream>

namespace at {
namespace native {
namespace vulkan {
namespace api {
namespace {

VkDevice create_device(
    const VkPhysicalDevice physical_device,
    const uint32_t compute_queue_family_index) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      physical_device,
      "Invalid Vulkan physical device!");

  const float queue_priorities = 1.0f;
  const VkDeviceQueueCreateInfo device_queue_create_info{
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    nullptr,
    0u,
    compute_queue_family_index,
    1u,
    &queue_priorities,
  };

  uint32_t device_extension_properties_count = 0;
  VK_CHECK(vkEnumerateDeviceExtensionProperties(
      physical_device,
      nullptr,
      &device_extension_properties_count,
      nullptr));

  std::vector<VkExtensionProperties> device_extension_properties(
      device_extension_properties_count);

  VK_CHECK(vkEnumerateDeviceExtensionProperties(
      physical_device,
      nullptr,
      &device_extension_properties_count,
      device_extension_properties.data()));

  constexpr const char* const requested_device_extensions[]{
  #ifdef VK_KHR_portability_subset
    // https://vulkan.lunarg.com/doc/view/1.2.162.0/mac/1.2-extensions/vkspec.html#VUID-VkDeviceCreateInfo-pProperties-04451
    VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
  #endif
  };

  std::vector<const char*> enabled_device_extensions;

  for (const auto& requested_device_extension : requested_device_extensions) {
    for (const auto& extension : device_extension_properties) {
      if (strcmp(requested_device_extension, extension.extensionName) == 0) {
        enabled_device_extensions.push_back(requested_device_extension);
        break;
      }
    }
  }

  const VkDeviceCreateInfo device_create_info{
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    nullptr,
    0u,
    1u,
    &device_queue_create_info,
    0u,
    nullptr,
    static_cast<uint32_t>(enabled_device_extensions.size()),
    enabled_device_extensions.data(),
    nullptr,
  };

  VkDevice device{};
  VK_CHECK(vkCreateDevice(physical_device, &device_create_info, nullptr, &device));
  TORCH_CHECK(device, "Invalid Vulkan device!");

#ifdef USE_VULKAN_WRAPPER
#ifdef USE_VULKAN_VOLK
  volkLoadDevice(device);
#endif
#endif

  return device;
}

VkQueue acquire_queue(
    const VkDevice device,
    const uint32_t compute_queue_family_index) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      device,
      "Invalid Vulkan device!");

  VkQueue queue{};
  vkGetDeviceQueue(device, compute_queue_family_index, 0, &queue);
  TORCH_CHECK(queue, "Invalid Vulkan queue!");

  return queue;
}

} // namespace

Context::Context(const VkInstance instance, size_t adapter_i)
    : instance_(instance),
      adapter_i_(adapter_i),
      device_(runtime()->get_adapter(adapter_i).device_handle()),
      queue_(runtime()->get_adapter(adapter_i).request_queue()),
      shader_(gpu()),
      pipeline_(gpu()),
      threadcontext_(gpu()) {
}

Context::~Context() {
  // Let the device know the context is done with the queue
  runtime()->get_adapter(adapter_i_).return_queue(queue_);
  // Do not call flush() since all per-thread objects will be destroyed as each thread exits
}

void Context::flush() {
  VK_CHECK(vkQueueWaitIdle(queue()));

  resource().pool.purge();
  descriptor().pool.purge();
  command().pool.purge();
}

void Context::wait(const at::Tensor& src) {
  // wait only if Vulkan tensor
  if (at::kVulkan == src.device().type()) {
    api::Command::Pool& command_pool = command().pool;
    api::Command::Buffer& command_buffer = command_pool.stream();

    using Future = ops::vTensor::Future<const void, ops::vTensor::Access::Read>;
    const ops::vTensor& v_src = ops::convert(src);
    const Future v_src_future = v_src.host<const void>(command_buffer);

    // This wait() is a no-op if data is not out of sync.  More often than
    // not though, waits here are expected as the GPU catches up with
    // compute submitted from CPU.
    v_src_future.wait();
  }
}

bool available() {
  return context();
}

Context* context() {
  static const std::unique_ptr<Context> context([]() -> Context* {
    try {
      return new Context(runtime()->instance(), runtime()->default_adapter_i());
    }
    catch (const std::exception& e) {
      TORCH_CHECK(false, "Vulkan: Failed to initialize context! Error: ", e.what());
    }
    catch (...) {
      TORCH_CHECK(false, "Vulkan: Failed to initialize context! Error: Unknown");
    }

    return nullptr;
  }());

  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      context,
      "Invalid Vulkan context!");

  return context.get();
}

struct VulkanImpl final : public at::vulkan::VulkanImplInterface {
  bool is_vulkan_available() const override {
    return available();
  }

  Tensor& vulkan_copy_(Tensor& self, const Tensor& src) const override {
    return vulkan::ops::copy_(self, src);
  }
};
static at::vulkan::VulkanImplRegistrar g_vulkan_impl(new VulkanImpl());

Descriptor::Set dispatch_prologue(
    Command::Buffer& command_buffer,
    const Shader::Layout::Signature& shader_layout_signature,
    const Shader::Descriptor& shader_descriptor,
    const Shader::WorkGroup& local_work_group_size) {
  Context* const context = api::context();
  Descriptor& descriptor = context->descriptor();
  Pipeline& pipeline = context->pipeline();
  Shader& shader = context->shader();

  const Shader::Layout::Object shader_layout =
      shader.layout.cache.retrieve({
        shader_layout_signature,
      });

  command_buffer.bind(
      pipeline.cache.retrieve({
        pipeline.layout.cache.retrieve({
          shader_layout.handle,
        }),
        shader.cache.retrieve(shader_descriptor),
        local_work_group_size,
      }));

  return descriptor.pool.allocate(shader_layout);
}

void dispatch_epilogue(
    Command::Buffer& command_buffer,
    const Descriptor::Set& descriptor_set,
    const Shader::WorkGroup& global_work_group) {
  command_buffer.bind(descriptor_set);
  command_buffer.dispatch(global_work_group);
}

} // namespace api
} // namespace vulkan
} // namespace native
} // namespace at
