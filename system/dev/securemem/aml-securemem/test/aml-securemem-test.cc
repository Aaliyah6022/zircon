// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fake-bti/bti.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/fit/result.h>
#include <lib/zx/bti.h>
#include <lib/zx/interrupt.h>
#include <lib/zx/resource.h>
#include <zircon/limits.h>

#include <ddk/protocol/composite.h>
#include <ddk/protocol/platform/device.h>
#include <ddk/protocol/sysmem.h>
#include <ddktl/protocol/composite.h>
#include <ddktl/protocol/platform/device.h>
#include <ddktl/protocol/sysmem.h>
#include <fbl/array.h>
#include <zxtest/zxtest.h>

#include "device.h"

struct Context {
  amlogic_secure_mem::AmlogicSecureMemDevice* dev;
};

class Binder : public fake_ddk::Bind {
  zx_status_t DeviceRemove(zx_device_t* dev) override {
    Context* context = reinterpret_cast<Context*>(dev);
    if (context->dev != nullptr) {
      context->dev->DdkRelease();
    }
    context->dev = nullptr;
    return ZX_OK;
  }

  zx_status_t DeviceAdd(zx_driver_t* drv, zx_device_t* parent, device_add_args_t* args,
                        zx_device_t** out) override {
    *out = parent;
    Context* context = reinterpret_cast<Context*>(parent);
    context->dev = reinterpret_cast<amlogic_secure_mem::AmlogicSecureMemDevice*>(args->ctx);

    if (args && args->ops) {
      if (args->ops->message) {
        zx_status_t status;
        if ((status = fidl_.SetMessageOp(args->ctx, args->ops->message)) < 0) {
          return status;
        }
      }
    }
    return ZX_OK;
  }

  zx_status_t DeviceGetProtocol(const zx_device_t* device, uint32_t proto_id,
                                void* protocol) override {
    auto out = reinterpret_cast<fake_ddk::Protocol*>(protocol);
    for (const auto& proto : protocols_) {
      if (proto_id == proto.id) {
        out->ops = proto.proto.ops;
        out->ctx = proto.proto.ctx;
        return ZX_OK;
      }
    }
    return ZX_ERR_NOT_SUPPORTED;
  }
};

class FakePDev : public ddk::PDevProtocol<FakePDev, ddk::base_protocol> {
 public:
  FakePDev() : proto_({&pdev_protocol_ops_, this}) {}

  const pdev_protocol_t* proto() const { return &proto_; }

  zx_status_t PDevGetMmio(uint32_t index, pdev_mmio_t* out_mmio) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PDevGetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out_irq) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PDevGetBti(uint32_t index, zx::bti* out_bti) {
    zx_handle_t fake_bti;
    zx_status_t status = fake_bti_create(&fake_bti);
    if (status != ZX_OK) {
      return status;
    }

    *out_bti = zx::bti(fake_bti);
    return status;
  }

  zx_status_t PDevGetSmc(uint32_t index, zx::resource* out_resource) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PDevGetDeviceInfo(pdev_device_info_t* out_info) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PDevGetBoardInfo(pdev_board_info_t* out_info) { return ZX_ERR_NOT_SUPPORTED; }

 private:
  pdev_protocol_t proto_;
};

class FakeSysmem : public ddk::SysmemProtocol<FakeSysmem> {
 public:
  FakeSysmem() : proto_({&sysmem_protocol_ops_, this}) {}

  const sysmem_protocol_t* proto() const { return &proto_; }

  zx_status_t SysmemConnect(zx::channel allocator2_request) {
    // Currently, do nothing
    return ZX_OK;
  }

  zx_status_t SysmemRegisterHeap(uint64_t heap, zx::channel heap_connection) {
    // Currently, do nothing
    return ZX_OK;
  }

 private:
  sysmem_protocol_t proto_;
};

class FakeComposite : public ddk::CompositeProtocol<FakeComposite> {
 public:
  explicit FakeComposite(zx_device_t* parent)
      : proto_({&composite_protocol_ops_, this}), parent_(parent) {}

  const composite_protocol_t* proto() const { return &proto_; }

  uint32_t CompositeGetComponentCount() { return static_cast<uint32_t>(kNumComponents); }

  void CompositeGetComponents(zx_device_t** comp_list, size_t comp_count, size_t* comp_actual) {
    size_t comp_cur;

    for (comp_cur = 0; comp_cur < comp_count; comp_cur++) {
      comp_list[comp_cur] = parent_;
    }

    if (comp_actual != nullptr) {
      *comp_actual = comp_cur;
    }
  }

 private:
  static constexpr size_t kNumComponents = 2;

  composite_protocol_t proto_;
  zx_device_t* parent_;
};

class AmlogicSecureMemTest : public zxtest::Test {
 protected:
  AmlogicSecureMemTest() : composite_(parent()) {
    static constexpr size_t kNumBindProtocols = 3;

    fbl::Array<fake_ddk::ProtocolEntry> protocols(new fake_ddk::ProtocolEntry[kNumBindProtocols],
                                                  kNumBindProtocols);
    protocols[0] = {ZX_PROTOCOL_COMPOSITE,
                    *reinterpret_cast<const fake_ddk::Protocol*>(composite_.proto())};
    protocols[1] = {ZX_PROTOCOL_PDEV, *reinterpret_cast<const fake_ddk::Protocol*>(pdev_.proto())};
    protocols[2] = {ZX_PROTOCOL_SYSMEM,
                    *reinterpret_cast<const fake_ddk::Protocol*>(sysmem_.proto())};

    ddk_.SetProtocols(std::move(protocols));

    ASSERT_OK(amlogic_secure_mem::AmlogicSecureMemDevice::Create(nullptr, parent()));
  }

  zx_device_t* parent() { return reinterpret_cast<zx_device_t*>(&ctx_); }

  amlogic_secure_mem::AmlogicSecureMemDevice* dev() { return ctx_.dev; }

 private:
  Binder ddk_;
  FakePDev pdev_;
  FakeSysmem sysmem_;
  FakeComposite composite_;
  Context ctx_ = {};
};

TEST_F(AmlogicSecureMemTest, GetSecureMemoryPhysicalAddressBadVmo) {
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

  ASSERT_TRUE(dev()->GetSecureMemoryPhysicalAddress(std::move(vmo)).is_error());
}
