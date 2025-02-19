#include "rhi/DescriptorSetBuilder.hpp"
#include "rhi/DescriptorSetAllocator.hpp"
#include "rhi/Buffer.hpp"
#include "rhi/Texture.hpp"
#include "math/Hash.hpp"
#include "VisitorHelper.hpp"
#include "tracy/Tracy.hpp"
#include <bit>

namespace rhi {

namespace {

[[nodiscard]] auto validRange(const Buffer &buffer, VkDeviceSize offset,
                              std::optional<VkDeviceSize> range) {
  const auto size = buffer.getSize();
  if (offset > size) return false;

  return !range || (offset + *range < size);
}

} // namespace

//
// DescriptorSetBuilder class:
//

DescriptorSetBuilder::DescriptorSetBuilder(
  VkDevice device, DescriptorSetAllocator &descriptorSetAllocator,
  DescriptorSetCache &cache)
    : m_device{device}, m_descriptorSetAllocator{descriptorSetAllocator},
      m_descriptorSetCache{cache} {
  m_bindings.reserve(10);
  m_descriptors.reserve(10);
}

DescriptorSetBuilder &DescriptorSetBuilder::bind(uint32_t index,
                                                 const ResourceBinding &r) {
  return std::visit(
    [this, index](auto &info) -> decltype(auto) { return bind(index, info); },
    r);
}

DescriptorSetBuilder &
DescriptorSetBuilder::bind(uint32_t index,
                           const bindings::SeparateSampler &info) {
  assert(info.handle != VK_NULL_HANDLE);

  m_bindings[index] = BindingInfo{
    .type = VK_DESCRIPTOR_TYPE_SAMPLER,
    .count = 1,
    .descriptorId = int32_t(m_descriptors.size()),
  };
  _addSampler(info.handle);
  return *this;
}
DescriptorSetBuilder &
DescriptorSetBuilder::bind(uint32_t index,
                           const bindings::CombinedImageSampler &info) {
  assert(info.texture && *info.texture);

  m_bindings[index] = BindingInfo{
    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .count = 1,
    .descriptorId = int32_t(m_descriptors.size()),
  };

  const auto sampler = info.sampler.value_or(info.texture->getSampler());
  assert(sampler != VK_NULL_HANDLE);
  const auto imageLayout = info.texture->getImageLayout();
  assert(imageLayout != ImageLayout::Undefined);

  m_descriptors.emplace_back(DescriptorVariant{
    .imageInfo =
      {
        .sampler = sampler,
        .imageView = info.texture->getImageView(),
        .imageLayout = VkImageLayout(imageLayout),
      },
  });
  return *this;
}
DescriptorSetBuilder &
DescriptorSetBuilder::bind(uint32_t index, const bindings::SampledImage &info) {
  assert(info.texture && *info.texture);

  m_bindings[index] = BindingInfo{
    .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    .count = 1,
    .descriptorId = int32_t(m_descriptors.size()),
  };
  _addImage(info.texture->getImageView(),
            static_cast<VkImageLayout>(info.texture->getImageLayout()));
  return *this;
}
DescriptorSetBuilder &
DescriptorSetBuilder::bind(uint32_t index, const bindings::StorageImage &info) {
  assert(info.texture && *info.texture);
  const auto imageLayout = info.texture->getImageLayout();
  assert(imageLayout == ImageLayout::General);

  const auto numImages = info.mipLevel ? 1 : info.texture->getNumMipLevels();

  m_bindings[index] = BindingInfo{
    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    .count = numImages,
    .descriptorId = int32_t(m_descriptors.size()),
  };
  for (auto i = 0; i < numImages; ++i) {
    _addImage(info.texture->getMipLevel(i),
              static_cast<VkImageLayout>(imageLayout));
  }
  return *this;
}
DescriptorSetBuilder &
DescriptorSetBuilder::bind(uint32_t index,
                           const bindings::UniformBuffer &info) {
  assert(info.buffer && validRange(*info.buffer, info.offset, info.range));
  return _bindBuffer(index, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     {
                       .buffer = info.buffer->getHandle(),
                       .offset = info.offset,
                       .range = info.range.value_or(VK_WHOLE_SIZE),
                     });
  return *this;
}
DescriptorSetBuilder &
DescriptorSetBuilder::bind(uint32_t index,
                           const bindings::StorageBuffer &info) {
  assert(info.buffer && validRange(*info.buffer, info.offset, info.range));
  return _bindBuffer(index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     {
                       .buffer = info.buffer->getHandle(),
                       .offset = info.offset,
                       .range = info.range.value_or(VK_WHOLE_SIZE),
                     });
  return *this;
}

VkDescriptorSet DescriptorSetBuilder::build(VkDescriptorSetLayout layout) {
  assert(layout != VK_NULL_HANDLE);

  ZoneScopedN("BuildDescriptorSet");
  auto hash = std::bit_cast<std::size_t>(layout);

  std::vector<VkWriteDescriptorSet> writeDescriptors;
  writeDescriptors.reserve(m_bindings.size());
  for (const auto &[index, binding] : m_bindings) {
    hashCombine(hash, index, binding.type);

    VkWriteDescriptorSet record{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstBinding = index,
      .descriptorCount = binding.count,
      .descriptorType = binding.type,
    };
    void *descriptorPtr = &m_descriptors[binding.descriptorId];
    switch (binding.type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      record.pImageInfo =
        std::bit_cast<const VkDescriptorImageInfo *>(descriptorPtr);
      hashCombine(hash, record.pImageInfo->imageView);
      break;

    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      record.pBufferInfo =
        std::bit_cast<const VkDescriptorBufferInfo *>(descriptorPtr);
      hashCombine(hash, record.pBufferInfo->offset, record.pBufferInfo->range,
                  record.pBufferInfo->buffer);
      break;

    default:
      assert(false);
    }
    writeDescriptors.emplace_back(std::move(record));
  }

  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  if (const auto it = m_descriptorSetCache.find(hash);
      it != m_descriptorSetCache.cend()) {
    descriptorSet = it->second;
  } else {
    descriptorSet = m_descriptorSetAllocator.allocate(layout);
    for (auto &record : writeDescriptors)
      record.dstSet = descriptorSet;

    vkUpdateDescriptorSets(m_device, uint32_t(writeDescriptors.size()),
                           writeDescriptors.data(), 0, nullptr);
    m_descriptorSetCache.emplace(hash, descriptorSet);
  }
  _clear();
  return descriptorSet;
}

void DescriptorSetBuilder::_clear() {
  m_bindings.clear();
  m_descriptors.clear();
}

void DescriptorSetBuilder::_addImage(VkImageView imageView,
                                     VkImageLayout imageLayout) {
  assert(imageView != VK_NULL_HANDLE &&
         imageLayout != VK_IMAGE_LAYOUT_UNDEFINED);
  m_descriptors.emplace_back(DescriptorVariant{
    .imageInfo =
      {
        .sampler = VK_NULL_HANDLE,
        .imageView = imageView,
        .imageLayout = imageLayout,
      },
  });
}
void DescriptorSetBuilder::_addSampler(VkSampler sampler) {
  assert(sampler != VK_NULL_HANDLE);
  m_descriptors.emplace_back(DescriptorVariant{
    .imageInfo =
      {
        .sampler = sampler,
        .imageView = VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      },
  });
}
void DescriptorSetBuilder::_addCombinedImageSampler(VkImageView imageView,
                                                    VkImageLayout imageLayout,
                                                    VkSampler sampler) {
  assert(imageView != VK_NULL_HANDLE &&
         imageLayout != VK_IMAGE_LAYOUT_UNDEFINED && sampler != VK_NULL_HANDLE);
  m_descriptors.emplace_back(DescriptorVariant{
    .imageInfo =
      {
        .sampler = sampler,
        .imageView = imageView,
        .imageLayout = imageLayout,
      },
  });
}

DescriptorSetBuilder &
DescriptorSetBuilder::_bindBuffer(uint32_t index, VkDescriptorType type,
                                  VkDescriptorBufferInfo &&bufferInfo) {
  m_bindings[index] = BindingInfo{
    .type = type,
    .count = 1,
    .descriptorId = int32_t(m_descriptors.size()),
  };
  m_descriptors.emplace_back(DescriptorVariant{
    .bufferInfo = std::move(bufferInfo),
  });
  return *this;
}

//
// Utility:
//

const char *toString(const rhi::ResourceBinding &rb) {
#define CASE(T) [](const rhi::bindings::T &) { return #T; }

  return std::visit(
    Overload{
      CASE(SeparateSampler),
      CASE(CombinedImageSampler),
      CASE(SampledImage),
      CASE(StorageImage),
      CASE(UniformBuffer),
      CASE(StorageBuffer),
    },
    rb);

#undef CASE
}

} // namespace rhi
