#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netdb.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <errno.h>

#define PORT 8080
#define MAX_EVENTS 1000
#define MAX_CLIENTS 1000

//TODO clear connstate on client disconnection and finished writing

const char* header_too_big = "HTTP/1.1 431 Request Header Fields Too Large\r\n\r\n";
const char* content_not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
char* html_response;

struct conn_state {
    int protocol;
    int bytes_read;
    int bytes_wrote;
    char* buf;
    char* msg; //tailq
};

void write_to_socket(int clientfd, char* msg, struct conn_state* conn_state, int efd, char continued) {
    size_t msglen = strlen(msg);
    while (1) {
        ssize_t remaining_bytes = msglen - conn_state->bytes_wrote;
        ssize_t to_be_written = remaining_bytes < 4096 ? remaining_bytes : 4096;
        printf("To be written: %d\n", to_be_written);
        ssize_t bytes_wrote = write(clientfd, msg+conn_state->bytes_wrote, (size_t)to_be_written);
        if (bytes_wrote == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                //we didnt fit it all - need to check again later
                struct epoll_event event;
                memset(&event, 0, sizeof(event));
                event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                event.data.fd = clientfd;
                epoll_ctl(efd, EPOLL_CTL_MOD, clientfd, &event);
                //save state
                conn_state->msg = msg;
            } else if (errno == EPIPE) {
                puts("client has terminated connection");
                close(clientfd);
                break;
            } else {
                printf("errno %d\n", errno);
                perror("error on writing to client");
                exit(1);
            }
        } else if (bytes_wrote == 0) {
            puts("client has disconnected");
            close(clientfd);
            break;
        } else {
            conn_state->bytes_wrote += bytes_wrote;
            if (conn_state->bytes_wrote == msglen) {
                //were done
                conn_state->bytes_wrote = 0;
                if (continued) {
                    struct epoll_event event;
                    memset(&event, 0, sizeof(event));
                    event.events = EPOLLIN | EPOLLET;
                    event.data.fd = clientfd;
                    epoll_ctl(efd, EPOLL_CTL_MOD, clientfd, &event);
                }
                break;
            }
        }
    }
}

void parse_header_and_send_response(char* msg, int clientfd, struct conn_state* conn_state, int efd) {
    char* first_line = strtok(msg, "\r\n");
    char* rest =   strtok(msg, "");
    char* method = strtok(first_line, " ");
    char* resource = strtok(NULL, " ");
    printf("method: %s\nresource: %s\n", method, resource);
    if (strcmp(method, "GET") == 0) {
        if (strcmp(resource, "/") == 0) {
            puts("slij index");
        } else if (strcmp(resource, "/app.js") == 0) {
            puts("slij jsa");
        } else if (strcmp(resource, "/styles.css") == 0) {
            puts("slij cssa");
        } else if (strcmp(resource, "/chat") == 0) {
            char* host = strtok(rest, "\r\n");
            char* upgrade = strtok(NULL, "\r\n");
            char* connection = strtok(NULL, "\r\n");
            char* sec_websocket_key = strtok(NULL, "\r\n");
            char* sec_websocket_version = strtok(NULL, "\r\n");
            char* rest_of_handshake = strtok(NULL, "");
        } else {
            puts("slij 404");
        }
    } else {
        puts("aaa");
    }
}

void read_from_socket(int clientfd, struct conn_state* conn_state, int efd) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    char finished = 0;
    while (1) {
        ssize_t bytes_read = read(clientfd, buf, sizeof(buf));
        printf("bytes read: %d\n", (int)bytes_read);
        if (bytes_read == -1) {

            if (errno == EAGAIN || errno == EWOULDBLOCK) {  //no more data
                break;
            } else if (errno == EPIPE) {
                puts("client has terminated connection");
                close(clientfd);
                return;
            } else {
                perror("error on reading from client");
                exit(1);
            }

        } else if (bytes_read == 0) {
            puts("client has disconnected");
            close(clientfd);
            return;
        } else {
            if (finished) continue; //ignore whats after header this time in case we got header and
            if (conn_state->bytes_read != 0) {                  //then data arrived instead of getting eagain and break
                //reading is resumed
                //concatenate buffers
                puts("mempcy");
                memcpy(conn_state->buf+conn_state->bytes_read, buf,
                       sizeof(buf)+conn_state->bytes_read);
                //printf("%s\n", conn_state->buf);
                //has it found header this time?
                if (strstr(buf, "\r\n\r\n") != NULL) {
                    //if yes dealloc memory, finished = 1 (has to do more loops to get to the EAGAIN)
                    puts("found rn");
                    printf("%s\n", conn_state->buf);
                    //parse_header_and_send_response(conn_state->buf, clientfd, conn_state, efd);
                    memset(conn_state->buf, 0, 4096);
                    free (conn_state->buf);
                    conn_state->bytes_read = 0;
                    finished = 1;

                } else {
                    //if no check if read_len > 4kb (header too large)
                    //if no then go on
                    conn_state->bytes_read += (int)bytes_read;
                    if (conn_state->bytes_read >= 4096) {
                        write_to_socket(clientfd, header_too_big, conn_state, efd, 0);
                        conn_state->bytes_read = 0;
                        free (conn_state->buf);
                    }
                }
            } else {
                //standard read
                //look for rnrn - found? ok stop - were only interesed in headers
                if (strstr(buf, "\r\n\r\n") != NULL) {
                    //parse header
                    write(clientfd, "a\n", 3);
                    puts("found rn right away");
                    //parse_header_and_send_response(buf, clientfd, conn_state, efd);
                    write_to_socket(clientfd, html_response, conn_state, efd, 0);
                } else {
                    //else save state - malloc, add to conn_states
                    puts("mallocing");
                    conn_state->buf = (char*)malloc(4096 * sizeof(char));
                    memcpy(conn_state->buf, buf, (size_t)bytes_read);
                    conn_state->bytes_read = (int)bytes_read;
                }

            }
        }
    }
}



int main(int argc, char const *argv[]) {

    char* response_header = "HTTP/1.1 200 OK\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %d\r\n\r\n";

    struct stat st;
    stat("../index.html", &st);
    __off_t fsize = st.st_size;
    int file = open("../index.html", O_RDONLY);
    if (file == -1) {
        perror("index html");
        exit(1);
    }


    html_response = calloc(strlen(response_header) + fsize + 100, sizeof(char));

    sprintf(html_response, response_header, "text/html", fsize);

    ssize_t err = read(file, html_response+strlen(html_response), (size_t)fsize);
    if (err == -1) {
        perror("error on reading file to buf");
        exit(1);
    }

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
            printf("readyFD: %d ", events[i].data.fd);
            if (events[i].events & EPOLLOUT)
                puts("wants to read");
            else
                puts("wants to write");
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
                if (events[i].events & EPOLLOUT) {
                    write_to_socket(events[i].data.fd, conn_states[events[i].data.fd - 4].msg, &conn_states[events[i].data.fd - 4], efd, 1);
                } else {
                    //printf("socket nr %d\n", events[i].data.fd);
                    read_from_socket(events[i].data.fd, &conn_states[events[i].data.fd - 4], efd);
                }
            }

        }
    }

    return 0;

}
