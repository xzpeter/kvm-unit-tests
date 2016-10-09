/*
 * Edu PCI device header.
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * Authors:
 *   Peter Xu <peterx@redhat.com>,
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or
 * later.
 *
 * Edu device is a virtualized device in QEMU. Please refer to
 * docs/specs/edu.txt in QEMU repository for EDU device manual.
 */
#ifndef __PCI_EDU_H__
#define __PCI_EDU_H__

#include "pci.h"

#define  PCI_VENDOR_ID_QEMU              (0x1234)
#define  PCI_DEVICE_ID_EDU               (0x11e8)

/* The only bar used by EDU device */
#define EDU_BAR_MEM                 (0)
#define EDU_MAGIC                   (0xed)
#define EDU_VERSION                 (0x100)
#define EDU_DMA_BUF_SIZE            (1 << 20)
#define EDU_INPUT_BUF_SIZE          (256)

#define EDU_REG_ID                  (0x0)
#define EDU_REG_ALIVE               (0x4)
#define EDU_REG_FACTORIAL           (0x8)
#define EDU_REG_STATUS              (0x20)
#define EDU_REG_DMA_SRC             (0x80)
#define EDU_REG_DMA_DST             (0x88)
#define EDU_REG_DMA_COUNT           (0x90)
#define EDU_REG_DMA_CMD             (0x98)

#define EDU_CMD_DMA_START           (0x01)
#define EDU_CMD_DMA_FROM            (0x02)
#define EDU_CMD_DMA_TO              (0x00)

#define EDU_STATUS_FACTORIAL        (0x1)
#define EDU_STATUS_INT_ENABLE       (0x80)

#define EDU_DMA_START               (0x40000)
#define EDU_DMA_SIZE_MAX            (4096)

struct pci_edu_dev {
	struct pci_dev pci_dev;
};

static inline uint64_t edu_reg_readq(struct pci_edu_dev *dev, int reg)
{
	return *(volatile uint64_t *)(dev->pci_dev.pci_bar[EDU_BAR_MEM] + reg);
}

static inline uint32_t edu_reg_read(struct pci_edu_dev *dev, int reg)
{
	return *(volatile uint32_t *)(dev->pci_dev.pci_bar[EDU_BAR_MEM] + reg);
}

static inline void edu_reg_writeq(struct pci_edu_dev *dev, int reg,
				  uint64_t val)
{
	*(volatile uint64_t *)(dev->pci_dev.pci_bar[EDU_BAR_MEM] + reg) = val;
}

static inline void edu_reg_write(struct pci_edu_dev *dev, int reg,
				 uint32_t val)
{
	*(volatile uint32_t *)(dev->pci_dev.pci_bar[EDU_BAR_MEM] + reg) = val;
}

int edu_init(struct pci_edu_dev *dev);
void edu_dma(struct pci_edu_dev *dev, iova_t iova,
	     size_t size, int dev_offset, bool from_device);

#endif
