/*
 * Copyright (C) 2013, Red Hat Inc, Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <linux/pci_regs.h>
#include "pci.h"
#include "asm/pci.h"

typedef void (*pci_cap_handler)(struct pci_dev *dev, int cap_offset);

static void pci_cap_msi_handler(struct pci_dev *dev, int cap_offset)
{
	printf("Detected MSI for device 0x%x offset 0x%x\n",
	       dev->pci_bdf, cap_offset);
	dev->msi_offset = cap_offset;
}

static pci_cap_handler cap_handlers[0xff] = {
	[PCI_CAP_ID_MSI] = pci_cap_msi_handler,
};

void pci_cap_walk(struct pci_dev *dev)
{
	uint8_t cap_offset;
	uint8_t cap_id;

	cap_offset = pci_config_readb(dev->pci_bdf, PCI_CAPABILITY_LIST);
	while (cap_offset) {
		cap_id = pci_config_readb(dev->pci_bdf, cap_offset);
		printf("PCI detected cap 0x%x\n", cap_id);
		if (cap_handlers[cap_id])
			cap_handlers[cap_id](dev, cap_offset);
		cap_offset = pci_config_readb(dev->pci_bdf, cap_offset + 1);
	}
}

int pci_setup_msi(struct pci_dev *dev, uint64_t msi_addr, uint32_t msi_data)
{
	uint16_t msi_control;
	uint16_t offset;
	pcidevaddr_t addr = dev->pci_bdf;

	assert(dev);

	if (!dev->msi_offset) {
		printf("MSI: dev 0x%x does not support MSI.\n", addr);
		return -1;
	}

	offset = dev->msi_offset;
	msi_control = pci_config_readw(addr, offset + PCI_MSI_FLAGS);
	pci_config_writel(addr, offset + PCI_MSI_ADDRESS_LO,
			  msi_addr & 0xffffffff);

	if (msi_control & PCI_MSI_FLAGS_64BIT) {
		pci_config_writel(addr, offset + PCI_MSI_ADDRESS_HI,
				  (uint32_t)(msi_addr >> 32));
		pci_config_writel(addr, offset + PCI_MSI_DATA_64, msi_data);
		printf("MSI: dev 0x%x init 64bit address: ", addr);
	} else {
		pci_config_writel(addr, offset + PCI_MSI_DATA_32, msi_data);
		printf("MSI: dev 0x%x init 32bit address: ", addr);
	}
	printf("addr=0x%lx, data=0x%x\n", msi_addr, msi_data);

	msi_control |= PCI_MSI_FLAGS_ENABLE;
	pci_config_writew(addr, offset + PCI_MSI_FLAGS, msi_control);

	return 0;
}

void pci_set_master(struct pci_dev *dev, int master)
{
	uint32_t val = pci_config_readw(dev->pci_bdf, PCI_COMMAND);
	val |= PCI_COMMAND_MASTER;
	pci_config_writew(dev->pci_bdf, PCI_COMMAND, val);
}

bool pci_dev_exists(pcidevaddr_t dev)
{
	return (pci_config_readw(dev, PCI_VENDOR_ID) != 0xffff &&
		pci_config_readw(dev, PCI_DEVICE_ID) != 0xffff);
}

/*
 * Scan bus look for a specific device. Only bus 0 scanned for now.
 * After the scan, a pci_dev is returned with correct BAR information.
 */
void pci_dev_init(struct pci_dev *dev, pcidevaddr_t bdf)
{
       memset(dev, 0, sizeof(*dev));
       dev->pci_bdf = bdf;
}

/* Scan bus look for a specific device. Only bus 0 scanned for now. */
int pci_find_dev(struct pci_dev *pci_dev, uint16_t vendor_id, uint16_t device_id)
{
	pcidevaddr_t dev;

	for (dev = 0; dev < PCI_DEVFN_MAX; ++dev) {
		if (pci_config_readw(dev, PCI_VENDOR_ID) == vendor_id &&
		    pci_config_readw(dev, PCI_DEVICE_ID) == device_id) {
			pci_dev_init(pci_dev, dev);
			return 0;
		}
	}

	return -1;
}

void pci_scan_bars(struct pci_dev *dev)
{
	int i = 0;

	for (i = 0; i < PCI_BAR_NUM; i++) {
		if (!pci_bar_is_valid(dev, i))
			continue;
		dev->pci_bar[i] = pci_bar_get_addr(dev, i);
		printf("PCI: init dev 0x%04x BAR %d [%s] addr 0x%lx\n",
		       dev->pci_bdf, i, pci_bar_is_memory(dev, i) ?
		       "MEM" : "IO", dev->pci_bar[i]);
	}
}

int pci_enable_defaults(struct pci_dev *dev)
{
	pci_scan_bars(dev);
	pci_set_master(dev, 1);
	pci_cap_walk(dev);
	return 0;
}

uint32_t pci_bar_mask(uint32_t bar)
{
	return (bar & PCI_BASE_ADDRESS_SPACE_IO) ?
		PCI_BASE_ADDRESS_IO_MASK : PCI_BASE_ADDRESS_MEM_MASK;
}

uint32_t pci_bar_get(struct pci_dev *dev, int bar_num)
{
	return pci_config_readl(dev->pci_bdf, PCI_BASE_ADDRESS_0 +
				bar_num * 4);
}

phys_addr_t pci_bar_get_addr(struct pci_dev *dev, int bar_num)
{
	uint32_t bar = pci_bar_get(dev, bar_num);
	uint32_t mask = pci_bar_mask(bar);
	uint64_t addr = bar & mask;

	if (pci_bar_is64(dev, bar_num))
		addr |= (uint64_t)pci_bar_get(dev, bar_num + 1) << 32;

	return pci_translate_addr(dev->pci_bdf, addr);
}

void pci_bar_set_addr(struct pci_dev *dev, int bar_num, phys_addr_t addr)
{
	int off = PCI_BASE_ADDRESS_0 + bar_num * 4;

	pci_config_writel(dev->pci_bdf, off, (uint32_t)addr);

	if (pci_bar_is64(dev, bar_num))
		pci_config_writel(dev->pci_bdf, off + 4,
				  (uint32_t)(addr >> 32));
}

/*
 * To determine the amount of address space needed by a PCI device,
 * one must save the original value of the BAR, write a value of
 * all 1's to the register, and then read it back. The amount of
 * memory can be then determined by masking the information bits,
 * performing a bitwise NOT, and incrementing the value by 1.
 *
 * The following pci_bar_size_helper() and pci_bar_size() functions
 * implement the algorithm.
 */
static uint32_t pci_bar_size_helper(struct pci_dev *dev, int bar_num)
{
	int off = PCI_BASE_ADDRESS_0 + bar_num * 4;
	uint16_t bdf = dev->pci_bdf;
	uint32_t bar, val;

	bar = pci_config_readl(bdf, off);
	pci_config_writel(bdf, off, ~0u);
	val = pci_config_readl(bdf, off);
	pci_config_writel(bdf, off, bar);

	return val;
}

phys_addr_t pci_bar_size(struct pci_dev *dev, int bar_num)
{
	uint32_t bar, size;

	size = pci_bar_size_helper(dev, bar_num);
	if (!size)
		return 0;

	bar = pci_bar_get(dev, bar_num);
	size &= pci_bar_mask(bar);

	if (pci_bar_is64(dev, bar_num)) {
		phys_addr_t size64 = pci_bar_size_helper(dev, bar_num + 1);
		size64 = (size64 << 32) | size;

		return ~size64 + 1;
	} else {
		return ~size + 1;
	}
}

bool pci_bar_is_memory(struct pci_dev *dev, int bar_num)
{
	uint32_t bar = pci_bar_get(dev, bar_num);

	return !(bar & PCI_BASE_ADDRESS_SPACE_IO);
}

bool pci_bar_is_valid(struct pci_dev *dev, int bar_num)
{
	return pci_bar_get(dev, bar_num);
}

bool pci_bar_is64(struct pci_dev *dev, int bar_num)
{
	uint32_t bar = pci_bar_get(dev, bar_num);

	if (bar & PCI_BASE_ADDRESS_SPACE_IO)
		return false;

	return (bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
		      PCI_BASE_ADDRESS_MEM_TYPE_64;
}

static void pci_dev_print(pcidevaddr_t dev)
{
	uint16_t vendor_id = pci_config_readw(dev, PCI_VENDOR_ID);
	uint16_t device_id = pci_config_readw(dev, PCI_DEVICE_ID);
	uint8_t header = pci_config_readb(dev, PCI_HEADER_TYPE);
	uint8_t progif = pci_config_readb(dev, PCI_CLASS_PROG);
	uint8_t subclass = pci_config_readb(dev, PCI_CLASS_DEVICE);
	uint8_t class = pci_config_readb(dev, PCI_CLASS_DEVICE + 1);
	struct pci_dev pci_dev;
	int i;

	printf("dev %2d fn %d vendor_id %04x device_id %04x type %02x "
	       "progif %02x class %02x subclass %02x\n",
	       dev / 8, dev % 8, vendor_id, device_id, header,
	       progif, class, subclass);

	if ((header & PCI_HEADER_TYPE_MASK) != PCI_HEADER_TYPE_NORMAL)
		return;

	pci_dev_init(&pci_dev, dev);

	for (i = 0; i < 6; i++) {
		phys_addr_t size, start, end;
		uint32_t bar;

		size = pci_bar_size(&pci_dev, i);
		if (!size)
			continue;

		start = pci_bar_get_addr(&pci_dev, i);
		end = start + size - 1;

		if (pci_bar_is64(&pci_dev, i)) {
			printf("\tBAR#%d,%d [%" PRIx64 "-%" PRIx64 " ",
			       i, i + 1, start, end);
			i++;
		} else {
			printf("\tBAR#%d    [%02x-%02x ",
			       i, (uint32_t)start, (uint32_t)end);
		}

		bar = pci_bar_get(&pci_dev, i);

		if (bar & PCI_BASE_ADDRESS_SPACE_IO) {
			printf("PIO]\n");
			continue;
		}

		printf("MEM");

		switch (bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK) {
		case PCI_BASE_ADDRESS_MEM_TYPE_32:
			printf("32");
			break;
		case PCI_BASE_ADDRESS_MEM_TYPE_1M:
			printf("1M");
			break;
		case PCI_BASE_ADDRESS_MEM_TYPE_64:
			printf("64");
			break;
		default:
			assert(0);
		}

		if (bar & PCI_BASE_ADDRESS_MEM_PREFETCH)
			printf("/p");

		printf("]\n");
	}
}

void pci_print(void)
{
	pcidevaddr_t dev;

	for (dev = 0; dev < 256; ++dev) {
		if (pci_dev_exists(dev))
			pci_dev_print(dev);
	}
}
