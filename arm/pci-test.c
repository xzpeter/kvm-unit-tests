/*
 * PCI bus operation test
 *
 * Copyright (C) 2016, Red Hat Inc, Alexander Gordeev <agordeev@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <libcflat.h>
#include <pci.h>

#define NR_TESTS (PCI_TESTDEV_NUM_BARS * PCI_TESTDEV_NUM_TESTS)

int main(void)
{
	int ret;

	if (!pci_probe())
		report_abort("PCI bus probing failed\n");

	pci_print();

	ret = pci_testdev();
	report("PCI test device passed %d/%d tests",
		ret >= NR_TESTS, ret > 0 ? ret : 0, NR_TESTS);

	return report_summary();
}
