#include <etna/GlobalContext.hpp>

#include <array>
#include <vector>

#include <etna/DescriptorSet.hpp>
#include <etna/Etna.hpp>


namespace etna
{

bool DescriptorSet::isValid() const
{
  return get_context().getDescriptorPool().isSetValid(*this);
}

/*Todo: Add struct with parameters*/
static constexpr uint32_t NUM_DESCRIPTORS = 2048;

static constexpr uint32_t NUM_TEXTURES = 2048;
static constexpr uint32_t NUM_RW_TEXTURES = 512;
static constexpr uint32_t NUM_BUFFERS = 2048;
static constexpr uint32_t NUM_RW_BUFFERS = 512;
static constexpr uint32_t NUM_SAMPLERS = 128;  

static constexpr std::array<vk::DescriptorPoolSize, 6> DEFAULT_POOL_SIZES{
  vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, NUM_BUFFERS},
  vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, NUM_RW_BUFFERS},
  vk::DescriptorPoolSize{vk::DescriptorType::eSampler, NUM_SAMPLERS},
  vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, NUM_RW_TEXTURES},
  vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, NUM_RW_TEXTURES},
  vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, NUM_TEXTURES}};

DynamicDescriptorPool::DynamicDescriptorPool(vk::Device dev, const GpuWorkCount& work_count)
  : vkDevice{dev}
  , workCount{work_count}
  , pools{work_count, [dev](std::size_t) {
            vk::DescriptorPoolCreateInfo info{
              .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
              .maxSets = NUM_DESCRIPTORS,
              .poolSizeCount = static_cast<std::uint32_t>(DEFAULT_POOL_SIZES.size()),
              .pPoolSizes = DEFAULT_POOL_SIZES.data(),
            };
            return unwrap_vk_result(dev.createDescriptorPoolUnique(info));
          }}
{
}

void DynamicDescriptorPool::beginFrame()
{
  vkDevice.resetDescriptorPool(pools.get().get());
}

void DynamicDescriptorPool::destroyAllocatedSets()
{
  pools.iterate([this](auto& pool) { vkDevice.resetDescriptorPool(pool.get()); });
}

DescriptorSet DynamicDescriptorPool::allocateSet(
  DescriptorLayoutId layout_id,
  std::vector<Binding> bindings,
  vk::CommandBuffer command_buffer,
  BarrierBehavoir behavoir)
{
  auto& dslCache = get_context().getDescriptorSetLayouts();
  auto setLayouts = {dslCache.getVkLayout(layout_id)};

  vk::DescriptorSetAllocateInfo info{};
  info.setDescriptorPool(pools.get().get());
  info.setSetLayouts(setLayouts);

  std::vector<uint32_t> counts = {};
  counts.reserve(bindings.size());
  vk::DescriptorSetVariableDescriptorCountAllocateInfo countInfo{};
  if(bindings.size() == 1 && bindings.at(0).size > 1) {
    counts.push_back(bindings.at(0).size);
    countInfo.setDescriptorCounts(counts);
    info.setPNext(&countInfo);
  }

  vk::DescriptorSet vkSet{};
  ETNA_VERIFY(vkDevice.allocateDescriptorSets(&info, &vkSet) == vk::Result::eSuccess);
  return DescriptorSet{
    workCount.batchIndex(), layout_id, vkSet, std::move(bindings), command_buffer, behavoir};
}

static bool is_image_resource(vk::DescriptorType ds_type)
{
  switch (ds_type)
  {
  case vk::DescriptorType::eUniformBuffer:
  case vk::DescriptorType::eStorageBuffer:
  case vk::DescriptorType::eUniformBufferDynamic:
  case vk::DescriptorType::eStorageBufferDynamic:
    return false;
  case vk::DescriptorType::eCombinedImageSampler:
  case vk::DescriptorType::eSampledImage:
  case vk::DescriptorType::eStorageImage:
  case vk::DescriptorType::eSampler:
    return true;
  default:
    break;
  }

  ETNA_PANIC("Descriptor write error : unsupported resource {}", vk::to_string(ds_type));
}

static void validate_descriptor_write(const DescriptorSet& dst)
{
  const auto& layoutInfo = get_context().getDescriptorSetLayouts().getLayoutInfo(dst.getLayoutId());
  const auto& bindings = dst.getBindings();

  std::array<uint32_t, MAX_DESCRIPTOR_BINDINGS> unboundResources{};

  for (uint32_t binding = 0; binding < MAX_DESCRIPTOR_BINDINGS; binding++)
  {
    unboundResources[binding] =
      layoutInfo.isBindingUsed(binding) ? layoutInfo.getBinding(binding).descriptorCount : 0u;
  }

  for (const auto& binding : bindings)
  {
    if (!layoutInfo.isBindingUsed(binding.binding))
      ETNA_PANIC("Descriptor write error: descriptor set doesn't have {} slot", binding.binding);

    const auto& bindingInfo = layoutInfo.getBinding(binding.binding);
    bool isImageRequired = is_image_resource(bindingInfo.descriptorType);
    bool isImageBinding = std::get_if<std::vector<ImageBinding>>(&binding.resources) != nullptr;
    if (isImageRequired != isImageBinding)
    {
      ETNA_PANIC(
        "Descriptor write error: slot {} {} required but {} bound",
        binding.binding,
        (isImageRequired ? "image" : "buffer"),
        (isImageBinding ? "imaged" : "buffer"));
    }

    unboundResources[binding.binding] -= 1;
  }
#if 0
  for (uint32_t binding = 0; binding < MAX_DESCRIPTOR_BINDINGS; binding++)
  {
    if (unboundResources[binding] > 0)
      ETNA_PANIC(
        "Descriptor write error: slot {} has {} unbound resources",
        binding,
        unboundResources[binding]);
  }
#endif 
}

void write_set(const DescriptorSet& dst)
{
  ETNA_VERIFY(dst.isValid());
  validate_descriptor_write(dst);

  std::vector<vk::WriteDescriptorSet> writes;
  writes.reserve(dst.getBindings().size());

  uint32_t numBufferInfo = 0;
  uint32_t numImageInfo = 0;

  const auto& layoutInfo = get_context().getDescriptorSetLayouts().getLayoutInfo(dst.getLayoutId());

  for (auto& binding : dst.getBindings())
  {
    const auto& bindingInfo = layoutInfo.getBinding(binding.binding);
    if (is_image_resource(bindingInfo.descriptorType))
      numImageInfo  += binding.size;
    else
      numBufferInfo += binding.size;
  }

  std::vector<vk::DescriptorImageInfo> imageInfos;
  std::vector<vk::DescriptorBufferInfo> bufferInfos;
  imageInfos.resize(numImageInfo);
  bufferInfos.resize(numBufferInfo);
  numImageInfo = 0;
  numBufferInfo = 0;

  for (const auto& binding : dst.getBindings())
  {
    const auto& bindingInfo = layoutInfo.getBinding(binding.binding);
    vk::WriteDescriptorSet write{};
    write.setDstSet(dst.getVkSet())
      .setDescriptorCount(binding.size)
      .setDstBinding(binding.binding)
      .setDstArrayElement(binding.arrayElem)
      .setDescriptorType(bindingInfo.descriptorType);

    if (is_image_resource(bindingInfo.descriptorType))
    {
      write.setPImageInfo(imageInfos.data() + numImageInfo);
      const auto& imgs = std::get<std::vector<ImageBinding>>(binding.resources);
      for (const auto& img : imgs) {
        imageInfos[numImageInfo] = img.descriptor_info;
        numImageInfo++;
      }
    }
    else
    {
      const auto& bufs = std::get<std::vector<BufferBinding>>(binding.resources);
      write.setPBufferInfo(bufferInfos.data() + numBufferInfo);
      for (const auto& buf : bufs) {
        bufferInfos[numBufferInfo] = buf.descriptor_info;
        numBufferInfo++;
      }
    }

    writes.push_back(write);
  }

  get_context().getDevice().updateDescriptorSets(writes, {});
}

constexpr static vk::PipelineStageFlags2 shader_stage_to_pipeline_stage(
  vk::ShaderStageFlags shader_stages)
{
  constexpr uint32_t MAPPING_LENGTH = 6;
  constexpr std::array<vk::ShaderStageFlagBits, MAPPING_LENGTH> SHADER_STAGES = {
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eCompute,
  };
  constexpr std::array<vk::PipelineStageFlagBits2, MAPPING_LENGTH> PIPELINE_STAGES = {
    vk::PipelineStageFlagBits2::eVertexShader,
    vk::PipelineStageFlagBits2::eTessellationControlShader,
    vk::PipelineStageFlagBits2::eTessellationEvaluationShader,
    vk::PipelineStageFlagBits2::eGeometryShader,
    vk::PipelineStageFlagBits2::eFragmentShader,
    vk::PipelineStageFlagBits2::eComputeShader,
  };

  vk::PipelineStageFlags2 pipelineStages = vk::PipelineStageFlagBits2::eNone;
  for (uint32_t i = 0; i < MAPPING_LENGTH; ++i)
  {
    if (SHADER_STAGES[i] & shader_stages)
      pipelineStages |= PIPELINE_STAGES[i];
  }

  return pipelineStages;
}

constexpr static vk::AccessFlags2 descriptor_type_to_access_flag(vk::DescriptorType descriptor_type)
{
  constexpr uint32_t MAPPING_LENGTH = 3;
  constexpr std::array<vk::DescriptorType, MAPPING_LENGTH> DESCRIPTOR_TYPES = {
    vk::DescriptorType::eSampledImage,
    vk::DescriptorType::eStorageImage,
    vk::DescriptorType::eCombinedImageSampler,
  };
  constexpr std::array<vk::AccessFlags2, MAPPING_LENGTH> ACCESS_FLAGS = {
    vk::AccessFlagBits2::eShaderSampledRead,
    vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    vk::AccessFlagBits2::eShaderSampledRead,
  };
  for (uint32_t i = 0; i < MAPPING_LENGTH; ++i)
  {
    if (DESCRIPTOR_TYPES[i] == descriptor_type)
      return ACCESS_FLAGS[i];
  }
  return vk::AccessFlagBits2::eNone;
}

void DescriptorSet::processBarriers() const
{
  auto& layoutInfo = get_context().getDescriptorSetLayouts().getLayoutInfo(layoutId);
  for (auto& binding : bindings)
  {
    if (std::get_if<std::vector<ImageBinding>>(&binding.resources) == nullptr)
      continue; // Add processing for buffer here if you need.

    auto& bindingInfo = layoutInfo.getBinding(binding.binding);
    //FIXME: at(0)
    const ImageBinding& imgData = std::get<std::vector<ImageBinding>>(binding.resources).at(0);
    etna::set_state(
      command_buffer,
      imgData.image.get(),
      shader_stage_to_pipeline_stage(bindingInfo.stageFlags),
      descriptor_type_to_access_flag(bindingInfo.descriptorType),
      imgData.descriptor_info.imageLayout,
      imgData.image.getAspectMaskByFormat());
  }
}

} // namespace etna
