/* Included by test_win32_posix.c; exercises only the public C socket surface. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

void test_win32_sockets(void) {
    int pair[2];
    char byte = 0;
    assert(SOCK_NONBLOCK != 0 && SOCK_CLOEXEC != 0 && MSG_DONTWAIT != 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    assert(pair[0] >= 0x40000000 && pair[1] >= 0x40000000);
    assert(fcntl(pair[0], F_GETFD) == FD_CLOEXEC);
    assert((fcntl(pair[0], F_GETFL) & O_NONBLOCK) == 0);
    errno = 0;
    assert(recv(pair[0], &byte, 1, MSG_DONTWAIT) == -1 && errno == ENOTSUP);
    assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(pair[0], F_GETFL) & O_NONBLOCK);
    errno = 0;
    assert(recv(pair[0], &byte, 1, MSG_DONTWAIT) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(fcntl(pair[0], F_SETFL, 0) == 0);
    assert(fcntl(pair[0], F_SETFD, 0) == 0);
    assert(fcntl(pair[0], F_GETFD) == 0);
    errno = 0;
    assert(gufo_socket_spawn_check() == ENOTSUP && errno == ENOTSUP);
    assert(fcntl(pair[0], F_SETFD, FD_CLOEXEC) == 0);
    assert(gufo_socket_spawn_check() == 0);

    struct timeval timeout = {0, 1501};
    assert(setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    socklen_t length = sizeof(timeout);
    assert(getsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, &length) == 0);
    assert(length == sizeof(timeout) && timeout.tv_sec == 0 && timeout.tv_usec == 2000);
    timeout.tv_sec = -1;
    errno = 0;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1);
    assert(errno == EINVAL);
    int error = -1;
    length = sizeof(error);
    assert(getsockopt(pair[0], SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0);
    assert(write(pair[1], "x", 1) == 1);
    assert(read(pair[0], &byte, 1) == 1 && byte == 'x');
    assert(shutdown(pair[1], SHUT_WR) == 0);
    assert(recv(pair[0], &byte, 1, 0) == 0);
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);
    errno = 0;
    assert(close(pair[0]) == -1 && errno == EBADF);
    errno = 0;
    assert(recv(pair[0], &byte, 1, 0) == -1 && errno == EBADF);
    errno = 0;
    assert(socketpair(AF_INET, SOCK_STREAM, 0, pair) == -1 && errno == EAFNOSUPPORT);

    /* More than FD_SETSIZE real sockets; output-only bits must be ignored in
     * events without changing the caller's requested mask at any index. */
    enum { SOCKET_COUNT = 70 };
    int pairs[SOCKET_COUNT][2];
    struct pollfd descriptors[SOCKET_COUNT + 1];
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    for (int i = 0; i < SOCKET_COUNT; ++i) {
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pairs[i]) == 0);
        descriptors[i].fd = pairs[i][0];
        descriptors[i].events = POLLIN | POLLERR | POLLHUP | POLLNVAL;
        descriptors[i].revents = -1;
    }
    const int closed = pairs[0][0];
    assert(close(closed) == 0);
    pairs[0][0] = -1;
    descriptors[SOCKET_COUNT].fd = -7;
    descriptors[SOCKET_COUNT].events = POLLIN;
    descriptors[SOCKET_COUNT].revents = -1;
    assert(send(pairs[SOCKET_COUNT - 1][1], "p", 1, MSG_NOSIGNAL | MSG_DONTWAIT) == 1);
    assert(poll(descriptors, SOCKET_COUNT + 1, 1000) == 2);
    assert(descriptors[0].revents == POLLNVAL);
    assert(descriptors[SOCKET_COUNT - 1].revents & POLLIN);
    assert(descriptors[SOCKET_COUNT].revents == 0);
    for (int i = 0; i < SOCKET_COUNT; ++i) {
        assert(descriptors[i].events == (POLLIN | POLLERR | POLLHUP | POLLNVAL));
        if (pairs[i][0] >= 0) assert(close(pairs[i][0]) == 0);
        assert(close(pairs[i][1]) == 0);
    }
    /* Socket closure must not close unrelated CRT descriptors. */
    assert(write(pipe_fds[1], "f", 1) == 1);
    assert(read(pipe_fds[0], &byte, 1) == 1 && byte == 'f');
    assert(close(pipe_fds[0]) == 0 && close(pipe_fds[1]) == 0);

    /* Server's ordinary bind/listen/connect/accept path, including addresses. */
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(listener >= 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    assert(address.sin_port != 0 && listen(listener, 1) == 0);
    int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(client >= 0 && connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    int accepted = accept(listener, NULL, NULL);
    assert(accepted >= 0);
    assert((fcntl(accepted, F_GETFL) & O_NONBLOCK) == 0);
    assert(fcntl(accepted, F_GETFD) == 0);
    length = sizeof(address);
    assert(getpeername(accepted, (struct sockaddr *)&address, &length) == 0);
    assert(address.sin_family == AF_INET);
    assert(send(client, "s", 1, MSG_NOSIGNAL) == 1);
    assert(recv(accepted, &byte, 1, 0) == 1 && byte == 's');
    assert(close(accepted) == 0 && close(client) == 0 && close(listener) == 0);
}
