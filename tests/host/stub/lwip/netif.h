// Host-test stub: mm_halow.h embeds these by value, so they need to be complete
// types for it to parse. The allocator never touches mm_halow_t, so the layout is
// irrelevant here -- only that it compiles.
#ifndef MM_HALOW_HOSTTEST_LWIP_NETIF_H
#define MM_HALOW_HOSTTEST_LWIP_NETIF_H
#include <stdint.h>
typedef struct { uint32_t addr;
} ip4_addr_t;
typedef struct { uint32_t addr;
} ip_addr_t;
struct netif { void *state;
               uint8_t num;
};
#endif
