// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <arch/guest_mmu.h>
#include <arch/aspace.h>
#include <kernel/vm/vm_object.h>
#include <mxtl/macros.h>
#include <mxtl/unique_ptr.h>

class GuestPhysicalAddressSpace {
public:
    static status_t Create(mxtl::RefPtr<VmObject> guest_phys_mem,
                           mxtl::unique_ptr<GuestPhysicalAddressSpace>* gpas);

    ~GuestPhysicalAddressSpace();

    status_t Init(size_t size);

    size_t size() const { return guest_phys_mem_->size(); }

    status_t UnmapRange(vaddr_t guest_paddr, size_t size);
    status_t GetPage(vaddr_t guest_paddr, paddr_t* host_paddr);

#if ARCH_X86_64
    paddr_t Pml4Address() { return aspace_.Pml4Address(); }
    status_t MapApicPage(vaddr_t guest_paddr, paddr_t host_paddr);
#endif

private:
    ArchVmGuestAspace aspace_;
    mxtl::RefPtr<VmObject> guest_phys_mem_;

    explicit GuestPhysicalAddressSpace(mxtl::RefPtr<VmObject> guest_phys_mem);
    DISALLOW_COPY_ASSIGN_AND_MOVE(GuestPhysicalAddressSpace);

    status_t MapRange(vaddr_t guest_paddr, size_t size);
};
