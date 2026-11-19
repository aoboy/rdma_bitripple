#include <string.h>
#include "bitripple.h"
#include "bitripple_rdma_internal.h"

/* UDP compatibility layer — now maps to RDMA operations */
int br_udp_socket_create(void) { return 0; }  /* No-op for RDMA */
int br_udp_bind(int fd, uint16_t port) { (void)fd; (void)port; return 0; }
int br_udp_set_nonblock(int fd) { (void)fd; return 0; }

ssize_t br_udp_sendto(int fd, const void* buf, size_t len,
                      const char* host, uint16_t port) {
    /* This function is no longer used directly. 
       RDMA send is handled through br_rdma_post_send() */
    (void)fd; (void)buf; (void)len; (void)host; (void)port;
    return -1;
}

ssize_t br_udp_recvfrom(int fd, void* buf, size_t len,
                        struct sockaddr_in* from) {
    /* This function is no longer used directly.
       RDMA recv is handled through br_rdma_poll_cq() */
    (void)fd; (void)buf; (void)len; (void)from;
    return -1;
}

