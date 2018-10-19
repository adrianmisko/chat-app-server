#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netdb.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <errno.h>


#define PORT 1234
#define MAX_EVENTS 1000

int main(int argc, char const *argv[]) {

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    int res = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    if (res == -1) {
        perror("setsockopt reuseaddr error");
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(struct sockaddr_in));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(PORT);

    res = bind(sockfd, (const struct sockaddr*)&sa, sizeof(struct sockaddr_in));
    if (res == -1) {
        perror("bind error");
        exit(1);
    }

    res = fcntl(socketfd, F_SETFL, fcntl(socketfd, F_GETFL, 0) | O_NONBLOCK);
    if (res == -1) {
        perror("error on setting socket as non-blocking");
        exit(1);
    }

    res = listen(sockfd, 0);
    if (res == -1 ) {
        perror("listen error");
        exit(1);
    }

    int efd = epoll_create1(0);
    if (efd == -1) {
        perror("epoll create: ");
        exit(1);
    }

    struct epoll_event event;
    memset(&event, 0, sizeof(struct epoll_event));
    event.data.fd = sockfd;
    event.events = EPOLLIN;
    res = epoll_ctl(efd, EPOLL_CTL_ADD, sockfd, &event);
    if (res == -1) {
        perror("on adding sockfd to epoll");
        exit(1);
    }

    struct epoll_event* events = (struct epoll_event*)calloc(MAX_EVENTS, sizeof(struct epoll_event));


    //TODO - add worker thread, look for new connections in a loop (mark new ones as non-blocking) and give them to worker thread
    //later on - thread pool and queue

    return 0;

}