/* The clean marker is published only after all earlier writes are durable.
 * Any failed write/flush makes the session fail closed until the next mount. */
static int ext2_clean,ext2_write_error;
volatile u64 ext2_unclean_mounts,ext2_dirty_marks,ext2_sync_failures;
static int ext2_state(u16 state){struct ext4_sblock *sb=0;int error=ext4_get_sblock("/",&sb);if(error)return error;sb->state=state;return ext4_sb_write(&ext2_device,sb);}
static int ext2_failed(int error){ext2_write_error=error?error:5;ext2_sync_failures++;return ext2_write_error;}
static int ext2_dirty(void){
    if(!ext2_ready)return 0;if(ext2_write_error)return ext2_write_error;if(!ext2_clean)return 0;
    int error=ext2_state(2);if(error||!native_disk_flush())return ext2_failed(error);
    ext2_clean=0;ext2_dirty_marks++;return 0;
}
static int ext2_sync(void){
    if(ext2_write_error)return ext2_write_error;
    int error=ext4_cache_flush("/");if(error||!native_disk_flush())return ext2_failed(error);
    error=ext2_state(1);if(error||!native_disk_flush())return ext2_failed(error);
    ext2_clean=1;return 0;
}
