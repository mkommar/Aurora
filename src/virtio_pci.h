#ifndef AURORA_VIRTIO_PCI_H
#define AURORA_VIRTIO_PCI_H

/* VirtIO 1.x PCI capability types used by the block driver. */
#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2
#define VIRTIO_PCI_CAP_ISR 3
#define VIRTIO_PCI_CAP_DEVICE 4
#define VIRTIO_F_VERSION_1 32

typedef struct {
    u8 type;
    u8 bar;
    u32 offset;
    u32 length;
    u32 notify_multiplier;
} VirtioPciCapability;

static int virtio_pci_capability_valid(const VirtioPciCapability *cap) {
    return cap && cap->bar < 6 && cap->length && !(cap->offset & 3) &&
           cap->offset <= ~0U - cap->length;
}

static int virtio_pci_capabilities_complete(const VirtioPciCapability *common,
                                            const VirtioPciCapability *notify,
                                            const VirtioPciCapability *device) {
    return virtio_pci_capability_valid(common) && common->type == VIRTIO_PCI_CAP_COMMON &&
           virtio_pci_capability_valid(notify) && notify->type == VIRTIO_PCI_CAP_NOTIFY &&
           virtio_pci_capability_valid(device) && device->type == VIRTIO_PCI_CAP_DEVICE &&
           notify->notify_multiplier && notify->notify_multiplier <= notify->length;
}

/* VirtIO 1.x queue notifications are byte offsets in the notify capability:
 * notify_base + queue_notify_off * notify_off_multiplier. */
static u64 virtio_pci_notify_address(const VirtioPciCapability *notify,
                                     u64 notify_bar, u16 queue_notify_off) {
    u64 displacement;
    if (!virtio_pci_capability_valid(notify) || notify->length < 2 ||
        notify->type != VIRTIO_PCI_CAP_NOTIFY || !notify->notify_multiplier ||
        queue_notify_off > (~0U / notify->notify_multiplier)) return 0;
    displacement = (u64)queue_notify_off * notify->notify_multiplier;
    if (displacement > notify->length - 2) return 0;
    if (notify_bar > ~0ULL - notify->offset - displacement - 2) return 0;
    return notify_bar + notify->offset + displacement;
}

static u64 virtio_pci_queue_bytes(u16 queue_size) {
    u64 driver = 16ULL * queue_size + 4 + 2ULL * queue_size;
    return (driver + 4095) & ~4095ULL;
}

#endif
