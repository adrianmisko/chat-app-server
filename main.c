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

const char* header_too_big = "HTTP/1.1 431 Request Header Fields Too Large\r\n\r\n";
const char* content_not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
char* html_response;


struct write_state {
    struct write_state* next;
    char* buf;
};


struct write_queue {
    struct write_state* head;
    struct write_state* tail;
    size_t size;
};


struct conn_state {         //used to store state of connection if we got partial read or write
    char protocol;          //0 - HTTP, 1 - WebSocket (...#define)
    size_t bytes_read;
    char* buf;
    struct write_queue write_queue;    //write needs a queue in case we had partial write and then read which started another write
};


struct write_state* front(struct write_queue* write_queue) {
    return write_queue->head;
}

void append(struct write_queue* write_queue, struct write_state* write_state) {
    if (write_queue->size == 0) {
        write_queue->head = write_state;
        write_queue->tail = write_state;
    } else {
        write_queue->tail->next = write_state;
        write_queue->tail = write_state;
    }
    write_queue->size++;
}

void remove_front(struct write_queue* write_queue) {
    if (write_queue->size == 0)
        return;
    if (write_queue->size == 1) {
        free (write_queue->head);
        memset(write_queue, 0, sizeof(struct write_queue));
        return;
    } else {
        struct write_state* temp = write_queue->head->next;
        free(write_queue->head);
        write_queue->head = temp;
        write_queue->size--;
    }
}

void release_and_reset(struct conn_state* conn_state) {
    if (conn_state->write_queue.size == 0) {
        memset(conn_state, 0, sizeof(struct conn_state));
        return;
    }
    struct write_state* write_state = conn_state->write_queue.head;
    while (write_state != NULL) {
        free(write_state->buf);
        write_state = write_state->next;
    }
    memset(conn_state, 0, sizeof(struct conn_state));
}

void resume_write(int clientfd, struct conn_state* conn_state, int efd) {
    char* msg = conn_state->write_queue.head->buf;
    size_t msglen = strlen(msg);
    size_t remaining_bytes = msglen;
    while (1) {
        size_t to_write = remaining_bytes < 4096 ? remaining_bytes : 4096;
        ssize_t bytes_wrote = write(clientfd, msg, to_write);
        if (bytes_wrote == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                //we didn't fit it all - need to check again later
                //epoll is still polling for write, no need to rearm the descriptor
                //move remaining memory to the beggining of the buffer
                memcpy(msg, msg+msglen-remaining_bytes, remaining_bytes);
                memset(msg+remaining_bytes, 0, msglen-remaining_bytes);
            } else if (errno == EPIPE) {
                puts("client has terminated connection");
                release_and_reset(conn_state);
                close(clientfd);
                break;
            } else {
                remaining_bytes -= bytes_wrote;
                if (remaining_bytes == 0) {
                    //were done -> remove head from queue and start writing next message. Stop polling for write event
                    remove_front(&conn_state->write_queue);                            //and return if there are no enqueued operations
                    if (conn_state->write_queue.size == 0) {
                        struct epoll_event event;
                        memset(&event, 0, sizeof(event));
                        event.data.fd = clientfd;
                        event.events = EPOLLIN | EPOLLET;
                        epoll_ctl(efd, EPOLL_CTL_MOD, clientfd, &event);
                        break;
                    } else {
                        //update variables so that next write starts to write next message
                        msg = conn_state->write_queue.head->buf;
                        msglen = strlen(msg);
                        remaining_bytes = msglen;
                    }
                }
                //otherwise continue writing untill we get EAGAIN or finish the write
            }
        }
    }
}

void write_to_socket(int clientfd, char* msg, struct conn_state* conn_state, int efd) {
    size_t msglen = strlen(msg);
    size_t remaining_bytes = msglen;
    size_t bytes_sent = 0;
    while (1) {
        size_t to_write = remaining_bytes < 4096 ? remaining_bytes : 4096;
        printf("to be written %d\n", (int)to_write);
        ssize_t bytes_wrote = write(clientfd, msg+bytes_sent, to_write);
        if (bytes_wrote == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                //we didn't fit it all - need to check again later
                struct epoll_event event;
                memset(&event, 0, sizeof(event));
                event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                event.data.fd = clientfd;
                epoll_ctl(efd, EPOLL_CTL_MOD, clientfd, &event);
                //also save the state
                struct write_state* write_state = (struct write_state*)calloc(1, sizeof(struct write_state));
                conn_state->write_queue.tail->buf = (char*)malloc(remaining_bytes * sizeof(char));
                memcpy(conn_state->write_queue.tail->buf, msg+bytes_sent, remaining_bytes);
                append(&conn_state->write_queue, write_state);
            } else if (errno == EPIPE) {
                puts("client has terminated connection");
                release_and_reset(conn_state);
                close(clientfd);
                break;
            } else {
                printf("errno %d\n", errno);
                perror("error on writing to client");
            }
        } else {
            bytes_sent += bytes_wrote;
            remaining_bytes -= bytes_wrote;
            printf("remaining bytes %d\n", (int)remaining_bytes);
            if (remaining_bytes == 0) {
                //were done
                break;
            }
            //else continue
        }
    }
}


//as for now, we have only handful of files to send
//so instead of using sendfile() they are already stored in memory
//and request is dispatched in else-if spaghetti
void parse_header(char* msg, int clientfd, struct conn_state* conn_state, int efd) {
    char *first_line = strtok(msg, "\r\n");
    char *rest = strtok(msg, "");
    char *method = strtok(first_line, " ");
    char *resource = strtok(NULL, " ");
    printf("method: %s\nresource: %s\n", method, resource);
    if (strcmp(method, "GET") == 0) {
        if (strcmp(resource, "/") == 0) {
            puts("send html file");
        } else if (strcmp(resource, "/app.js") == 0) {
            puts("send js file\"");
        } else if (strcmp(resource, "/styles.css") == 0) {
            puts("send css file\"");
        } else if (strcmp(resource, "/chat") == 0) {    //protocol upgrade
            puts("protocol upgrade");
            char* host = strtok(rest, "\r\n");
            char* upgrade = strtok(NULL, "\r\n");
            char* connection = strtok(NULL, "\r\n");
            char* sec_websocket_key = strtok(NULL, "\r\n");
            char* sec_websocket_version = strtok(NULL, "\r\n");
            char* rest_of_handshake = strtok(NULL, "");
        } else {
            puts("send 404");
        }
    } else {
        puts("method not supported");
    }
}

void resume_read(int clientfd, struct conn_state* conn_state, int efd) {
    while (1) {
        size_t remaining_space = 4096 - conn_state->bytes_read;
        ssize_t bytes_read = read(clientfd, conn_state->buf+conn_state->bytes_read, remaining_space);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {  //no more data
                break;
            } else if (errno == EPIPE) {
                puts("client has terminated connection");
                release_and_reset(conn_state);
                close(clientfd);
                break;
            } else {
                perror("error on reading from client");
                exit(1);
            }
        } else if (bytes_read == 0) {
            puts("client has disconnected");
            release_and_reset(conn_state);
            close(clientfd);
            break;
        } else if (bytes_read == remaining_space) {
            //buffer is full
            break;
        } else {
            conn_state->bytes_read += bytes_read;
            //has it found header this time?
            size_t offset = conn_state->bytes_read - bytes_read;
            if (strstr(conn_state->buf + offset, "\r\n\r\n") != NULL) {      //buff + offset -> no need to look for delimiter in whole buffer
                puts("found header this time");
                //parse header and decide what to do next (answer or wait for whole message)
                write_to_socket(clientfd, html_response, conn_state, efd);
            }
            //else continue reading, most likely we will get EAGAIN here
        }
    }
}

void read_from_socket(int clientfd, struct conn_state* conn_state, int efd) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    while (1) {
        ssize_t bytes_read = read(clientfd, buf, sizeof(buf));
        printf("bytes read: %d\n", (int)bytes_read);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {  //no more data
                break;
            } else if (errno == EPIPE) {
                puts("client has terminated connection");
                release_and_reset(conn_state);
                close(clientfd);
                break;
            } else {
                perror("error on reading from client");
                exit(1);
            }
        } else if (bytes_read == 0) {
            puts("client has disconnected");
            release_and_reset(conn_state);
            close(clientfd);
            break;
        } else {
            //look for header delimiter
            if (strstr(buf, "\r\n\r\n") != NULL) {
                puts("found rn right away");
                //parse header and decide what to do next (answer or wait for whole message)
                write_to_socket(clientfd, html_response, conn_state, efd);
            } else {
                //else save state - malloc, add to conn_states
                puts("malloc-ing");
                conn_state->buf = (char*)malloc(4096 * sizeof(char));
                memcpy(conn_state->buf, buf, (size_t)bytes_read);
                conn_state->bytes_read = (size_t)bytes_read;
            }
        }
    }
}



int main(int argc, char const *argv[]) {

    char* response_header = "HTTP/1.1 200 OK\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %d\r\n\r\n";

    // as for now, we have only handful of files to send
    //so instead of using sendfile() they are already stored in memory

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
                int clientfd = events[i].data.fd;
                struct conn_state* conn_state = &conn_states[events[i].data.fd];
                if (events[i].events & EPOLLOUT) {
                    resume_write(clientfd, conn_state, efd);
                } else {
                    if (conn_state->bytes_read == 0)
                        read_from_socket(clientfd, conn_state, efd);
                    else
                        resume_read(clientfd, conn_state, efd);
                }
            }

        }
    }

    return 0;

}
