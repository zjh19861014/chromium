// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image_factory.h"

#include <inttypes.h>

#include "base/strings/stringprintf.h"
#include "base/trace_event/memory_dump_manager.h"
#include "build/build_config.h"
#include "components/viz/common/resources/resource_format_utils.h"
#include "gpu/command_buffer/common/gpu_memory_buffer_support.h"
#include "gpu/command_buffer/common/shared_image_trace_utils.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/gles2_cmd_decoder.h"
#include "gpu/command_buffer/service/image_factory.h"
#include "gpu/command_buffer/service/mailbox_manager.h"
#include "gpu/command_buffer/service/service_utils.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image_backing_factory_gl_texture.h"
#include "gpu/command_buffer/service/shared_image_manager.h"
#include "gpu/command_buffer/service/shared_image_representation.h"
#include "gpu/command_buffer/service/wrapped_sk_image.h"
#include "gpu/config/gpu_preferences.h"
#include "ui/gl/trace_util.h"

#if (defined(USE_X11) || defined(OS_FUCHSIA)) && BUILDFLAG(ENABLE_VULKAN)
#include "gpu/command_buffer/service/external_vk_image_factory.h"
#elif defined(OS_ANDROID) && BUILDFLAG(ENABLE_VULKAN)
#include "gpu/command_buffer/service/shared_image_backing_factory_ahardwarebuffer.h"
#elif defined(OS_MACOSX)
#include "gpu/command_buffer/service/shared_image_backing_factory_iosurface.h"
#endif

namespace gpu {

// Overrides for flat_set lookups:
bool operator<(
    const std::unique_ptr<SharedImageRepresentationFactoryRef>& lhs,
    const std::unique_ptr<SharedImageRepresentationFactoryRef>& rhs) {
  return lhs->mailbox() < rhs->mailbox();
}

bool operator<(
    const Mailbox& lhs,
    const std::unique_ptr<SharedImageRepresentationFactoryRef>& rhs) {
  return lhs < rhs->mailbox();
}

bool operator<(const std::unique_ptr<SharedImageRepresentationFactoryRef>& lhs,
               const Mailbox& rhs) {
  return lhs->mailbox() < rhs;
}

SharedImageFactory::SharedImageFactory(
    const GpuPreferences& gpu_preferences,
    const GpuDriverBugWorkarounds& workarounds,
    const GpuFeatureInfo& gpu_feature_info,
    SharedContextState* context_state,
    MailboxManager* mailbox_manager,
    SharedImageManager* shared_image_manager,
    ImageFactory* image_factory,
    MemoryTracker* memory_tracker,
    bool is_using_skia_renderer)
    : mailbox_manager_(mailbox_manager),
      shared_image_manager_(shared_image_manager),
      memory_tracker_(std::make_unique<MemoryTypeTracker>(memory_tracker)),
      using_vulkan_(context_state && context_state->use_vulkan_gr_context()) {
  gl_backing_factory_ = std::make_unique<SharedImageBackingFactoryGLTexture>(
      gpu_preferences, workarounds, gpu_feature_info, image_factory);
  // For X11
#if (defined(USE_X11) || defined(OS_FUCHSIA)) && BUILDFLAG(ENABLE_VULKAN)
  if (using_vulkan_) {
    interop_backing_factory_ =
        std::make_unique<ExternalVkImageFactory>(context_state);
  }
#elif defined(OS_ANDROID) && BUILDFLAG(ENABLE_VULKAN)
  // For Android
  interop_backing_factory_ = std::make_unique<SharedImageBackingFactoryAHB>(
      workarounds, gpu_feature_info);
#elif defined(OS_MACOSX)
  // OSX
  DCHECK(!using_vulkan_);
  interop_backing_factory_ =
      std::make_unique<SharedImageBackingFactoryIOSurface>(workarounds,
                                                           gpu_feature_info);
#else
  // Others
  DCHECK(!using_vulkan_);
#endif
  // Certain test suites may enable UseSkiaRenderer feature flag, but never
  // create a SkiaRenderer. In this case context_state is nullptr and we should
  // not create a WrappedSkImageFactory.
  if (is_using_skia_renderer && context_state) {
    wrapped_sk_image_factory_ =
        std::make_unique<raster::WrappedSkImageFactory>(context_state);
  }
}

SharedImageFactory::~SharedImageFactory() {
  DCHECK(shared_images_.empty());
}

bool SharedImageFactory::CreateSharedImage(const Mailbox& mailbox,
                                           viz::ResourceFormat format,
                                           const gfx::Size& size,
                                           const gfx::ColorSpace& color_space,
                                           uint32_t usage) {
  bool allow_legacy_mailbox = false;
  auto* factory = GetFactoryByUsage(usage, &allow_legacy_mailbox);
  if (!factory)
    return false;
  auto backing = factory->CreateSharedImage(
      mailbox, format, size, color_space, usage, IsSharedBetweenThreads(usage));
  return RegisterBacking(std::move(backing), allow_legacy_mailbox);
}

bool SharedImageFactory::CreateSharedImage(const Mailbox& mailbox,
                                           viz::ResourceFormat format,
                                           const gfx::Size& size,
                                           const gfx::ColorSpace& color_space,
                                           uint32_t usage,
                                           base::span<const uint8_t> data) {
  bool allow_legacy_mailbox = false;
  auto* factory = GetFactoryByUsage(usage, &allow_legacy_mailbox);
  if (!factory)
    return false;
  auto backing = factory->CreateSharedImage(mailbox, format, size, color_space,
                                            usage, data);
  return RegisterBacking(std::move(backing), allow_legacy_mailbox);
}

bool SharedImageFactory::CreateSharedImage(const Mailbox& mailbox,
                                           int client_id,
                                           gfx::GpuMemoryBufferHandle handle,
                                           gfx::BufferFormat format,
                                           SurfaceHandle surface_handle,
                                           const gfx::Size& size,
                                           const gfx::ColorSpace& color_space,
                                           uint32_t usage) {
  // TODO(piman): depending on handle.type, choose platform-specific backing
  // factory, e.g. SharedImageBackingFactoryAHB.
  bool allow_legacy_mailbox = false;
  SharedImageBackingFactory* factory = nullptr;
  if (!using_vulkan_) {
    // GMB is only supported by gl backing factory when gl is being used.
    allow_legacy_mailbox = true;
    factory = gl_backing_factory_.get();
  } else {
    // TODO(penghuang): support GMB for vulkan.
    NOTIMPLEMENTED() << "GMB is not supported for vulkan.";
  }
  if (!factory)
    return false;
  auto backing =
      factory->CreateSharedImage(mailbox, client_id, std::move(handle), format,
                                 surface_handle, size, color_space, usage);
  return RegisterBacking(std::move(backing), allow_legacy_mailbox);
}

bool SharedImageFactory::UpdateSharedImage(const Mailbox& mailbox) {
  auto it = shared_images_.find(mailbox);
  if (it == shared_images_.end()) {
    LOG(ERROR) << "UpdateSharedImage: Could not find shared image mailbox";
    return false;
  }
  (*it)->Update();
  return true;
}

bool SharedImageFactory::DestroySharedImage(const Mailbox& mailbox) {
  auto it = shared_images_.find(mailbox);
  if (it == shared_images_.end()) {
    LOG(ERROR) << "DestroySharedImage: Could not find shared image mailbox";
    return false;
  }
  shared_images_.erase(it);
  return true;
}

void SharedImageFactory::DestroyAllSharedImages(bool have_context) {
  if (!have_context) {
    for (auto& shared_image : shared_images_)
      shared_image->OnContextLost();
  }
  shared_images_.clear();
}

// TODO(ericrk): Move this entirely to SharedImageManager.
bool SharedImageFactory::OnMemoryDump(
    const base::trace_event::MemoryDumpArgs& args,
    base::trace_event::ProcessMemoryDump* pmd,
    int client_id,
    uint64_t client_tracing_id) {
  for (const auto& shared_image : shared_images_) {
    shared_image_manager_->OnMemoryDump(shared_image->mailbox(), pmd, client_id,
                                        client_tracing_id);
  }

  return true;
}

bool SharedImageFactory::IsSharedBetweenThreads(uint32_t usage) {
  // If |shared_image_manager_| is thread safe, it means the display is running
  // on a separate thread (which uses a separate GL context or VkDeviceQueue).
  return shared_image_manager_->is_thread_safe() &&
         (usage & SHARED_IMAGE_USAGE_DISPLAY);
}

SharedImageBackingFactory* SharedImageFactory::GetFactoryByUsage(
    uint32_t usage,
    bool* allow_legacy_mailbox) {
  // wrapped_sk_image_factory_ is only used for OOPR and supports
  // a limited number of flags (e.g. no SHARED_IMAGE_USAGE_SCANOUT).
  constexpr auto kWrappedSkImageUsage = SHARED_IMAGE_USAGE_RASTER |
                                        SHARED_IMAGE_USAGE_OOP_RASTERIZATION |
                                        SHARED_IMAGE_USAGE_DISPLAY;
  bool using_wrapped_sk_image =
      wrapped_sk_image_factory_ && (usage == kWrappedSkImageUsage);
  bool using_dawn = usage & SHARED_IMAGE_USAGE_WEBGPU;

  bool vulkan_usage = using_vulkan_ && (usage & SHARED_IMAGE_USAGE_DISPLAY);
  bool gl_usage = usage & SHARED_IMAGE_USAGE_GLES2;
  bool share_between_threads = IsSharedBetweenThreads(usage);
  bool share_between_gl_vulkan = gl_usage && vulkan_usage;
  bool using_interop_factory = share_between_threads ||
                               share_between_gl_vulkan || using_dawn ||
                               (vulkan_usage && !using_wrapped_sk_image);

  *allow_legacy_mailbox =
      !using_wrapped_sk_image && !using_interop_factory && !using_vulkan_;

  if (using_wrapped_sk_image)
    return wrapped_sk_image_factory_.get();

  if (using_interop_factory) {
    LOG_IF(ERROR, !interop_backing_factory_)
        << "Unable to create SharedImage backing: GL / Vulkan interoperability "
           "is not supported on this platform";
    return interop_backing_factory_.get();
  }

  return gl_backing_factory_.get();
}

bool SharedImageFactory::RegisterBacking(
    std::unique_ptr<SharedImageBacking> backing,
    bool allow_legacy_mailbox) {
  if (!backing) {
    LOG(ERROR) << "CreateSharedImage: could not create backing.";
    return false;
  }

  std::unique_ptr<SharedImageRepresentationFactoryRef> shared_image =
      shared_image_manager_->Register(std::move(backing),
                                      memory_tracker_.get());

  if (!shared_image) {
    LOG(ERROR) << "CreateSharedImage: could not register backing.";
    return false;
  }

  // TODO(ericrk): Remove this once no legacy cases remain.
  if (allow_legacy_mailbox &&
      !shared_image->ProduceLegacyMailbox(mailbox_manager_)) {
    LOG(ERROR) << "CreateSharedImage: could not convert shared_image to legacy "
                  "mailbox.";
    return false;
  }

  shared_images_.emplace(std::move(shared_image));
  return true;
}

SharedImageRepresentationFactory::SharedImageRepresentationFactory(
    SharedImageManager* manager,
    MemoryTracker* tracker)
    : manager_(manager),
      tracker_(std::make_unique<MemoryTypeTracker>(tracker)) {}

SharedImageRepresentationFactory::~SharedImageRepresentationFactory() {
  DCHECK_EQ(0u, tracker_->GetMemRepresented());
}

std::unique_ptr<SharedImageRepresentationGLTexture>
SharedImageRepresentationFactory::ProduceGLTexture(const Mailbox& mailbox) {
  return manager_->ProduceGLTexture(mailbox, tracker_.get());
}

std::unique_ptr<SharedImageRepresentationGLTexture>
SharedImageRepresentationFactory::ProduceRGBEmulationGLTexture(
    const Mailbox& mailbox) {
  return manager_->ProduceRGBEmulationGLTexture(mailbox, tracker_.get());
}

std::unique_ptr<SharedImageRepresentationGLTexturePassthrough>
SharedImageRepresentationFactory::ProduceGLTexturePassthrough(
    const Mailbox& mailbox) {
  return manager_->ProduceGLTexturePassthrough(mailbox, tracker_.get());
}

std::unique_ptr<SharedImageRepresentationSkia>
SharedImageRepresentationFactory::ProduceSkia(
    const Mailbox& mailbox,
    scoped_refptr<SharedContextState> context_state) {
  return manager_->ProduceSkia(mailbox, tracker_.get(), context_state);
}

std::unique_ptr<SharedImageRepresentationDawn>
SharedImageRepresentationFactory::ProduceDawn(const Mailbox& mailbox,
                                              DawnDevice device) {
  return manager_->ProduceDawn(mailbox, tracker_.get(), device);
}

}  // namespace gpu
