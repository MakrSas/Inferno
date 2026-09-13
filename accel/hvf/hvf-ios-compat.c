/*
 * Hypervisor.framework calls missing from the iOS reimplementation
 *
 * iPadOS up to 16.3.1 on M1 and M2 keeps the hypervisor in its kernel but ships
 * no Hypervisor.framework to call it with. The one bundled with the app is a
 * reimplementation over the kernel's hv_trap (utmapp/Hypervisor), and it covers
 * what macOS 11 and 12 offered. QEMU's hvf code calls a few functions from
 * later releases; they are built here out of what the reimplementation does
 * export. Only for iOS: on macOS the real framework has every one of them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE

#include "qemu/host-utils.h"
#include <Hypervisor/Hypervisor.h>
#include <pthread.h>
#include <sys/sysctl.h>

/* Exported by the reimplementation under the private names macOS 12 used. */
extern hv_return_t _hv_vm_config_set_ipa_size(hv_vm_config_t config, uint64_t ipa_size);
extern void*       _os_object_alloc(const void* cls, size_t size);

/*
 * The layout the reimplementation reads a vCPU configuration as: an OS object
 * header, then the PAC keys the VM is given. Zero keys are what a vCPU created
 * without a configuration gets.
 */
typedef struct
{
    char     header[16];
    uint64_t vmkeylo_el2;
    uint64_t vmkeyhi_el2;
} IOSVcpuConfig;

hv_vcpu_config_t hv_vcpu_config_create(void)
{
    IOSVcpuConfig* config = _os_object_alloc(NULL, sizeof(*config));

    config->vmkeylo_el2 = 0;
    config->vmkeyhi_el2 = 0;
    return (hv_vcpu_config_t)config;
}

/*
 * The feature registers, read from a vCPU.
 *
 * macOS answers these from a configuration alone. The reimplementation keeps
 * them per vCPU instead, filled from the kernel's capabilities when the vCPU is
 * created, and hands them out through hv_vcpu_get_sys_reg. So a vCPU is made
 * just to read them — on a thread of its own, because a vCPU belongs to the
 * thread that creates it and the caller may already own one. A failure is not
 * remembered: before the VM exists no vCPU can be created, and a later call
 * should get another try.
 */
static const struct
{
    hv_feature_reg_t feature;
    hv_sys_reg_t     sys;
} feature_regs[] = {
    {HV_FEATURE_REG_ID_AA64DFR0_EL1, HV_SYS_REG_ID_AA64DFR0_EL1},
    {HV_FEATURE_REG_ID_AA64DFR1_EL1, HV_SYS_REG_ID_AA64DFR1_EL1},
    {HV_FEATURE_REG_ID_AA64ISAR0_EL1, HV_SYS_REG_ID_AA64ISAR0_EL1},
    {HV_FEATURE_REG_ID_AA64ISAR1_EL1, HV_SYS_REG_ID_AA64ISAR1_EL1},
    {HV_FEATURE_REG_ID_AA64MMFR0_EL1, HV_SYS_REG_ID_AA64MMFR0_EL1},
    {HV_FEATURE_REG_ID_AA64MMFR1_EL1, HV_SYS_REG_ID_AA64MMFR1_EL1},
    {HV_FEATURE_REG_ID_AA64MMFR2_EL1, HV_SYS_REG_ID_AA64MMFR2_EL1},
    {HV_FEATURE_REG_ID_AA64PFR0_EL1, HV_SYS_REG_ID_AA64PFR0_EL1},
    {HV_FEATURE_REG_ID_AA64PFR1_EL1, HV_SYS_REG_ID_AA64PFR1_EL1},
};

static pthread_mutex_t feature_lock = PTHREAD_MUTEX_INITIALIZER;
static bool            feature_read;
static hv_return_t     feature_status;
static uint64_t        feature_values[ARRAY_SIZE(feature_regs)];

static void* read_feature_regs(void* opaque)
{
    hv_vcpu_t       vcpu;
    hv_vcpu_exit_t* exit;
    size_t          i;

    feature_status = hv_vcpu_create(&vcpu, &exit, NULL);
    if (feature_status != HV_SUCCESS) { return NULL; }

    for (i = 0; i < ARRAY_SIZE(feature_regs); i++) {
        feature_status |= hv_vcpu_get_sys_reg(vcpu, feature_regs[i].sys, &feature_values[i]);
    }
    hv_vcpu_destroy(vcpu);
    return NULL;
}

hv_return_t hv_vcpu_config_get_feature_reg(hv_vcpu_config_t config, hv_feature_reg_t feature_reg, uint64_t* value)
{
    hv_return_t ret = HV_BAD_ARGUMENT;
    pthread_t   thread;
    size_t      i;

    pthread_mutex_lock(&feature_lock);
    if (!feature_read) {
        if (pthread_create(&thread, NULL, read_feature_regs, NULL) == 0) {
            pthread_join(thread, NULL);
            feature_read = feature_status == HV_SUCCESS;
        } else {
            feature_status = HV_NO_RESOURCES;
        }
    }
    if (!feature_read) {
        ret = feature_status;
    } else {
        for (i = 0; i < ARRAY_SIZE(feature_regs); i++) {
            if (feature_regs[i].feature == feature_reg) {
                *value = feature_values[i];
                ret    = HV_SUCCESS;
                break;
            }
        }
    }
    pthread_mutex_unlock(&feature_lock);
    return ret;
}

/*
 * The IPA size.
 *
 * The kernel publishes what it supports for each page size; the smaller of the
 * two is what any VM can have. 36 bits is macOS's default, and what QEMU asks
 * for when a machine does not say.
 */
hv_return_t hv_vm_config_get_default_ipa_size(uint32_t* ipa_bit_length)
{
    *ipa_bit_length = 36;
    return HV_SUCCESS;
}

hv_return_t hv_vm_config_get_max_ipa_size(uint32_t* ipa_bit_length)
{
    uint64_t size_4k  = 0;
    uint64_t size_16k = 0;
    size_t   length   = sizeof(uint64_t);

    if (sysctlbyname("kern.hv.ipa_size_16k", &size_16k, &length, NULL, 0) != 0) { size_16k = 0; }
    length = sizeof(uint64_t);
    if (sysctlbyname("kern.hv.ipa_size_4k", &size_4k, &length, NULL, 0) != 0) { size_4k = 0; }
    if (size_4k == 0 && size_16k == 0) { return HV_UNSUPPORTED; }

    *ipa_bit_length = MIN(ctz64(size_16k), ctz64(size_4k));
    return HV_SUCCESS;
}

/* The private call takes the size in bytes; the public one in bits. */
hv_return_t hv_vm_config_set_ipa_size(hv_vm_config_t config, uint32_t ipa_bit_length)
{
    return _hv_vm_config_set_ipa_size(config, 1ULL << ipa_bit_length);
}

/*
 * Only the gdbstub asks for these. Execution time is reported as none, and
 * debug traps are left as the kernel sets them, which is off.
 */
hv_return_t hv_vcpu_get_exec_time(hv_vcpu_t vcpu, uint64_t* time)
{
    *time = 0;
    return HV_SUCCESS;
}

hv_return_t hv_vcpu_set_trap_debug_exceptions(hv_vcpu_t vcpu, bool value) { return HV_SUCCESS; }

hv_return_t hv_vcpu_set_trap_debug_reg_accesses(hv_vcpu_t vcpu, bool value) { return HV_SUCCESS; }

#endif /* TARGET_OS_IPHONE */
