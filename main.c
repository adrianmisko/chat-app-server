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


//TODO - read_len -> bytes read
//TODO - resilet write (func)
//TODO - parse headers

void read_from_socket(int clientfd, struct conn_state* conn_state) {
    char buf[1024];
    memset(buf, 0, sizeof(buf));
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
            break;
        } else {
            if (finished) continue; //ignore whats after header this time in case we got header and
            if (conn_state->read_len != 0) {                  //then data arrived instead of getting eagain and break
                //reading is resumed
                //concatenate buffers
                puts("mempcy");
                memcpy(conn_state->buf+conn_state->read_len, buf,
                       sizeof(buf)+conn_state->read_len);
                //has it found header this time?
                if (strstr(buf, "\r\n\r\n") != NULL) {
                    //if yes dealloc memory, finished = 1 (has to do more loops to get to the EAGAIN)
                    puts("found rn");
                    printf("%s\n", conn_state->buf);
                    memset(conn_state->buf, 0, 4096);
                    free (conn_state->buf);
                    conn_state->read_len = 0;
                    finished = 1;
                    //parse header
                } else {
                    //if no check if read_len > 4kb (header too large)
                    //if no then go on
                    conn_state->read_len += (int)bytes_read;
                    if (conn_state->read_len >= 4096) {
                        //header too big
                    }
                }
            } else {
                //standard read
                //look for rnrn - found? ok stop - were only interesed in headers
                if (strstr(buf, "\r\n\r\n") != NULL) {
                    //parse header
                    puts("found rn right away");
                    printf("%s\n", buf);
                } else {
                    //else save state - malloc, add to conn_states
                    puts("mallocing");
                    conn_state->buf = (char*)malloc(4096 * sizeof(char));
                    memcpy(conn_state->buf, buf, (size_t)bytes_read);
                    conn_state->read_len = (int)bytes_read;
                }
            }
        }
    }
}



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
                printf("socket nr %d\n", events[i].data.fd);
                read_from_socket(events[i].data.fd, &conn_states[events[i-5].data.fd]); //4 decriptors are always taken + 1 for index 0
            }

        }
    }

    return 0;

}
