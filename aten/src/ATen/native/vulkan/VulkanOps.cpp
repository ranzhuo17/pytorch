#include <iostream>
#include <limits>
#include <vector>

#include <c10/util/Exception.h>
#include <c10/util/Optional.h>

#include <ATen/native/vulkan/Vulkan.h>
#include <ATen/native/vulkan/VulkanCommon.h>
#include <ATen/native/vulkan/VulkanConvolution.h>
#include <ATen/native/vulkan/VulkanOps.h>

namespace at {
namespace native {
namespace vulkan {
namespace detail {

void upsample_nearest2d(
    VulkanTensor& output,
    const VulkanTensor& input,
    int64_t IH,
    int64_t IW,
    int64_t OH,
    int64_t OW,
    int64_t _N,
    int64_t _C,
    float scaleH,
    float scaleW) {
  auto device = context().device();
  auto physicalDevice = context().physicalDevice();
  int64_t C = _N * _C;
  struct ConstBlock {
    int32_t IW;
    int32_t IH;
    int32_t OW;
    int32_t OH;
    float scaleX;
    float scaleY;
  };
  ConstBlock cb{IW, IH, OW, OH, scaleW, scaleH};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  constBuffer.bind(descriptorSet, 2);

  WorkGroupSize workGroupSize{8, 8, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(upsampleNearest2d), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  input.image()->addImageMemoryBarrierToShaderRead(computeUnit.commandBuffer());
  computeUnit.dispatchCommandBuffer(OW, OH, C, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

VulkanTensor reshape_copy(
    const VulkanTensor& input,
    std::vector<int64_t> shape) {
  const auto shapeNumel = std::accumulate(
      std::begin(shape), std::end(shape), 1, std::multiplies<int64_t>());
  TORCH_INTERNAL_ASSERT(
      shapeNumel == input.numel(),
      "reshape_copy expects shape with equal number of elements with input Vulkan tensor");

  input.sync_image_to_buffer();

  VulkanTensor output{shape};
  output.allocate_storage();
  copy_buffer_to_buffer(
      *(input.buffer()), *(output.buffer()), input.buffer()->sizeBytes());
  return output;
}

VulkanTensor cat(
    VulkanTensor& output,
    ArrayRef<VulkanTensor> inputs,
    int64_t dim) {
  VkDeviceSize outputOffset = 0;
  for (const auto& input : inputs) {
    input.sync_image_to_buffer();
    const auto sizeBytes = sizeof(float) * input.numel();
    copy_buffer_to_buffer(
        *(input.buffer()), *(output.buffer()), sizeBytes, 0, outputOffset);
    outputOffset += sizeBytes;
  }
  return output;
}

void adaptive_avg_pool2d(
    VulkanTensor& output,
    const VulkanTensor& input,
    const int64_t IH,
    const int64_t IW,
    const int64_t OH,
    const int64_t OW,
    const int64_t IN,
    const int64_t IC) {
  auto device = context().device();
  int64_t C = IN * IC;
  struct ConstBlock {
    int32_t IW;
    int32_t IH;
    int32_t OW;
    int32_t OH;
  };
  ConstBlock cb{IW, IH, OW, OH};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  constBuffer.bind(descriptorSet, 2);

  WorkGroupSize workGroupSize{8, 8, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(adaptive_avg_pool2d), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  input.image()->addImageMemoryBarrierToShaderRead(computeUnit.commandBuffer());
  computeUnit.dispatchCommandBuffer(OW, OH, C, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

void max_pool2d(
    VulkanTensor& output,
    const VulkanTensor& input,
    const int iH,
    const int iW,
    const int oH,
    const int oW,
    const int _n,
    const int _c,
    const int kH,
    const int kW,
    const int dH,
    const int dW,
    const int padH,
    const int padW,
    const int dilationH,
    const int dilationW) {
  auto device = context().device();
  const auto c = _n * _c;
  struct ConstBlock {
    int32_t inputSize[4];
    int32_t outputSize[4];
    int32_t kernelSize[2];
    int32_t stride[2];
    int32_t padding[2];
    int32_t dilate[2];
  };
  ConstBlock cb{
      {iW, iH, c, 0},
      {oW, oH, c, 0},
      {kW, kH},
      {dW, dH},
      {padW, padH},
      {dilationW, dilationH},
  };
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  constBuffer.bind(descriptorSet, 2);

  WorkGroupSize workGroupSize{8, 8, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(max_pool2d), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  input.image()->addImageMemoryBarrierToShaderRead(computeUnit.commandBuffer());
  computeUnit.dispatchCommandBuffer(oW, oH, c, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

void add(
    VulkanTensor& output,
    const VulkanTensor& input0,
    const VulkanTensor& input1,
    float alpha) {
  auto odim = output.dim();
  TORCH_INTERNAL_ASSERT(
      odim <= 4, "Vulkan add is implemented for dim <= 4, output dim > 4");
  auto i0dim = input0.dim();
  TORCH_INTERNAL_ASSERT(
      i0dim <= 4, "Vulkan add is implemented for dim <= 4, input0 dim > 4");
  auto i1dim = input1.dim();
  TORCH_INTERNAL_ASSERT(
      i1dim <= 4, "Vulkan add is implemented for dim <= 4, input1 dim > 4");

  auto os = output.sizes();
  auto i0s = input0.sizes();
  auto i1s = input1.sizes();

  std::array<int64_t, 4> os4 = {1, 1, 1, 1};
  std::copy(os.begin(), os.end(), os4.end() - odim);
  std::array<int64_t, 4> i0s4 = {1, 1, 1, 1};
  std::copy(i0s.cbegin(), i0s.cend(), i0s4.end() - i0dim);
  std::array<int64_t, 4> i1s4 = {1, 1, 1, 1};
  std::copy(i1s.cbegin(), i1s.cend(), i1s4.end() - i1dim);

  TORCH_INTERNAL_ASSERT(
      (os4 == i0s4) && (i0s4 == i1s4),
      "Vulkan add expects the same dimensions for all operands");

  auto C = os4[0] * os4[1];
  auto H = os4[2];
  auto W = os4[3];

  auto device = context().device();
  auto physicalDevice = context().physicalDevice();
  struct ConstBlock {
    int32_t W;
    int32_t H;
    int32_t C;
    float alpha;
  };
  ConstBlock cb{W, H, C, alpha};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input0.image()->bindShaderRead(descriptorSet, 1);
  input1.image()->bindShaderRead(descriptorSet, 2);
  constBuffer.bind(descriptorSet, 3);

  WorkGroupSize workGroupSize{8, 8, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(add), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  auto commandBuffer = computeUnit.commandBuffer();
  output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
  input0.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  input1.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  computeUnit.dispatchCommandBuffer(W, H, C, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

void add(VulkanTensor& output, const VulkanTensor& input, const float s) {
  const auto sizes = input.sizes();

  const auto C = std::accumulate(
      sizes.cbegin(), sizes.cend() - 2, 1, std::multiplies<int64_t>());
  const auto H = sizes[2];
  const auto W = sizes[3];

  auto device = context().device();
  struct ConstBlock {
    int32_t inputSize[4];
    float s;
  };
  ConstBlock cb{{W, H, C, 0}, s};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  constBuffer.bind(descriptorSet, 2);

  WorkGroupSize workGroupSize{8, 8, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(add_scalar), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  auto commandBuffer = computeUnit.commandBuffer();
  output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
  input.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  computeUnit.dispatchCommandBuffer(W, H, C, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

void mul(VulkanTensor& output, const VulkanTensor& input, const float s) {
  const auto sizes = input.sizes();

  const auto C = std::accumulate(
      sizes.cbegin(), sizes.cend() - 2, 1, std::multiplies<int64_t>());
  const auto H = sizes[2];
  const auto W = sizes[3];

  auto device = context().device();
  struct ConstBlock {
    int32_t inputSize[4];
    float s;
  };
  ConstBlock cb{{W, H, C, 0}, s};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  constBuffer.bind(descriptorSet, 2);

  WorkGroupSize workGroupSize{8, 8, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(mul_scalar), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  auto commandBuffer = computeUnit.commandBuffer();
  output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
  input.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  computeUnit.dispatchCommandBuffer(W, H, C, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

VBuffer kernelNCHW_OCHW_repack_O4C4HWi4o4(
    const float* weights,
    const int OC,
    const int C,
    const int KH,
    const int KW) {
  const auto C_4 = UP_DIV(C, 4);
  const auto kBufSizeNumel = ALIGN_UP4(OC) * ALIGN_UP4(C) * KH * KW;
  auto size = sizeof(float) * kBufSizeNumel;
  VBuffer kernelBuffer{size};
  const int oc_4SizeNumel = KW * KH * C_4 * 16;
  auto mappedMemory = kernelBuffer.map();
  if (mappedMemory.ptr()) {
    float* basePtr = (float*)mappedMemory.ptr();
    memset(basePtr, 0, size);
    const float* src = weights;
    int ridx = 0;
    for (int oc = 0; oc < OC; ++oc) {
      int oc_4 = oc / 4;
      int oc_4_i = oc % 4;
      float* dst_oc = basePtr + oc_4 * oc_4SizeNumel;
      for (int ic = 0; ic < C; ++ic) {
        int ic_4 = ic / 4;
        int ic_4_i = ic % 4;
        float* dst_ic = dst_oc + ic_4 * KW * KH * 16;
        for (int ky = 0; ky < KH; ++ky) {
          float* dst_ky = dst_ic + ky * KW * 16;
          for (int kx = 0; kx < KW; ++kx) {
            float* dst_kx = dst_ky + kx * 16;
            dst_kx[4 * ic_4_i + oc_4_i] = src[ridx++];
          }
        }
      }
    }
  }
  mappedMemory.flushWriteToDevice();
  return kernelBuffer;
}

VBuffer bufferFromOptionalHostData(
    c10::optional<const float*> data,
    const uint32_t dataSize,
    const uint32_t bufferSize) {
  TORCH_INTERNAL_ASSERT(
      dataSize <= bufferSize,
      "buffer size(",
      bufferSize,
      ") is not enough for data(",
      dataSize,
      ")");
  const auto sizeAligned =
      ROUND_UP(bufferSize, context().limits().minStorageBufferOffsetAlignment);
  VBuffer buffer{sizeAligned};
  if (data.has_value()) {
    buffer.copy_from_host_to_device(*data, dataSize);
  } else {
    buffer.set_zeros();
  }
  return buffer;
}

VBuffer bufferZeros(const uint32_t size) {
  VBuffer buffer{size};
  buffer.set_zeros();
  return buffer;
}

void conv2d_depthwise(
    VulkanTensor& output,
    const VulkanTensor& input,
    const VulkanTensor& weight,
    const VBuffer& biasBuffer,
    const Conv2DParams& params,
    c10::optional<float> output_min,
    c10::optional<float> output_max) {
  TORCH_INTERNAL_ASSERT(params.G == params.C);
  auto osizes = output.sizes();
  TORCH_INTERNAL_ASSERT(osizes[2] == params.OH);
  TORCH_INTERNAL_ASSERT(osizes[3] == params.OW);
  struct ConstBlock {
    int32_t padding[2];
    int32_t kernelSize[2];
    int32_t stride[2];
    int32_t dilate[2];
    int32_t inputSize[4];
    int32_t outputSize[4];
    float outputMin;
    float outputMax;
  };
  ConstBlock cb{
      {params.PX, params.PY},
      {params.KW, params.KH},
      {params.SX, params.SY},
      {params.DX, params.DY},
      {params.OW, params.OH, params.OC_4, 0},
      {params.W, params.H, params.C_4, 0},
      output_min ? *output_min : -std::numeric_limits<float>::infinity(),
      output_max ? *output_max : std::numeric_limits<float>::infinity()};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  auto device = context().device();
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  weight.image()->bindShaderRead(descriptorSet, 2);
  biasBuffer.bind(descriptorSet, 3);
  constBuffer.bind(descriptorSet, 4);

  WorkGroupSize workGroupSize{8, 8, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(conv2d_dw_clamp), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  auto commandBuffer = computeUnit.commandBuffer();
  output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
  input.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  weight.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  computeUnit.dispatchCommandBuffer(
      params.OW, params.OH, params.OC_4, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();

  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

void conv2d_depthwise(
    VulkanTensor& output,
    const VulkanTensor& input,
    const VulkanTensor& weight,
    const c10::optional<const float*> bias,
    const Conv2DParams params,
    c10::optional<float> output_min,
    c10::optional<float> output_max) {
  conv2d_depthwise(
      output,
      input,
      weight,
      bufferFromOptionalHostData(
          bias,
          sizeof(float) * params.OC,
          sizeof(float) * ALIGN_UP4(params.OC)),
      params,
      output_min,
      output_max);
}

void conv2d_depthwise(
    VulkanTensor& output,
    const VulkanTensor& input,
    const float* weight,
    const c10::optional<const float*> bias,
    const Conv2DParams params,
    c10::optional<float> output_min,
    c10::optional<float> output_max) {
  VulkanTensor weightTensor{{params.OC, params.KH, params.KW}};
  weightTensor.set_data_from_host(weight);
  conv2d_depthwise(
      output,
      input,
      weightTensor,
      bufferFromOptionalHostData(
          bias,
          sizeof(float) * params.OC,
          sizeof(float) * ALIGN_UP4(params.OC)),
      params,
      output_min,
      output_max);
}

ImageSizes conv2d_prepack_weights_image_sizes(
    int64_t OC,
    int64_t C,
    int64_t KH,
    int64_t KW) {
  return {{ALIGN_UP4(C), UP_DIV(OC, 4), KH * KW},
          {ALIGN_UP4(C), UP_DIV(OC, 4), KH * KW}};
}

void conv2d_prepack_weights_to_image(
    VImage& image,
    const float* weight,
    int64_t OC,
    int64_t C,
    int64_t KH,
    int64_t KW) {
  auto kernelBuffer = kernelNCHW_OCHW_repack_O4C4HWi4o4(weight, OC, C, KH, KW);
  auto OC_4 = UP_DIV(OC, 4);
  auto C_4 = UP_DIV(C, 4);

  auto expectedSizes = conv2d_prepack_weights_image_sizes(OC, C, KH, KW);
  TORCH_INTERNAL_ASSERT(
      image.sizes() == expectedSizes.imageSize,
      "Out VImage sizes do not match expected");

  struct ConstBlock {
    int32_t KWxKH;
    int32_t C_4;
  };
  ConstBlock cb{KW * KH, C_4};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      context().device(),
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  image.bindStorageImage(descriptorSet, 0);
  kernelBuffer.bind(descriptorSet, 1);
  constBuffer.bind(descriptorSet, 2);

  WorkGroupSize workGroupSize{1, 1, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(KO4C4HW_to_image), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  auto commandBuffer = computeUnit.commandBuffer();
  image.addImageMemoryBarrierToGeneral(commandBuffer);
  kernelBuffer.addBufferMemoryBarrier(
      commandBuffer, 0, kernelBuffer.sizeBytes());
  computeUnit.addMemoryBarrier(
      VK_PIPELINE_STAGE_HOST_BIT,
      VK_ACCESS_HOST_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
  computeUnit.dispatchCommandBuffer(C_4, OC_4, KH * KW, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(context().device(), descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(
      context().device(), descriptorSetLayout, nullptr);
}

VImage conv2d_prepack_weights_image(
    const float* weight,
    int64_t OC,
    int64_t C,
    int64_t KH,
    int64_t KW) {
  VImage image{conv2d_prepack_weights_image_sizes(OC, C, KH, KW)};
  conv2d_prepack_weights_to_image(image, weight, OC, C, KH, KW);
  return image;
}

void conv2d_prepack_weights(
    VulkanTensor& output,
    const float* weight,
    int64_t OC,
    int64_t C,
    int64_t KH,
    int64_t KW) {
  auto imageSizes = conv2d_prepack_weights_image_sizes(OC, C, KH, KW);
  conv2d_prepack_weights_to_image(
      *(output.image(imageSizes)), weight, OC, C, KH, KW);
}

void conv2d(
    VulkanTensor& output,
    const VulkanTensor& input,
    const VImage& kernelImage,
    const VBuffer& biasBuffer,
    const Conv2DParams& params,
    c10::optional<float> output_min,
    c10::optional<float> output_max) {
  TORCH_INTERNAL_ASSERT(
      params.G == 1, "Prepacked kernel VImage for non-group conv2d only");
  auto osizes = output.sizes();
  TORCH_INTERNAL_ASSERT(
      osizes[2] == params.OH,
      "Output tensor dims do not match specified conv2d params");
  TORCH_INTERNAL_ASSERT(
      osizes[3] == params.OW,
      "Output tensor dims do not match specified conv2d params");

  struct ConstBlock {
    int32_t padding[2];
    int32_t kernelSize[2];
    int32_t stride[2];
    int32_t dilate[2];
    int32_t inputSize[4];
    int32_t outputSize[4];
    float outputMin;
    float outputMax;
  };
  float outputMin =
      output_min ? *output_min : -std::numeric_limits<float>::infinity();
  float outputMax =
      output_max ? *output_max : std::numeric_limits<float>::infinity();
  ConstBlock cb{{params.PX, params.PY},
                {params.KW, params.KH},
                {params.SX, params.SY},
                {params.DX, params.DY},
                {params.OW, params.OH, params.OC_4, params.OC},
                {params.W, params.H, params.C_4, params.C},
                outputMin,
                outputMax};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  auto device = context().device();
  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  kernelImage.bindShaderRead(descriptorSet, 2);
  biasBuffer.bind(descriptorSet, 3);
  constBuffer.bind(descriptorSet, 4);

  WorkGroupSize workGroupSize{1, 1, params.OC_4};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(conv2d_nogroup_clamp), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  auto commandBuffer = computeUnit.commandBuffer();
  output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
  input.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  kernelImage.addImageMemoryBarrierToShaderRead(commandBuffer);
  computeUnit.dispatchCommandBuffer(
      UP_DIV(params.OW, 4 * workGroupSize.x),
      UP_DIV(params.OH, workGroupSize.y),
      UP_DIV(params.OC_4, workGroupSize.z));
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();

  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

void conv2d(
    VulkanTensor& output,
    const VulkanTensor& input,
    const VImage& kernelImage,
    const c10::optional<const float*> bias,
    const Conv2DParams& params,
    c10::optional<float> output_min,
    c10::optional<float> output_max) {
  TORCH_INTERNAL_ASSERT(
      params.G == 1, "Prepacked kernel VImage for non-group conv2d only");
  conv2d(
      output,
      input,
      kernelImage,
      bufferFromOptionalHostData(
          bias,
          sizeof(float) * params.OC,
          sizeof(float) * ALIGN_UP4(params.OC)),
      params,
      output_min,
      output_max);
}

void conv2d(
    VulkanTensor& output,
    const VulkanTensor& input,
    const VulkanTensor& weight_prepacked,
    c10::optional<const float*> bias,
    const Conv2DParams params,
    c10::optional<float> output_min,
    c10::optional<float> output_max) {
  if (params.G > 1) {
    conv2d_depthwise(
        output,
        input,
        weight_prepacked,
        bufferFromOptionalHostData(
            bias,
            sizeof(float) * params.OC,
            sizeof(float) * ALIGN_UP4(params.OC)),
        params,
        output_min,
        output_max);
    return;
  }

  conv2d(
      output,
      input,
      *(weight_prepacked.image()),
      bias,
      params,
      output_min,
      output_max);
}

void conv2d(
    VulkanTensor& output,
    const VulkanTensor& input,
    const VulkanTensor& weight_prepacked,
    const VulkanTensor& bias,
    const Conv2DParams params,
    c10::optional<float> output_min,
    c10::optional<float> output_max) {
  if (params.G > 1) {
    conv2d_depthwise(
        output,
        input,
        weight_prepacked,
        *(bias.buffer()),
        params,
        output_min,
        output_max);
    return;
  }

  conv2d(
      output,
      input,
      *(weight_prepacked.image()),
      *(bias.buffer()),
      params,
      output_min,
      output_max);
}

void conv2d(
    VulkanTensor& output,
    const VulkanTensor& input,
    const float* weight,
    const c10::optional<const float*> bias,
    const Conv2DParams params,
    c10::optional<float> output_min,
    c10::optional<float> output_max) {
  if (params.G > 1) {
    TORCH_INTERNAL_ASSERT(
        params.G == params.C,
        "Vulkan conv2d supports only no-group and depthwise");
    conv2d_depthwise(
        output, input, weight, bias, params, output_min, output_max);
    return;
  }

  conv2d(
      output,
      input,
      conv2d_prepack_weights_image(
          weight, params.OC, params.C, params.KH, params.KW),
      bias,
      params,
      output_min,
      output_max);
}

void clamp(
    VulkanTensor& output,
    const VulkanTensor& input,
    float min,
    float max) {
  auto sizes = output.sizes();
  auto C = sizes[0] * sizes[1];
  auto H = sizes[2];
  auto W = sizes[3];
  auto C_4 = UP_DIV(C, 4);

  auto device = context().device();
  auto physicalDevice = context().physicalDevice();
  struct ConstBlock {
    int32_t W;
    int32_t H;
    int32_t C_4;
    int32_t C;
    float min;
    float max;
  };
  ConstBlock cb{W, H, C_4, C, min, max};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  constBuffer.bind(descriptorSet, 2);

  WorkGroupSize workGroupSize{8, 8, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(clamp), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  auto commandBuffer = computeUnit.commandBuffer();
  output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
  input.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  computeUnit.dispatchCommandBuffer(W, H, C, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

void addmm(
    VulkanTensor& output,
    c10::optional<const VulkanTensor> t,
    const VulkanTensor& m1,
    const VulkanTensor& m2,
    float beta,
    float alpha) {
  bool hasT = t.has_value();
  auto m1Sizes = m1.sizes();
  auto m2Sizes = m2.sizes();
  TORCH_INTERNAL_ASSERT(m1Sizes.size() == 2);
  TORCH_INTERNAL_ASSERT(m2Sizes.size() == 2);
  uint32_t m1H = m1Sizes[0];
  uint32_t m1W = m1Sizes[1];
  uint32_t m1C = 1;
  uint32_t m1C_4 = UP_DIV(m1C, 4);

  uint32_t m2H = m2Sizes[0];
  uint32_t m2W = m2Sizes[1];
  uint32_t m2C = 1;
  uint32_t m2C_4 = UP_DIV(m2C, 4);

  uint32_t OH = m1Sizes[0];
  uint32_t OW = m2Sizes[1];

  TORCH_INTERNAL_ASSERT(m1W == m2H);
  TORCH_INTERNAL_ASSERT(m1C == m2C);

  uint32_t C = m1C;
  uint32_t C_4 = UP_DIV(C, 4);
  uint32_t K = m1W;

  auto device = context().device();

  struct ConstBlock {
    int32_t OW;
    int32_t OH;
    int32_t C_4;
    int32_t C;
    float beta;
    float alpha;
    int32_t K;
  };
  ConstBlock cb{OW, OH, C_4, C, beta, alpha, K};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{};
  if (hasT) {
    descriptorTypes = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    };
  } else {
    descriptorTypes = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    };
  }

  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  m1.image()->bindShaderRead(descriptorSet, 1);
  m2.image()->bindShaderRead(descriptorSet, 2);
  constBuffer.bind(descriptorSet, 3);
  if (hasT) {
    (*t).image()->bindShaderRead(descriptorSet, 4);
  }

  WorkGroupSize workGroupSize{8, 8, 1};
  if (hasT) {
    auto& computeUnit = context().computeUnitFactory().get(
        GLSL_SPV(addmm), descriptorSetLayout, workGroupSize);
    computeUnit.createCommandBuffer(descriptorSet);
    auto commandBuffer = computeUnit.commandBuffer();
    output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
    m1.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
    m2.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
    (*t).image()->addImageMemoryBarrierToShaderRead(commandBuffer);
    computeUnit.dispatchCommandBuffer(OW, OH, C_4, workGroupSize);
    computeUnit.endCommandBuffer();
    computeUnit.submitAndWaitCommandBuffer();
  } else {
    auto& computeUnit = context().computeUnitFactory().get(
        GLSL_SPV(mm), descriptorSetLayout, workGroupSize);
    computeUnit.createCommandBuffer(descriptorSet);
    auto commandBuffer = computeUnit.commandBuffer();
    output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
    m1.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
    m2.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
    computeUnit.dispatchCommandBuffer(OW, OH, C_4, workGroupSize);
    computeUnit.endCommandBuffer();
    computeUnit.submitAndWaitCommandBuffer();
  }
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

void mean(VulkanTensor& output, const VulkanTensor& input) {
  auto isizes = input.sizes();
  auto N = isizes[0];
  auto C = isizes[1];
  auto H = isizes[2];
  auto W = isizes[3];
  auto C_4 = UP_DIV(N * C, 4);

  auto device = context().device();
  auto physicalDevice = context().physicalDevice();
  struct ConstBlock {
    int32_t W;
    int32_t H;
    int32_t OW;
    int32_t OH;
  };
  ConstBlock cb{W, H, C, N};
  VBuffer constBuffer = makeUniformConstBuffer((void*)&cb, sizeof(cb));

  VkDescriptorSetLayout descriptorSetLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptorSet{};
  std::vector<VkDescriptorType> descriptorTypes{
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  createDescriptorSetLayoutSinglePool(
      device,
      descriptorTypes,
      &descriptorSetLayout,
      &descriptorPool,
      &descriptorSet);

  output.image()->bindStorageImage(descriptorSet, 0);
  input.image()->bindShaderRead(descriptorSet, 1);
  constBuffer.bind(descriptorSet, 2);

  WorkGroupSize workGroupSize{1, 1, 1};
  auto& computeUnit = context().computeUnitFactory().get(
      GLSL_SPV(mean), descriptorSetLayout, workGroupSize);
  computeUnit.createCommandBuffer(descriptorSet);
  auto commandBuffer = computeUnit.commandBuffer();
  output.image()->addImageMemoryBarrierToGeneral(commandBuffer);
  input.image()->addImageMemoryBarrierToShaderRead(commandBuffer);
  computeUnit.dispatchCommandBuffer(1, 1, C_4, workGroupSize);
  computeUnit.endCommandBuffer();
  computeUnit.submitAndWaitCommandBuffer();
  vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
}

} // namespace detail
} // namespace vulkan
} // namespace native
} // namespace at
