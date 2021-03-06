// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/test/pixel_test.h"

#include "base/path_service.h"
#include "base/run_loop.h"
#include "cc/output/compositor_frame_metadata.h"
#include "cc/output/copy_output_request.h"
#include "cc/output/gl_renderer.h"
#include "cc/output/output_surface.h"
#include "cc/output/software_renderer.h"
#include "cc/resources/resource_provider.h"
#include "cc/test/paths.h"
#include "cc/test/pixel_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gl/gl_implementation.h"
#include "webkit/common/gpu/context_provider_in_process.h"
#include "webkit/common/gpu/webgraphicscontext3d_in_process_command_buffer_impl.h"

namespace cc {

class PixelTest::PixelTestRendererClient : public RendererClient {
 public:
  explicit PixelTestRendererClient(gfx::Rect device_viewport)
      : device_viewport_(device_viewport) {}

  // RendererClient implementation.
  virtual gfx::Rect DeviceViewport() const OVERRIDE {
    return device_viewport_ + test_expansion_offset_;
  }
  virtual float DeviceScaleFactor() const OVERRIDE {
    return 1.f;
  }
  virtual const LayerTreeSettings& Settings() const OVERRIDE {
    return settings_;
  }
  virtual void SetFullRootLayerDamage() OVERRIDE {}
  virtual void SetManagedMemoryPolicy(
      const ManagedMemoryPolicy& policy) OVERRIDE {}
  virtual void EnforceManagedMemoryPolicy(
      const ManagedMemoryPolicy& policy) OVERRIDE {}
  virtual bool HasImplThread() const OVERRIDE { return false; }
  virtual bool ShouldClearRootRenderPass() const OVERRIDE { return true; }
  virtual CompositorFrameMetadata MakeCompositorFrameMetadata() const
      OVERRIDE {
    return CompositorFrameMetadata();
  }
  virtual bool AllowPartialSwap() const OVERRIDE {
    return true;
  }

  void SetTestExpansionOffset(gfx::Vector2d test_expansion_offset) {
    test_expansion_offset_ = test_expansion_offset;
  }

 private:
  gfx::Rect device_viewport_;
  gfx::Vector2d test_expansion_offset_;
  LayerTreeSettings settings_;
};

class PixelTest::PixelTestOutputSurface : public OutputSurface {
 public:
  explicit PixelTestOutputSurface(
      scoped_ptr<WebKit::WebGraphicsContext3D> context3d)
      : OutputSurface(context3d.Pass()) {}
  explicit PixelTestOutputSurface(
      scoped_ptr<cc::SoftwareOutputDevice> software_device)
      : OutputSurface(software_device.Pass()) {}
  virtual void Reshape(gfx::Size size, float scale_factor) OVERRIDE {
    OutputSurface::Reshape(
        gfx::Size(size.width() + test_expansion_size_.width(),
                  size.height() + test_expansion_size_.height()),
        scale_factor);
  }

  void SetTestExpansionSize(gfx::Size test_expansion_size) {
    test_expansion_size_ = test_expansion_size;
  }

 private:
  gfx::Size test_expansion_size_;
};

class PixelTestSoftwareOutputDevice : public SoftwareOutputDevice {
 public:
  PixelTestSoftwareOutputDevice() {}
  virtual void Resize(gfx::Size size) OVERRIDE {
    SoftwareOutputDevice::Resize(
        gfx::Size(size.width() + test_expansion_size_.width(),
                  size.height() + test_expansion_size_.height()));
  }

  void SetTestExpansionSize(gfx::Size test_expansion_size) {
    test_expansion_size_ = test_expansion_size;
  }

 private:
  gfx::Size test_expansion_size_;
};

PixelTest::PixelTest()
    : device_viewport_size_(gfx::Size(200, 200)),
      fake_client_(
          new PixelTestRendererClient(gfx::Rect(device_viewport_size_))) {}

PixelTest::~PixelTest() {}

bool PixelTest::RunPixelTest(RenderPassList* pass_list,
                             const base::FilePath& ref_file,
                             const PixelComparator& comparator) {
  return RunPixelTestWithReadbackTarget(pass_list,
                                        pass_list->back(),
                                        ref_file,
                                        comparator);
}

bool PixelTest::RunPixelTestWithReadbackTarget(
    RenderPassList* pass_list,
    RenderPass* target,
    const base::FilePath& ref_file,
    const PixelComparator& comparator) {
  base::RunLoop run_loop;

  target->copy_requests.push_back(CopyOutputRequest::CreateBitmapRequest(
      base::Bind(&PixelTest::ReadbackResult,
                 base::Unretained(this),
                 run_loop.QuitClosure())));

  renderer_->DecideRenderPassAllocationsForFrame(*pass_list);
  renderer_->DrawFrame(pass_list);

  // Wait for the readback to complete.
  resource_provider_->Finish();
  run_loop.Run();

  return PixelsMatchReference(ref_file, comparator);
}

void PixelTest::ReadbackResult(base::Closure quit_run_loop,
                               scoped_ptr<SkBitmap> bitmap) {
  result_bitmap_ = bitmap.Pass();
  quit_run_loop.Run();
}

bool PixelTest::PixelsMatchReference(const base::FilePath& ref_file,
                                     const PixelComparator& comparator) {
  base::FilePath test_data_dir;
  if (!PathService::Get(cc::DIR_TEST_DATA, &test_data_dir))
    return false;

  // If this is false, we didn't set up a readback on a render pass.
  if (!result_bitmap_)
    return false;

  // To rebaseline:
  // return WritePNGFile(*result_bitmap_, test_data_dir.Append(ref_file), true);

  return MatchesPNGFile(*result_bitmap_,
                        test_data_dir.Append(ref_file),
                        comparator);
}

void PixelTest::SetUpGLRenderer(bool use_skia_gpu_backend) {
  CHECK(fake_client_);
  CHECK(gfx::InitializeGLBindings(gfx::kGLImplementationOSMesaGL));

  using webkit::gpu::WebGraphicsContext3DInProcessCommandBufferImpl;
  scoped_ptr<WebGraphicsContext3DInProcessCommandBufferImpl> context3d(
      WebGraphicsContext3DInProcessCommandBufferImpl::CreateOffscreenContext(
          WebKit::WebGraphicsContext3D::Attributes()));
  output_surface_.reset(new PixelTestOutputSurface(
      context3d.PassAs<WebKit::WebGraphicsContext3D>()));
  resource_provider_ = ResourceProvider::Create(output_surface_.get(), 0);
  renderer_ = GLRenderer::Create(fake_client_.get(),
                                 output_surface_.get(),
                                 resource_provider_.get(),
                                 0,
                                 use_skia_gpu_backend).PassAs<DirectRenderer>();

  scoped_refptr<webkit::gpu::ContextProviderInProcess> offscreen_contexts =
      webkit::gpu::ContextProviderInProcess::Create();
  ASSERT_TRUE(offscreen_contexts->BindToCurrentThread());
  resource_provider_->set_offscreen_context_provider(offscreen_contexts);
}

void PixelTest::ForceExpandedViewport(gfx::Size surface_expansion,
                                      gfx::Vector2d viewport_offset) {
  static_cast<PixelTestOutputSurface*>(output_surface_.get())
      ->SetTestExpansionSize(surface_expansion);
  fake_client_->SetTestExpansionOffset(viewport_offset);
  SoftwareOutputDevice* device = output_surface_->software_device();
  if (device) {
    static_cast<PixelTestSoftwareOutputDevice*>(device)
        ->SetTestExpansionSize(surface_expansion);
  }
}

void PixelTest::SetUpSoftwareRenderer() {
  CHECK(fake_client_);

  scoped_ptr<SoftwareOutputDevice> device(new PixelTestSoftwareOutputDevice());
  output_surface_.reset(new PixelTestOutputSurface(device.Pass()));
  resource_provider_ = ResourceProvider::Create(output_surface_.get(), 0);
  renderer_ = SoftwareRenderer::Create(
      fake_client_.get(),
      output_surface_.get(),
      resource_provider_.get()).PassAs<DirectRenderer>();
}

}  // namespace cc
