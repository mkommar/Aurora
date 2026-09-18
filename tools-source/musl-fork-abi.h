/* Check the x86-64 layouts used by the pinned archive's existing _Fork object. */
#include <stddef.h>
#include "pthread_impl.h"
_Static_assert(offsetof(struct pthread,tid)==48,"pinned musl tid layout");
_Static_assert(offsetof(struct pthread,prev)==16,"pinned musl prev layout");
_Static_assert(offsetof(struct pthread,next)==24,"pinned musl next layout");
_Static_assert(offsetof(struct pthread,robust_list.off)==144,"pinned musl robust offset layout");
_Static_assert(offsetof(struct pthread,robust_list.pending)==152,"pinned musl robust pending layout");
_Static_assert(offsetof(struct __libc,need_locks)==3,"pinned musl lock layout");
_Static_assert(offsetof(struct __libc,threads_minus_1)==4,"pinned musl thread count layout");
