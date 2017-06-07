// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2015-2016 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <arch/arm64/mmu.h>
#include <kernel/vm/arch_vm_aspace.h>

#define ARCH_ASPACE_MAGIC 0x41524153 // ARAS

struct arch_aspace {
    /* magic value for use-after-free detection */
    uint32_t magic;

    uint16_t asid;

    /* pointer to the translation table */
    paddr_t tt_phys;
    volatile pte_t *tt_virt;

    uint flags;

    /* range of address space */
    vaddr_t base;
    size_t size;
};

class ARM64ArchVmAspace final : public ArchVmAspaceBase {
public:
    ARM64ArchVmAspace();
    virtual ~ARM64ArchVmAspace();

    DISALLOW_COPY_ASSIGN_AND_MOVE(ARM64ArchVmAspace);

    virtual status_t Init(vaddr_t base, size_t size, uint mmu_flags);
    virtual status_t Destroy();

    // main methods
    virtual status_t Map(vaddr_t vaddr, paddr_t paddr, size_t count, uint mmu_flags, size_t* mapped);
    virtual status_t Unmap(vaddr_t vaddr, size_t count, size_t* unmapped);
    virtual status_t Protect(vaddr_t vaddr, size_t count, uint mmu_flags);
    virtual status_t Query(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags);

    static void ContextSwitch(ARM64ArchVmAspace *from, ARM64ArchVmAspace *to);

    arch_aspace& GetInnerAspace() { return aspace_; }

private:
    arch_aspace aspace_ = {};
};

using ArchVmAspace = ARM64ArchVmAspace;
