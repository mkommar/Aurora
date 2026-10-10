#include <assert.h>
#include <stdio.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64;
#include "../src/virtio_pci.h"

int main(void) {
    VirtioPciCapability common = {VIRTIO_PCI_CAP_COMMON, 0, 0x100, 0x38, 0};
    VirtioPciCapability notify = {VIRTIO_PCI_CAP_NOTIFY, 2, 0x200, 0x20, 4};
    VirtioPciCapability device = {VIRTIO_PCI_CAP_DEVICE, 4, 0x300, 0x100, 0};
    assert(virtio_pci_capabilities_complete(&common, &notify, &device));
    assert(virtio_pci_queue_bytes(8) == 4096);
    assert(virtio_pci_notify_address(&notify, 0xfee00000, 3) == 0xfee0020c);
    assert(virtio_pci_notify_address(&notify, 0xfee00000, 4) == 0xfee00210);
    assert(!virtio_pci_notify_address(&notify, 0xfee00000, 0x1000));
    notify.notify_multiplier = 0;
    assert(!virtio_pci_capabilities_complete(&common, &notify, &device));
    notify.notify_multiplier = 4;
    common.bar = 6;
    assert(!virtio_pci_capabilities_complete(&common, &notify, &device));
    puts("PASS modern VirtIO PCI capability validation and queue layout");
    return 0;
}
