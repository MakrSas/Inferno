#pragma once

#include "hw/arm/dt.h"
#include "hw/misc/a7iop/base.h"
#include "hw/pci/pci.h"
#include "hw/sysbus.h"

SysBusDevice* apple_ans_from_node(AppleDTNode* node, AppleA7IOPVersion version, PCIBus* pci_bus);

// Rewrites the controller's namespace list in the device tree from the
// namespaces actually attached to it. Call it only while the tree is
// unfinalised and after every `-device` namespace has been created.
void apple_ans_sync_dt_namespaces(SysBusDevice* ans);
