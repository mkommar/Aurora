/* Ring-3 storage service. The kernel owns the device and performs the bounded
 * request, but this process owns service admission and is the only caller of
 * the storage syscall. */
#include "lib.h"
#include "../storage_service.h"

#define STORAGE_SERVICE_BUFFER 0x0d080000ULL

void user_main(void) {
    serial("STORAGE: ring3 service ready\r\n");
    /* A flush proves the service reached the kernel storage endpoint without
     * exposing device ports or descriptor addresses to user mode. */
    i64 status = syscall(SYS_STORAGE, STORAGE_IPC_FLUSH, 0, 0);
    serial(status == STORAGE_IPC_OK ? "STORAGE: dispatch ready\r\n" :
                                      "STORAGE: dispatch unavailable\r\n");
#ifdef AURORA_SELF_TEST
    if (*(volatile u8 *)STORAGE_SERVICE_BUFFER == 0) {
        *(volatile u8 *)STORAGE_SERVICE_BUFFER = 1;
        *(volatile u64 *)0x10000 = 0;
    }
#endif
    for (;;) {
        *(volatile u8 *)STORAGE_SERVICE_BUFFER = 1;
        yield();
    }
}
