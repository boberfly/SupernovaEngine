#include "renderer/WeightedBlendedPass.hpp"

#include "FrameGraphForwardPass.hpp"
#include "renderer/FrameGraphTexture.hpp"
#include "FrameGraphResourceAccess.hpp"

#include "FrameGraphData/GBuffer.hpp"
#include "FrameGraphData/WeightedBlended.hpp"
#include "FrameGraphData/SceneColor.hpp"

#include "renderer/CommonSamplers.hpp"

#include "MaterialShader.hpp"
#include "BatchBuilder.hpp"
#include "UploadInstances.hpp"

#include "RenderContext.hpp"

namespace gfx {

namespace {

[[nodiscard]] auto canDraw(const Renderable *renderable) {
  const auto &surface =
    *renderable->subMeshInstance.material->getBlueprint().surface;
  return surface.blendMode == BlendMode::Transparent;
};

[[nodiscard]] auto batchCompatible(const Batch &b, const Renderable &r) {
  return sameGeometry(b, r) && sameMaterial(b, r) && sameTextures(b, r);
}

} // namespace

//
// WeightedBlendedPass class:
//

WeightedBlendedPass::WeightedBlendedPass(rhi::RenderDevice &rd,
                                         const CommonSamplers &commonSamplers)
    : rhi::RenderPass<WeightedBlendedPass>{rd}, m_samplers{commonSamplers},
      m_compositionPass{rd} {}

uint32_t WeightedBlendedPass::count(PipelineGroups flags) const {
  uint32_t n{0};
  if (bool(flags & PipelineGroups::SurfaceMaterial)) {
    n += BasePass::count();
  }
  if (bool(flags & PipelineGroups::BuiltIn)) {
    n += m_compositionPass.count();
  }
  return n;
}
void WeightedBlendedPass::clear(PipelineGroups flags) {
  if (bool(flags & PipelineGroups::SurfaceMaterial)) BasePass::clear();
  if (bool(flags & PipelineGroups::BuiltIn)) m_compositionPass.clear();
}

void WeightedBlendedPass::addGeometryPass(
  FrameGraph &fg, FrameGraphBlackboard &blackboard, const ViewInfo &viewData,
  const PropertyGroupOffsets &propertyGroupOffsets,
  const LightingSettings &lightingSettings, bool softShadows) {
  constexpr auto kPassName = "WeightedBlended OIT";
  ZoneScopedN(kPassName);

  std::vector<const Renderable *> transparentRenderables;
  transparentRenderables.reserve(viewData.visibleRenderables.size());
  std::ranges::copy_if(viewData.visibleRenderables,
                       std::back_inserter(transparentRenderables), canDraw);
  if (transparentRenderables.empty()) return;

  std::vector<GPUInstance> gpuInstances;
  auto batches = buildBatches(gpuInstances, transparentRenderables,
                              propertyGroupOffsets, batchCompatible);
  if (batches.empty()) return;

  const auto instances = *uploadInstances(fg, std::move(gpuInstances));

  LightingPassFeatures features{.softShadows = softShadows};
  getLightingPassFeatures(features, blackboard);

  blackboard
    .add<WeightedBlendedData>() = fg.addCallbackPass<WeightedBlendedData>(
    kPassName,
    [&fg, &blackboard, instances](FrameGraph::Builder &builder,
                                  WeightedBlendedData &data) {
      PASS_SETUP_ZONE;

      read(builder, blackboard, instances);

      const auto sceneDepth = blackboard.get<GBufferData>().depth;
      const auto inputExtent =
        fg.getDescriptor<FrameGraphTexture>(sceneDepth).extent;

      data.accum = builder.create<FrameGraphTexture>(
        "Accum", {
                   .extent = inputExtent,
                   .format = rhi::PixelFormat::RGBA16F,
                   .usageFlags =
                     rhi::ImageUsage::RenderTarget | rhi::ImageUsage::Sampled,
                 });
      data.accum =
        builder.write(data.accum, Attachment{
                                    .index = 0,
                                    .clearValue = ClearValue::TransparentBlack,
                                  });

      data.reveal = builder.create<FrameGraphTexture>(
        "Reveal", {
                    .extent = inputExtent,
                    .format = rhi::PixelFormat::R8_UNorm,
                    .usageFlags =
                      rhi::ImageUsage::RenderTarget | rhi::ImageUsage::Sampled,
                  });
      data.reveal =
        builder.write(data.reveal, Attachment{
                                     .index = 1,
                                     .clearValue = ClearValue::OpaqueWhite,
                                   });
    },
    [this, lightingSettings, features, batches = std::move(batches)](
      const WeightedBlendedData &, const FrameGraphPassResources &, void *ctx) {
      auto &rc = *static_cast<RenderContext *>(ctx);
      auto &[cb, framebufferInfo, sets] = rc;
      RHI_GPU_ZONE(cb, kPassName);

      auto &samplerBindings = sets[0];
      samplerBindings[4] = rhi::bindings::SeparateSampler{m_samplers.shadow};
      samplerBindings[5] =
        rhi::bindings::SeparateSampler{m_samplers.omniShadow};

      overrideSampler(sets[1][5], m_samplers.bilinear);
      overrideSampler(sets[1][11], m_samplers.bilinear);

      BaseGeometryPassInfo passInfo{
        .depthFormat = rhi::getDepthFormat(*framebufferInfo),
        .colorFormats = rhi::getColorFormats(*framebufferInfo),
      };

      cb.beginRendering(*framebufferInfo);
      for (const auto &batch : batches) {
        const auto *pipeline =
          _getPipeline(ForwardPassInfo{adjust(passInfo, batch), features});
        if (pipeline) {
          bindBatch(rc, batch);
          cb.bindPipeline(*pipeline);
          bindDescriptorSets(rc, *pipeline);
          cb.pushConstants(rhi::ShaderStages::Fragment, 16, &lightingSettings);
          drawBatch(rc, batch);
        }
      }
      endRendering(rc);
    });
}

void WeightedBlendedPass::compose(FrameGraph &fg,
                                  FrameGraphBlackboard &blackboard) {
  if (const auto *d = blackboard.try_get<WeightedBlendedData>(); d) {
    auto &sceneColor = blackboard.get<SceneColorData>().HDR;
    sceneColor = m_compositionPass.addPass(fg, *d, sceneColor);
  }
}

CodePair WeightedBlendedPass::buildShaderCode(
  const rhi::RenderDevice &rd, const VertexFormat *vertexFormat,
  const Material &material, const LightingPassFeatures &features) {
  const auto offsetAlignment =
    rd.getDeviceLimits().minStorageBufferOffsetAlignment;

  CodePair code;

  const auto commonDefines = buildDefines(*vertexFormat);

  ShaderCodeBuilder shaderCodeBuilder;

  // -- VertexShader:

  shaderCodeBuilder.setDefines(commonDefines);
  addMaterial(shaderCodeBuilder, material, rhi::ShaderType::Vertex,
              offsetAlignment);
  code.vert = shaderCodeBuilder.buildFromFile("Mesh.vert");

  // -- FragmentShader:

  shaderCodeBuilder.setDefines(commonDefines)
    .addDefine("HAS_SCENE_DEPTH", 1)
    .addDefine("HAS_SCENE_COLOR", 1)
    .addDefine("WEIGHTED_BLENDED", 1);
  addMaterial(shaderCodeBuilder, material, rhi::ShaderType::Fragment,
              offsetAlignment);
  addLighting(shaderCodeBuilder, features);
  code.frag = shaderCodeBuilder.buildFromFile("ForwardPass.frag");

  return code;
}

//
// (private):
//

rhi::GraphicsPipeline
WeightedBlendedPass::_createPipeline(const ForwardPassInfo &passInfo) const {
  assert(passInfo.vertexFormat && passInfo.material);

  auto &rd = getRenderDevice();

  const auto &material = *passInfo.material;
  const auto [vertCode, fragCode] =
    buildShaderCode(rd, passInfo.vertexFormat, material, passInfo.features);

  const auto &surface = getSurface(material);

  return rhi::GraphicsPipeline::Builder{}
    .setDepthFormat(passInfo.depthFormat)
    .setColorFormats(passInfo.colorFormats)
    .setInputAssembly(passInfo.vertexFormat->getAttributes())
    .setTopology(passInfo.topology)
    .addShader(rhi::ShaderType::Vertex, vertCode)
    .addShader(rhi::ShaderType::Fragment, fragCode)

    .setDepthStencil({
      .depthTest = true,
      .depthWrite = false,
      .depthCompareOp = rhi::CompareOp::LessOrEqual,
    })
    .setRasterizer({
      .polygonMode = rhi::PolygonMode::Fill,
      .cullMode = surface.cullMode,
    })
    // clang-format off
    .setBlending(0, {
      .enabled = true,
      .srcColor = rhi::BlendFactor::One,
      .dstColor = rhi::BlendFactor::One,
      .srcAlpha = rhi::BlendFactor::One,
      .dstAlpha = rhi::BlendFactor::One,
    })
    .setBlending(1, {
      .enabled = true,
      .srcColor = rhi::BlendFactor::Zero,
      .dstColor = rhi::BlendFactor::OneMinusSrcColor,
      .srcAlpha = rhi::BlendFactor::Zero,
      .dstAlpha = rhi::BlendFactor::OneMinusSrcColor,
    })
    // clang-format on
    .build(rd);
}

} // namespace gfx
