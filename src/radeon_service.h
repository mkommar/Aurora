/* Kernel-to-display Radeon capability handoff. Hardware access is refused
 * unless the PCI BAR and command ring pass the bounded checks below. */
#define RADEON_RING_ADDRESS 0x01800000ULL
#define RADEON_RING_ENTRIES 64
#define RADEON_FEATURE_MMIO 1ULL
#define RADEON_FEATURE_VRAM 2ULL
#define RADEON_FEATURE_IRQ 4ULL
#define RADEON_FEATURE_RING 8ULL
#define RADEON_GEN_LEGACY 1
#define RADEON_GEN_R300 2
#define RADEON_GEN_R600 3
#define RADEON_GEN_GCN 4
#define RADEON_GEN_RDNA3 5
#define RADEON_RX7800XT_DEVICE 0x747e
/* Navi 32 register offsets used only for capability/status reporting. */
#define NAVI32_MMIO_STATUS 0x8010
#define NAVI32_MMIO_SOFT_RESET 0x12000
#define NAVI32_MMIO_INTERRUPT_STATUS 0x4018
#define NAVI32_MMIO_INTERRUPT_ENABLE 0x401c
typedef struct __attribute__((packed)) {
    volatile u32 opcode,flags,sequence,status;
    volatile u64 address,value,length;
} RadeonCommand;
typedef struct __attribute__((aligned(64))) {
    volatile u32 producer,consumer,doorbell,errors;
    RadeonCommand commands[RADEON_RING_ENTRIES];
} RadeonCommandRing;
enum { RADEON_CMD_NOP=0, RADEON_CMD_PRESENT=1, RADEON_CMD_FLUSH=2, RADEON_CMD_RESET=3 };
