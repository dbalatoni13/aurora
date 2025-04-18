#include "gx.hpp"

#include "../webgpu/gpu.hpp"
#include "../window.hpp"
#include "../internal.hpp"
#include "common.hpp"

#include <absl/container/flat_hash_map.h>
#include <cfloat>

using aurora::gfx::gx::g_gxState;
static aurora::Module Log("aurora::gx");

namespace aurora::gfx {
static Module Log("aurora::gfx::gx");

namespace gx {
using webgpu::g_device;
using webgpu::g_graphicsConfig;

GXState g_gxState{};

const TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }

static inline wgpu::BlendFactor to_blend_factor(GXBlendFactor fac, bool isDst) {
  switch (fac) {
    DEFAULT_FATAL("invalid blend factor {}", underlying(fac));
  case GX_BL_ZERO:
    return wgpu::BlendFactor::Zero;
  case GX_BL_ONE:
    return wgpu::BlendFactor::One;
  case GX_BL_SRCCLR: // + GX_BL_DSTCLR
    if (isDst) {
      return wgpu::BlendFactor::Src;
    } else {
      return wgpu::BlendFactor::Dst;
    }
  case GX_BL_INVSRCCLR: // + GX_BL_INVDSTCLR
    if (isDst) {
      return wgpu::BlendFactor::OneMinusSrc;
    } else {
      return wgpu::BlendFactor::OneMinusDst;
    }
  case GX_BL_SRCALPHA:
    return wgpu::BlendFactor::SrcAlpha;
  case GX_BL_INVSRCALPHA:
    return wgpu::BlendFactor::OneMinusSrcAlpha;
  case GX_BL_DSTALPHA:
    return wgpu::BlendFactor::DstAlpha;
  case GX_BL_INVDSTALPHA:
    return wgpu::BlendFactor::OneMinusDstAlpha;
  }
}

static inline wgpu::CompareFunction to_compare_function(GXCompare func) {
  switch (func) {
    DEFAULT_FATAL("invalid depth fn {}", underlying(func));
  case GX_NEVER:
    return wgpu::CompareFunction::Never;
  case GX_LESS:
    return UseReversedZ ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  case GX_EQUAL:
    return wgpu::CompareFunction::Equal;
  case GX_LEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual;
  case GX_GREATER:
    return UseReversedZ ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater;
  case GX_NEQUAL:
    return wgpu::CompareFunction::NotEqual;
  case GX_GEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::LessEqual : wgpu::CompareFunction::GreaterEqual;
  case GX_ALWAYS:
    return wgpu::CompareFunction::Always;
  }
}

static inline wgpu::BlendState to_blend_state(GXBlendMode mode, GXBlendFactor srcFac, GXBlendFactor dstFac,
                                              GXLogicOp op, u32 dstAlpha) {
  wgpu::BlendComponent colorBlendComponent;
  switch (mode) {
    DEFAULT_FATAL("unsupported blend mode {}", underlying(mode));
  case GX_BM_NONE:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
    break;
  case GX_BM_BLEND:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = to_blend_factor(srcFac, false),
        .dstFactor = to_blend_factor(dstFac, true),
    };
    break;
  case GX_BM_SUBTRACT:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::ReverseSubtract,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::One,
    };
    break;
  case GX_BM_LOGIC:
    switch (op) {
      DEFAULT_FATAL("unsupported logic op {}", underlying(op));
    case GX_LO_CLEAR:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_COPY:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_NOOP:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    }
    break;
  }
  wgpu::BlendComponent alphaBlendComponent{
      .operation = wgpu::BlendOperation::Add,
      .srcFactor = wgpu::BlendFactor::One,
      .dstFactor = wgpu::BlendFactor::Zero,
  };
  if (dstAlpha != UINT32_MAX) {
    alphaBlendComponent = wgpu::BlendComponent{
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::Constant,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
  }
  return {
      .color = colorBlendComponent,
      .alpha = alphaBlendComponent,
  };
}

static inline wgpu::ColorWriteMask to_write_mask(bool colorUpdate, bool alphaUpdate) {
  wgpu::ColorWriteMask writeMask = wgpu::ColorWriteMask::None;
  if (colorUpdate) {
    writeMask |= wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue;
  }
  if (alphaUpdate) {
    writeMask |= wgpu::ColorWriteMask::Alpha;
  }
  return writeMask;
}

static inline wgpu::PrimitiveState to_primitive_state(GXPrimitive gx_prim, GXCullMode gx_cullMode) {
  wgpu::PrimitiveTopology primitive = wgpu::PrimitiveTopology::TriangleList;
  switch (gx_prim) {
    DEFAULT_FATAL("unsupported primitive type {}", underlying(gx_prim));
  case GX_TRIANGLES:
    break;
  case GX_TRIANGLESTRIP:
    primitive = wgpu::PrimitiveTopology::TriangleStrip;
    break;
  }
  wgpu::CullMode cullMode = wgpu::CullMode::None;
  switch (gx_cullMode) {
    DEFAULT_FATAL("unsupported cull mode {}", underlying(gx_cullMode));
  case GX_CULL_FRONT:
    cullMode = wgpu::CullMode::Front;
    break;
  case GX_CULL_BACK:
    cullMode = wgpu::CullMode::Back;
    break;
  case GX_CULL_NONE:
    break;
  }
  return {
      .topology = primitive,
      .stripIndexFormat = primitive == wgpu::PrimitiveTopology::TriangleStrip ? wgpu::IndexFormat::Uint16
                                                                              : wgpu::IndexFormat::Undefined,
      .frontFace = wgpu::FrontFace::CW,
      .cullMode = cullMode,
  };
}

wgpu::RenderPipeline build_pipeline(const PipelineConfig& config, const ShaderInfo& info,
                                    ArrayRef<wgpu::VertexBufferLayout> vtxBuffers, wgpu::ShaderModule shader,
                                    const char* label) noexcept {
  const wgpu::DepthStencilState depthStencil{
      .format = g_graphicsConfig.depthFormat,
      .depthWriteEnabled = config.depthUpdate,
      .depthCompare = to_compare_function(config.depthFunc),
  };
  const auto blendState =
      to_blend_state(config.blendMode, config.blendFacSrc, config.blendFacDst, config.blendOp, config.dstAlpha);
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = g_graphicsConfig.surfaceConfiguration.format,
      .blend = &blendState,
      .writeMask = to_write_mask(config.colorUpdate, config.alphaUpdate),
  }};
  const wgpu::FragmentState fragmentState{
      .module = shader,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  auto layouts = build_bind_group_layouts(info, config.shaderConfig);
  const std::array bindGroupLayouts{
      layouts.uniformLayout,
      layouts.samplerLayout,
      layouts.textureLayout,
  };
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "GX Pipeline Layout",
      .bindGroupLayoutCount = static_cast<uint32_t>(info.sampledTextures.any() ? bindGroupLayouts.size() : 1),
      .bindGroupLayouts = bindGroupLayouts.data(),
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = pipelineLayout,
      .vertex =
          {
              .module = shader,
              .entryPoint = "vs_main",
              .bufferCount = static_cast<uint32_t>(vtxBuffers.size()),
              .buffers = vtxBuffers.data(),
          },
      .primitive = to_primitive_state(config.primitive, config.cullMode),
      .depthStencil = &depthStencil,
      .multisample =
          wgpu::MultisampleState{
              .count = g_graphicsConfig.msaaSamples,
              .mask = UINT32_MAX,
          },
      .fragment = &fragmentState,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  config.shaderConfig.fogType = g_gxState.fog.type;
  config.shaderConfig.vtxAttrs = g_gxState.vtxDesc;
  for (int i = 0; i < GX_VA_MAX_ATTR; ++i) {
    const auto type = g_gxState.vtxDesc[i];
    if (type != GX_INDEX8 && type != GX_INDEX16) {
      config.shaderConfig.attrMapping[i] = {};
      continue;
    }
    // Map attribute to its own storage
    config.shaderConfig.attrMapping[i] = StorageConfig{
        .attr = static_cast<GXAttr>(i),
        .cnt = vtxFmt.attrs[i].cnt,
        .compType = vtxFmt.attrs[i].type,
        .frac = vtxFmt.attrs[i].frac,
    };
  }
  config.shaderConfig.tevSwapTable = g_gxState.tevSwapTable;
  for (u8 i = 0; i < g_gxState.numTevStages; ++i) {
    config.shaderConfig.tevStages[i] = g_gxState.tevStages[i];
  }
  config.shaderConfig.tevStageCount = g_gxState.numTevStages;
  for (u8 i = 0; i < g_gxState.numChans * 2; ++i) {
    const auto& cc = g_gxState.colorChannelConfig[i];
    if (cc.lightingEnabled) {
      config.shaderConfig.colorChannels[i] = cc;
    } else {
      // Only matSrc matters when lighting disabled
      config.shaderConfig.colorChannels[i] = {
          .matSrc = cc.matSrc,
      };
    }
  }
  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {
    config.shaderConfig.tcgs[i] = g_gxState.tcgs[i];
  }
  if (g_gxState.alphaCompare) {
    config.shaderConfig.alphaCompare = g_gxState.alphaCompare;
  }
  config.shaderConfig.indexedAttributeCount =
      std::count_if(config.shaderConfig.vtxAttrs.begin(), config.shaderConfig.vtxAttrs.end(),
                    [](const auto type) { return type == GX_INDEX8 || type == GX_INDEX16; });
  for (u8 i = 0; i < MaxTextures; ++i) {
    const auto& bind = g_gxState.textures[i];
    TextureConfig texConfig{};
    if (bind.texObj.ref) {
      if (requires_copy_conversion(bind.texObj)) {
        texConfig.copyFmt = bind.texObj.ref->gxFormat;
      }
      if (requires_load_conversion(bind.texObj)) {
        texConfig.loadFmt = bind.texObj.fmt;
      }
      texConfig.renderTex = bind.texObj.ref->isRenderTexture;
    }
    config.shaderConfig.textureConfig[i] = texConfig;
  }
  config = {
      .shaderConfig = config.shaderConfig,
      .primitive = primitive,
      .depthFunc = g_gxState.depthFunc,
      .cullMode = g_gxState.cullMode,
      .blendMode = g_gxState.blendMode,
      .blendFacSrc = g_gxState.blendFacSrc,
      .blendFacDst = g_gxState.blendFacDst,
      .blendOp = g_gxState.blendOp,
      .dstAlpha = g_gxState.dstAlpha,
      .depthCompare = g_gxState.depthCompare,
      .depthUpdate = g_gxState.depthUpdate,
      .alphaUpdate = g_gxState.alphaUpdate,
      .colorUpdate = g_gxState.colorUpdate,
  };
}

Range build_uniform(const ShaderInfo& info) noexcept {
  auto [buf, range] = map_uniform(info.uniformSize);
  {
    buf.append(g_gxState.pnMtx[g_gxState.currentPnMtx]);
    buf.append(g_gxState.proj);
  }
  for (int i = 0; i < info.loadsTevReg.size(); ++i) {
    if (!info.loadsTevReg.test(i)) {
      continue;
    }
    buf.append(g_gxState.colorRegs[i]);
  }
  if (info.lightingEnabled) {
    // Lights
    static_assert(sizeof(g_gxState.lights) == 80 * GX::MaxLights);
    buf.append(g_gxState.lights);
    // Light state for all channels
    for (int i = 0; i < 4; ++i) {
      buf.append<u32>(g_gxState.colorChannelState[i].lightMask.to_ulong());
    }
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    const auto& ccc = g_gxState.colorChannelConfig[i];
    const auto& ccs = g_gxState.colorChannelState[i];
    if (ccc.lightingEnabled && ccc.ambSrc == GX_SRC_REG) {
      buf.append(ccs.ambColor);
    }
    if (ccc.matSrc == GX_SRC_REG) {
      buf.append(ccs.matColor);
    }
    const auto& ccca = g_gxState.colorChannelConfig[i + GX_ALPHA0];
    const auto& ccsa = g_gxState.colorChannelState[i + GX_ALPHA0];
    if (ccca.lightingEnabled && ccca.ambSrc == GX_SRC_REG) {
      buf.append(ccsa.ambColor);
    }
    if (ccca.matSrc == GX_SRC_REG) {
      buf.append(ccsa.matColor);
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (!info.sampledKColors.test(i)) {
      continue;
    }
    buf.append(g_gxState.kcolors[i]);
  }
  for (int i = 0; i < info.usesTexMtx.size(); ++i) {
    if (!info.usesTexMtx.test(i)) {
      continue;
    }
    switch (info.texMtxTypes[i]) {
      DEFAULT_FATAL("unhandled tex mtx type {}", underlying(info.texMtxTypes[i]));
    case GX_TG_MTX2x4:
      if (std::holds_alternative<Mat2x4<float>>(g_gxState.texMtxs[i])) {
        buf.append(std::get<Mat2x4<float>>(g_gxState.texMtxs[i]));
      } else
        UNLIKELY FATAL("expected 2x4 mtx in idx {}", i);
      break;
    case GX_TG_MTX3x4:
      if (std::holds_alternative<Mat3x4<float>>(g_gxState.texMtxs[i])) {
        buf.append(std::get<Mat3x4<float>>(g_gxState.texMtxs[i]));
      } else
        UNLIKELY FATAL("expected 3x4 mtx in idx {}", i);
      break;
    }
  }
  for (int i = 0; i < info.usesPTTexMtx.size(); ++i) {
    if (!info.usesPTTexMtx.test(i)) {
      continue;
    }
    buf.append(g_gxState.ptTexMtxs[i]);
  }
  if (info.usesFog) {
    const auto& state = g_gxState.fog;
    Fog fog{.color = state.color};
    if (state.nearZ != state.farZ && state.startZ != state.endZ) {
      const float depthRange = state.farZ - state.nearZ;
      const float fogRange = state.endZ - state.startZ;
      fog.a = (state.farZ * state.nearZ) / (depthRange * fogRange);
      fog.b = state.farZ / depthRange;
      fog.c = state.startZ / fogRange;
    }
    buf.append(fog);
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    const auto& tex = get_texture(static_cast<GXTexMapID>(i));
    CHECK(tex, "unbound texture {}", i);
    buf.append(tex.texObj.lodBias);
  }
  g_gxState.stateDirty = false;
  return range;
}

static absl::flat_hash_map<u32, wgpu::BindGroupLayout> sUniformBindGroupLayouts;
static absl::flat_hash_map<u32, std::pair<wgpu::BindGroupLayout, wgpu::BindGroupLayout>> sTextureBindGroupLayouts;

GXBindGroups build_bind_groups(const ShaderInfo& info, const ShaderConfig& config,
                               const BindGroupRanges& ranges) noexcept {
  const auto layouts = build_bind_group_layouts(info, config);

  std::array<wgpu::BindGroupEntry, GX_VA_MAX_ATTR + 1> uniformEntries;
  memset(&uniformEntries, 0, sizeof(uniformEntries));
  uniformEntries[0].binding = 0;
  uniformEntries[0].buffer = g_uniformBuffer;
  uniformEntries[0].size = info.uniformSize;
  u32 uniformBindIdx = 1;
  for (u32 i = 0; i < GX_VA_MAX_ATTR; ++i) {
    const Range& range = ranges.vaRanges[i];
    if (range.size <= 0) {
      continue;
    }
    wgpu::BindGroupEntry& entry = uniformEntries[uniformBindIdx];
    entry.binding = uniformBindIdx;
    entry.buffer = g_storageBuffer;
    entry.size = range.size;
    ++uniformBindIdx;
  }

  std::array<wgpu::BindGroupEntry, MaxTextures> samplerEntries;
  std::array<wgpu::BindGroupEntry, MaxTextures * 2> textureEntries;
  memset(&samplerEntries, 0, sizeof(samplerEntries));
  memset(&textureEntries, 0, sizeof(textureEntries));
  u32 samplerCount = 0;
  u32 textureCount = 0;
  for (u32 i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    const auto& tex = g_gxState.textures[i];
    CHECK(tex, "unbound texture {}", i);
    wgpu::BindGroupEntry& samplerEntry = samplerEntries[samplerCount];
    samplerEntry.binding = samplerCount;
    samplerEntry.size = wgpu::kWholeSize;
    samplerEntry.sampler = sampler_ref(tex.get_descriptor());
    ++samplerCount;
    wgpu::BindGroupEntry& textureEntry = textureEntries[textureCount];
    textureEntry.binding = textureCount;
    textureEntry.size = wgpu::kWholeSize;
    textureEntry.textureView = tex.texObj.ref->view;
    ++textureCount;
    // Load palette
    const auto& texConfig = config.textureConfig[i];
    if (is_palette_format(texConfig.loadFmt)) {
      u32 tlut = tex.texObj.tlut;
      CHECK(tlut >= GX_TLUT0 && tlut <= GX_BIGTLUT3, "tlut out of bounds {}", tlut);
      CHECK(g_gxState.tluts[tlut].ref, "tlut unbound {}", tlut);
      wgpu::BindGroupEntry& tlutEntry = textureEntries[textureCount];
      tlutEntry.binding = textureCount;
      tlutEntry.size = wgpu::kWholeSize;
      tlutEntry.textureView = g_gxState.tluts[tlut].ref->view;
      ++textureCount;
    }
  }
  const wgpu::BindGroupDescriptor uniformBindGroupDescriptor{
      .label = "GX Uniform Bind Group",
      .layout = layouts.uniformLayout,
      .entryCount = uniformBindIdx,
      .entries = uniformEntries.data(),
  };
  const wgpu::BindGroupDescriptor samplerBindGroupDescriptor{
      .label = "GX Sampler Bind Group",
      .layout = layouts.samplerLayout,
      .entryCount = samplerCount,
      .entries = samplerEntries.data(),
  };
  const wgpu::BindGroupDescriptor textureBindGroupDescriptor{
      .label = "GX Texture Bind Group",
      .layout = layouts.textureLayout,
      .entryCount = textureCount,
      .entries = textureEntries.data(),
  };
  return {
      .uniformBindGroup = bind_group_ref(uniformBindGroupDescriptor),
      .samplerBindGroup = bind_group_ref(samplerBindGroupDescriptor),
      .textureBindGroup = bind_group_ref(textureBindGroupDescriptor),
  };
}

GXBindGroupLayouts build_bind_group_layouts(const ShaderInfo& info, const ShaderConfig& config) noexcept {
  GXBindGroupLayouts out;

  Hasher uniformHasher;
  uniformHasher.update(info.uniformSize);
  uniformHasher.update(config.attrMapping);
  const auto uniformLayoutHash = uniformHasher.digest();
  auto it = sUniformBindGroupLayouts.find(uniformLayoutHash);
  if (it != sUniformBindGroupLayouts.end()) {
    out.uniformLayout = it->second;
  } else {
    std::array<wgpu::BindGroupLayoutEntry, GX_VA_MAX_ATTR + 1> uniformLayoutEntries{
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::Uniform,
                    .hasDynamicOffset = true,
                    .minBindingSize = info.uniformSize,
                },
        },
    };
    u32 bindIdx = 1;
    for (int i = 0; i < GX_VA_MAX_ATTR; ++i) {
      if (config.attrMapping[i].attr == static_cast<GXAttr>(i)) {
        uniformLayoutEntries[bindIdx] = wgpu::BindGroupLayoutEntry{
            .binding = bindIdx,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                    .hasDynamicOffset = true,
                },
        };
        ++bindIdx;
      }
    }
    const auto uniformLayoutDescriptor = wgpu::BindGroupLayoutDescriptor{
        .label = "GX Uniform Bind Group Layout",
        .entryCount = bindIdx,
        .entries = uniformLayoutEntries.data(),
    };
    out.uniformLayout = g_device.CreateBindGroupLayout(&uniformLayoutDescriptor);
    sUniformBindGroupLayouts[uniformLayoutHash] = out.uniformLayout;
  }

  Hasher textureHasher;
  textureHasher.update(info.sampledTextures);
  textureHasher.update(config.textureConfig);
  const auto textureLayoutHash = textureHasher.digest();
  auto it2 = sTextureBindGroupLayouts.find(textureLayoutHash);
  if (it2 != sTextureBindGroupLayouts.end()) {
    out.samplerLayout = it2->second.first;
    out.textureLayout = it2->second.second;
    return out;
  }

  u32 numSamplers = 0;
  u32 numTextures = 0;
  std::array<wgpu::BindGroupLayoutEntry, MaxTextures> samplerEntries;
  std::array<wgpu::BindGroupLayoutEntry, MaxTextures * 2> textureEntries;
  for (u32 i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    const auto& texConfig = config.textureConfig[i];
    bool copyAsPalette = is_palette_format(texConfig.copyFmt);
    bool loadAsPalette = is_palette_format(texConfig.loadFmt);
    samplerEntries[numSamplers] = {
        .binding = numSamplers,
        .visibility = wgpu::ShaderStage::Fragment,
        .sampler = {.type = copyAsPalette && loadAsPalette ? wgpu::SamplerBindingType::NonFiltering
                                                           : wgpu::SamplerBindingType::Filtering},
    };
    ++numSamplers;
    if (loadAsPalette) {
      textureEntries[numTextures] = {
          .binding = numTextures,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              {
                  .sampleType = copyAsPalette ? wgpu::TextureSampleType::Sint : wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      };
      ++numTextures;
      textureEntries[numTextures] = {
          .binding = numTextures,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              {
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      };
      ++numTextures;
    } else {
      textureEntries[numTextures] = {
          .binding = numTextures,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              {
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      };
      ++numTextures;
    }
  }
  {
    const wgpu::BindGroupLayoutDescriptor descriptor{
        .label = "GX Sampler Bind Group Layout",
        .entryCount = numSamplers,
        .entries = samplerEntries.data(),
    };
    out.samplerLayout = g_device.CreateBindGroupLayout(&descriptor);
  }
  {
    const wgpu::BindGroupLayoutDescriptor descriptor{
        .label = "GX Texture Bind Group Layout",
        .entryCount = numTextures,
        .entries = textureEntries.data(),
    };
    out.textureLayout = g_device.CreateBindGroupLayout(&descriptor);
  }
  sTextureBindGroupLayouts[textureLayoutHash] = {out.samplerLayout, out.textureLayout};
  return out;
}

// TODO this is awkward
extern absl::flat_hash_map<ShaderRef, std::pair<wgpu::ShaderModule, gx::ShaderInfo>> g_gxCachedShaders;
void shutdown() noexcept {
  // TODO we should probably store this all in g_state.gx instead
  sUniformBindGroupLayouts.clear();
  sTextureBindGroupLayouts.clear();
  for (auto& item : g_gxState.textures) {
    item.texObj.ref.reset();
  }
  for (auto& item : g_gxState.tluts) {
    item.ref.reset();
  }
  g_gxCachedShaders.clear();
  g_gxState.copyTextures.clear();
}
} // namespace gx

static wgpu::AddressMode wgpu_address_mode(GXTexWrapMode mode) {
  switch (mode) {
    DEFAULT_FATAL("invalid wrap mode {}", underlying(mode));
  case GX_CLAMP:
    return wgpu::AddressMode::ClampToEdge;
  case GX_REPEAT:
    return wgpu::AddressMode::Repeat;
  case GX_MIRROR:
    return wgpu::AddressMode::MirrorRepeat;
  }
}
static std::pair<wgpu::FilterMode, wgpu::MipmapFilterMode> wgpu_filter_mode(GXTexFilter filter) {
  switch (filter) {
    DEFAULT_FATAL("invalid filter mode {}", static_cast<int>(filter));
  case GX_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Linear};
  case GX_LINEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Linear};
  case GX_NEAR_MIP_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Nearest};
  case GX_LIN_MIP_NEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Nearest};
  case GX_NEAR_MIP_LIN:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Linear};
  case GX_LIN_MIP_LIN:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Linear};
  }
}
static u16 wgpu_aniso(GXAnisotropy aniso) {
  switch (aniso) {
    DEFAULT_FATAL("invalid aniso {}", static_cast<int>(aniso));
  case GX_ANISO_1:
    return 1;
  case GX_ANISO_2:
    return std::max<u16>(webgpu::g_graphicsConfig.textureAnisotropy / 2, 1);
  case GX_ANISO_4:
    return std::max<u16>(webgpu::g_graphicsConfig.textureAnisotropy, 1);
  }
}
wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept {
  if (gx::requires_copy_conversion(texObj) && gx::is_palette_format(texObj.ref->gxFormat)) {
    return {
        .label = "Generated Non-Filtering Sampler",
        .addressModeU = wgpu_address_mode(texObj.wrapS),
        .addressModeV = wgpu_address_mode(texObj.wrapT),
        .addressModeW = wgpu::AddressMode::Repeat,
        .magFilter = wgpu::FilterMode::Nearest,
        .minFilter = wgpu::FilterMode::Nearest,
        .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
        .maxAnisotropy = 1,
    };
  }
  const auto [minFilter, mipFilter] = wgpu_filter_mode(texObj.minFilter);
  const auto [magFilter, _] = wgpu_filter_mode(texObj.magFilter);
  return {
      .label = "Generated Filtering Sampler",
      .addressModeU = wgpu_address_mode(texObj.wrapS),
      .addressModeV = wgpu_address_mode(texObj.wrapT),
      .addressModeW = wgpu::AddressMode::Repeat,
      .magFilter = magFilter,
      .minFilter = minFilter,
      .mipmapFilter = mipFilter,
      .maxAnisotropy = wgpu_aniso(texObj.maxAniso),
  };
}
} // namespace aurora::gfx
