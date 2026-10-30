/* Address resolution must work before the process creates its first socket. */
#include <assert.h>
#include <netdb.h>
#include <stdio.h>

int main(void) {
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int rc = getaddrinfo("127.0.0.1", "9222", &hints, &result);
    if (rc != 0) fprintf(stderr, "first getaddrinfo failed: %d\n", rc);
    assert(rc == 0 && result);
    const struct sockaddr_in *address = (const struct sockaddr_in *)result->ai_addr;
    assert(address->sin_family == AF_INET);
    assert(ntohs(address->sin_port) == 9222);
    assert(ntohl(address->sin_addr.s_addr) == 0x7f000001);
    freeaddrinfo(result);
    puts("test_win32_socket_startup: all tests passed");
    return 0;
}
