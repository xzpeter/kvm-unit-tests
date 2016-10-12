/*
 * Intel IOMMU APIs
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * Authors:
 *   Peter Xu <peterx@redhat.com>,
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or
 * later.
 */

#include "intel-iommu.h"
#include "libcflat.h"
#include "pci.h"

/*
 * VT-d in QEMU currently only support 39 bits address width, which is
 * 3-level translation.
 */
#define VTD_PAGE_LEVEL      (3)
#define VTD_CE_AW_39BIT     (0x1)

typedef uint64_t vtd_pte_t;

struct vtd_root_entry {
	/* Quad 1 */
	uint64_t present:1;
	uint64_t __reserved:11;
	uint64_t context_table_p:52;
	/* Quad 2 */
	uint64_t __reserved_2;
} __attribute__ ((packed));
typedef struct vtd_root_entry vtd_re_t;

struct vtd_context_entry {
	/* Quad 1 */
	uint64_t present:1;
	uint64_t disable_fault_report:1;
	uint64_t trans_type:2;
	uint64_t __reserved:8;
	uint64_t slptptr:52;
	/* Quad 2 */
	uint64_t addr_width:3;
	uint64_t __ignore:4;
	uint64_t __reserved_2:1;
	uint64_t domain_id:16;
	uint64_t __reserved_3:40;
} __attribute__ ((packed));
typedef struct vtd_context_entry vtd_ce_t;

struct vtd_irte {
	uint32_t present:1;
	uint32_t fault_disable:1;    /* Fault Processing Disable */
	uint32_t dest_mode:1;        /* Destination Mode */
	uint32_t redir_hint:1;       /* Redirection Hint */
	uint32_t trigger_mode:1;     /* Trigger Mode */
	uint32_t delivery_mode:3;    /* Delivery Mode */
	uint32_t __avail:4;          /* Available spaces for software */
	uint32_t __reserved_0:3;     /* Reserved 0 */
	uint32_t irte_mode:1;        /* IRTE Mode */
	uint32_t vector:8;           /* Interrupt Vector */
	uint32_t __reserved_1:8;     /* Reserved 1 */
	uint32_t dest_id;            /* Destination ID */
	uint16_t source_id:16;       /* Source-ID */
	uint64_t sid_q:2;            /* Source-ID Qualifier */
	uint64_t sid_vtype:2;        /* Source-ID Validation Type */
	uint64_t __reserved_2:44;    /* Reserved 2 */
} __attribute__ ((packed));
typedef struct vtd_irte vtd_irte_t;

#define ONE_BIT_ONLY(x) ((x) && !((x) & ((x) - 1)))

#define VTD_RTA_MASK  (PAGE_MASK)
#define VTD_IRTA_MASK (PAGE_MASK)

static uint64_t vtd_root_table(void)
{
       /* No extend root table support yet */
       return vtd_readq(DMAR_RTADDR_REG) & VTD_RTA_MASK;
}

static uint64_t vtd_ir_table(void)
{
	return vtd_readq(DMAR_IRTA_REG) & VTD_IRTA_MASK;
}

static void vtd_gcmd_or(uint32_t cmd)
{
	uint32_t status;

	/* We only allow set one bit for each time */
	assert(ONE_BIT_ONLY(cmd));

	status = vtd_readl(DMAR_GSTS_REG);
	vtd_writel(DMAR_GCMD_REG, status | cmd);

	if (cmd & VTD_GCMD_ONE_SHOT_BITS)
		/* One-shot bits are taking effect immediately */
		return;

	/* Make sure IOMMU handled our command request */
	while (!(vtd_readl(DMAR_GSTS_REG) & cmd));
}

static void vtd_dump_init_info(void)
{
	printf("VT-d version:   0x%x\n", vtd_readl(DMAR_VER_REG));
	printf("     cap:       0x%016lx\n", vtd_readq(DMAR_CAP_REG));
	printf("     ecap:      0x%016lx\n", vtd_readq(DMAR_ECAP_REG));
}

static void vtd_setup_root_table(void)
{
	void *root = alloc_page();

	memset(root, 0, PAGE_SIZE);
	vtd_writeq(DMAR_RTADDR_REG, virt_to_phys(root));
	vtd_gcmd_or(VTD_GCMD_ROOT);
	printf("DMAR table address: 0x%016lx\n", vtd_root_table());
}

static void vtd_setup_ir_table(void)
{
	void *root = alloc_page();

	memset(root, 0, PAGE_SIZE);
	/* 0xf stands for table size (2^(0xf+1) == 65536) */
	vtd_writeq(DMAR_IRTA_REG, virt_to_phys(root) | 0xf);
	vtd_gcmd_or(VTD_GCMD_IR_TABLE);
	printf("IR table address: 0x%016lx\n", vtd_ir_table());
}

static void vtd_install_pte(vtd_pte_t *root, iova_t iova,
			    phys_addr_t pa, int level_target)
{
	int level;
	unsigned int offset;
	void *page;

	for (level = VTD_PAGE_LEVEL; level > level_target; level--) {
		offset = PGDIR_OFFSET(iova, level);
		if (!(root[offset] & VTD_PTE_RW)) {
			page = alloc_page();
			memset(page, 0, PAGE_SIZE);
			root[offset] = virt_to_phys(page) | VTD_PTE_RW;
		}
		root = (uint64_t *)(root[offset] & VTD_PTE_ADDR);
	}

	offset = PGDIR_OFFSET(iova, level);
	root[offset] = pa | VTD_PTE_RW;
	if (level != 1)
		/* This is huge page */
		root[offset] |= VTD_PTE_HUGE;
}

#define  VTD_FETCH_VIRT_ADDR(x) \
	((void *)(((uint64_t)phys_to_virt(x)) >> PAGE_SHIFT))

/**
 * vtd_map_range: setup IO address mapping for specific memory range
 *
 * @sid: source ID of the device to setup
 * @iova: start IO virtual address
 * @pa: start physical address
 * @size: size of the mapping area
 */
void vtd_map_range(uint16_t sid, iova_t iova, phys_addr_t pa, size_t size)
{
	uint8_t bus_n, devfn;
	void *slptptr;
	vtd_ce_t *ce;
	vtd_re_t *re = phys_to_virt(vtd_root_table());

	assert(IS_ALIGNED(iova, SZ_4K));
	assert(IS_ALIGNED(pa, SZ_4K));
	assert(IS_ALIGNED(size, SZ_4K));

	bus_n = PCI_BDF_GET_BUS(sid);
	devfn = PCI_BDF_GET_DEVFN(sid);

	/* Point to the correct root entry */
	re += bus_n;

	if (!re->present) {
		ce = alloc_page();
		memset(ce, 0, PAGE_SIZE);
		memset(re, 0, sizeof(*re));
		re->context_table_p = virt_to_phys(ce) >> PAGE_SHIFT;
		re->present = 1;
		printf("allocated vt-d root entry for PCI bus %d\n",
		       bus_n);
	} else
		ce = VTD_FETCH_VIRT_ADDR(re->context_table_p);

	/* Point to the correct context entry */
	ce += devfn;

	if (!ce->present) {
		slptptr = alloc_page();
		memset(slptptr, 0, PAGE_SIZE);
		memset(ce, 0, sizeof(*ce));
		/* To make it simple, domain ID is the same as SID */
		ce->domain_id = sid;
		/* We only test 39 bits width case (3-level paging) */
		ce->addr_width = VTD_CE_AW_39BIT;
		ce->slptptr = virt_to_phys(slptptr) >> PAGE_SHIFT;
		ce->trans_type = VTD_CONTEXT_TT_MULTI_LEVEL;
		ce->present = 1;
		/* No error reporting yet */
		ce->disable_fault_report = 1;
		printf("allocated vt-d context entry for devfn 0x%x\n",
		       devfn);
	} else
		slptptr = VTD_FETCH_VIRT_ADDR(ce->slptptr);

	while (size) {
		/* TODO: currently we only map 4K pages (level = 1) */
		printf("map 4K page IOVA 0x%lx to 0x%lx (sid=0x%04x)\n",
		       iova, pa, sid);
		vtd_install_pte(slptptr, iova, pa, 1);
		size -= PAGE_SIZE;
		iova += PAGE_SIZE;
		pa += PAGE_SIZE;
	}
}

static uint16_t vtd_intr_index_alloc(void)
{
	static int index_ctr = 0;
	assert(index_ctr < 65535);
	return index_ctr++;
}

static void vtd_setup_irte(struct pci_dev *dev, vtd_irte_t *irte,
			   int vector, int dest_id)
{
	assert(sizeof(vtd_irte_t) == 16);
	memset(irte, 0, sizeof(*irte));
	irte->fault_disable = 1;
	irte->dest_mode = 0;	 /* physical */
	irte->trigger_mode = 0;	 /* edge */
	irte->delivery_mode = 0; /* fixed */
	irte->irte_mode = 0;	 /* remapped */
	irte->vector = vector;
	irte->dest_id = dest_id;
	irte->source_id = dev->pci_bdf;
	irte->sid_q = 0;
	irte->sid_vtype = 1;     /* full-sid verify */
	irte->present = 1;
}

struct vtd_msi_addr {
	uint32_t __dont_care:2;
	uint32_t handle_15:1;	 /* handle[15] */
	uint32_t shv:1;
	uint32_t interrupt_format:1;
	uint32_t handle_0_14:15; /* handle[0:14] */
	uint32_t head:12;	 /* 0xfee */
	uint32_t addr_hi;	 /* not used except with x2apic */
} __attribute__ ((packed));
typedef struct vtd_msi_addr vtd_msi_addr_t;

struct vtd_msi_data {
	uint16_t __reserved;
	uint16_t subhandle;
} __attribute__ ((packed));
typedef struct vtd_msi_data vtd_msi_data_t;

/**
 * vtd_setup_msi - setup MSI message for a device
 *
 * @dev: PCI device to setup MSI
 * @vector: interrupt vector
 * @dest_id: destination processor
 */
int vtd_setup_msi(struct pci_dev *dev, int vector, int dest_id)
{
	vtd_msi_data_t msi_data = {};
	vtd_msi_addr_t msi_addr = {};
	vtd_irte_t *irte = phys_to_virt(vtd_ir_table());
	uint16_t index = vtd_intr_index_alloc();

	assert(sizeof(vtd_msi_addr_t) == 8);
	assert(sizeof(vtd_msi_data_t) == 4);

	printf("INTR: setup IRTE index %d\n", index);
	vtd_setup_irte(dev, irte + index, vector, dest_id);

	msi_addr.handle_15 = index >> 15 & 1;
	msi_addr.shv = 0;
	msi_addr.interrupt_format = 1;
	msi_addr.handle_0_14 = index & 0x7fff;
	msi_addr.head = 0xfee;
	msi_data.subhandle = 0;

	return pci_setup_msi(dev, *(uint64_t *)&msi_addr,
			     *(uint32_t *)&msi_data);
}

void vtd_init(void)
{
	setup_vm();
	smp_init();

	vtd_dump_init_info();
	vtd_gcmd_or(VTD_GCMD_QI); /* Enable QI */
	vtd_setup_root_table();
	vtd_setup_ir_table();
	vtd_gcmd_or(VTD_GCMD_DMAR); /* Enable DMAR */
	vtd_gcmd_or(VTD_GCMD_IR);   /* Enable IR */
}
