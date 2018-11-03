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


#define PORT 8080
#define MAX_EVENTS 1000
#define MAX_CLIENTS 1000

struct conn_state {

    int read_len;
    char* buf;

};


int main(int argc, char const *argv[]) {

    struct conn_state conn_states[MAX_CLIENTS];
    memset(conn_states, 0, sizeof(conn_states));

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

    res = fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);
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
    event.events = EPOLLIN | EPOLLET;
    res = epoll_ctl(efd, EPOLL_CTL_ADD, sockfd, &event);
    if (res == -1) {
        perror("on adding sockfd to epoll");
        exit(1);
    }

    struct epoll_event events[MAX_EVENTS];

    printf("Starting to listen on socket %d\n", sockfd);
    while(1) {

        int numready = epoll_wait(efd, events, MAX_EVENTS, -1);
        if (numready == -1) {
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < numready; ++i) {
            printf("readyFD: %d\n", events[i].data.fd);
            if (events[i].data.fd == sockfd) {
                int clientfd = accept(sockfd, 0, 0);
                printf("accepting new client, client fd: %d\n", clientfd);
                if (clientfd == -1) {
                    if (errno == EWOULDBLOCK || errno == EAGAIN) {
                        //that can happen for some reason
                        puts("EWOULDBLOCK || EAGAIN on accept");
                    } else {
                        perror("accept");
                        exit(1);
                    }
                } else {
                    //no error - mark as non blocking and add to epoll set
                    res = fcntl(clientfd, F_SETFL, fcntl(clientfd, F_GETFL, 0) | O_NONBLOCK);
                    if (res == -1) {
                        perror("error on setting socket as non-blocking");
                        exit(1);
                    }
                    memset(&event, 0, sizeof(struct epoll_event));
                    event.events = EPOLLIN | EPOLLET;
                    event.data.fd = clientfd;
                    if (epoll_ctl(efd, EPOLL_CTL_ADD, clientfd, &event) == -1) {
                        perror("epoll_ctl: on adding client socked");
                        exit(1);
                    }
                }
            } else {
                int clientfd = events[i].data.fd;
                printf("socket nr %d\n", clientfd);
                char buf[1024];
                char finished = 0;
                while (1) {
                    ssize_t bytes_read = read(clientfd, buf, sizeof(buf));

                    if (bytes_read == -1) {

                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                        } else {
                            perror("error on reading from client");
                            exit(1);
                        }

                    } else if (bytes_read == 0) {
                        puts("client has disconnected");
                        close(clientfd);
                    } else {
                        if (finished) continue; //ignore whats after header this time in case we got header and then data arrived instead of getting eagain and break
                        if (conn_states[clientfd].read_len != 0) {
                            //reading is resumed
                            //has it found header this time?
                            //if yes dealloc memory, finished = 1 (has to do more loops to get to the EAGAIN)
                            //if no check if read_len > 4kb (header too large)
                            //if no then go on
                        } else {
                           //standard read
                           //look for rnrn - found? ok stop
                           //else malloc, add to conn_states
                        }
                    }


                }



            }

        }
    }

    return 0;

}
