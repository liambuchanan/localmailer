#include <ctype.h>
#include <getopt.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * UTIL
 */
int TRUE = 1, FALSE = 0;
int start_listening(char *servname) {
    int error, sockfd;
    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    error = getaddrinfo(NULL, servname, &hints, &servinfo);
    sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    error = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &TRUE, sizeof(int));
    error = bind(sockfd, servinfo->ai_addr, servinfo->ai_addrlen);
    freeaddrinfo(servinfo);
    error = listen(sockfd, 10);
    return sockfd;
}

/*
 * MAILBOX
 */
struct _mailbox {
    int size;
    int deleted;
    int capacity;
    int content_length; // this is to support a weird pop3 feature
    struct message *messages;
    pthread_mutex_t mutex;
};
struct _mailbox _MAILBOX = { 0, 0, 1024, 0, NULL, PTHREAD_MUTEX_INITIALIZER };
struct message {
    int id;
    int content_length;
    char *content;
    int deleted;
};
struct mailbox_summary {
    int size;
    int content_length;
};
struct mailbox_summary create_mailbox_summary() {
    pthread_mutex_lock(&_MAILBOX.mutex);
    struct mailbox_summary ms = { _MAILBOX.size - _MAILBOX.deleted, _MAILBOX.content_length };
    pthread_mutex_unlock(&_MAILBOX.mutex);
    return ms;
}
void store_message(char *message) {
    int message_length = strlen(message);
    // TODO double the array when size == capacity
    pthread_mutex_lock(&_MAILBOX.mutex);
    if (_MAILBOX.messages == NULL) {
        _MAILBOX.messages = (struct message *)malloc(sizeof(struct message)*_MAILBOX.capacity);
    }
    int message_id = _MAILBOX.size + 1;
    _MAILBOX.messages[message_id-1].id = message_id;
    _MAILBOX.messages[message_id-1].content_length = message_length;
    _MAILBOX.messages[message_id-1].content = message;
    _MAILBOX.messages[message_id-1].deleted = FALSE;
    _MAILBOX.size++;
    _MAILBOX.content_length += message_length;
    pthread_mutex_unlock(&_MAILBOX.mutex);
}
struct message *fetch_message(message_id) {
    struct message *m;
    pthread_mutex_lock(&_MAILBOX.mutex);
    m = (1 <= message_id && message_id <= _MAILBOX.size) ? &_MAILBOX.messages[message_id-1] : NULL;
    pthread_mutex_unlock(&_MAILBOX.mutex);
    return m->deleted ? NULL : m;
}
void delete_message(message_id) {
    struct message *m;
    pthread_mutex_lock(&_MAILBOX.mutex);
    m = (1 <= message_id && message_id <= _MAILBOX.size) ? &_MAILBOX.messages[message_id-1] : NULL;
    if (m == NULL) {
        return;
    }
    _MAILBOX.deleted++;
    _MAILBOX.content_length -= m->content_length;
    m->content_length = 0;
    if (m->content != NULL) {
        free(m->content);
        m->content = NULL;
    }
    m->deleted = TRUE;
    pthread_mutex_unlock(&_MAILBOX.mutex);
    return;
}

/*
 * POP3
 */
void *handle_pop_session(void *fd_p) {
    int fd = *((int *)fd_p);
    send(fd, "+OK\r\n", 5, 0);
    int buffer_len = 1024;
    char buffer[buffer_len];
    int end_session = FALSE;
    printf("pop start\n");
    struct mailbox_summary ms = create_mailbox_summary(); // freeze state seen by client at start of session
    while (!end_session) {
        int len = recv(fd, buffer, 1023, 0);
        buffer[len] = '\0';
        printf("pop recv (%s)\n", buffer);
        for (int i=0; i<4 && i<len; i++) { buffer[i] = toupper(buffer[i]); }
        int resp_buffer_size = 1024, resp_size = 0;
        char resp[resp_buffer_size];
        if      (len < 4)                         { resp_size += snprintf(resp, resp_buffer_size-resp_size, "-ERR\r\n"); end_session=TRUE; }
        else if (strncmp(buffer, "CAPA", 4) == 0) { resp_size += snprintf(resp, resp_buffer_size-resp_size, "+OK\r\nUSER\r\n.\r\n"); }
        else if (strncmp(buffer, "DELE", 4) == 0) { 
            int message_id = (len > 5) ? (int)strtol(&buffer[5], NULL, 10) : 0;
            delete_message(message_id);
            resp_size = snprintf(resp, 16, "+OK\r\n");
        }
        else if (strncmp(buffer, "NOOP", 4) == 0) { resp_size = snprintf(resp, 16, "+OK\r\n"); }
        else if (strncmp(buffer, "PASS", 4) == 0) { resp_size += snprintf(resp, resp_buffer_size-resp_size, "+OK\r\n"); }
        else if (strncmp(buffer, "QUIT", 4) == 0) { resp_size += snprintf(resp, resp_buffer_size-resp_size, "+OK\r\n"); end_session=TRUE; } // FIXME: this should be commit
        else if (strncmp(buffer, "RSET", 4) == 0) { resp_size = snprintf(resp, 16, "+OK\r\n"); } // FIXME: this should be ROLLBACK
        else if (strncmp(buffer, "STAT", 4) == 0) { resp_size += snprintf(resp, resp_buffer_size-resp_size, "+OK %d %d\r\n", ms.size, ms.content_length); }
        else if (strncmp(buffer, "USER", 4) == 0) { resp_size += snprintf(resp, resp_buffer_size-resp_size, "+OK\r\n"); }
        else if (strncmp(buffer, "LIST", 4) == 0) {
            int message_id = (len > 5) ? (int)strtol(&buffer[5], NULL, 10) : 0;
            if (message_id > 0) {
                struct message *m = fetch_message(message_id);
                if (m != NULL) {
                    resp_size += snprintf(&resp[resp_size], resp_buffer_size-resp_size, "+OK %d %d\r\n", m->id, m->content_length);
                } else {
                    resp_size += snprintf(&resp[resp_size], resp_buffer_size-resp_size, "-ERR\r\n");
                }
            } else {
                resp_size += snprintf(&resp[resp_size], resp_buffer_size-resp_size, "+OK\r\n");
                for (int message_id=1; message_id<=ms.size; message_id++) {
                    struct message *m = fetch_message(message_id);
                    if (m != NULL) {
                        resp_size += snprintf(&resp[resp_size], resp_buffer_size-resp_size, "%d %d\r\n", m->id, m->content_length);
                    }
                }
                resp_size += snprintf(&resp[resp_size], resp_buffer_size-resp_size, ".\r\n");
            }
        }
        else if (strncmp(buffer, "RETR", 4) == 0) {
            // FIXME: byte stuff any . chars that start lines in the email
            // FIXME: this won't handle messages that have content_length > buffer size
            int message_id = (len > 5) ? (int) strtol(&buffer[5], NULL, 10) : 0;
            struct message *m = fetch_message(message_id);
            if (m != NULL) {
                resp_size += snprintf(&resp[resp_size], resp_buffer_size-resp_size, "+OK\r\n%s\r\n.\r\n", m->content);
            } else {
                resp_size += snprintf(&resp[resp_size], resp_buffer_size-resp_size, "-ERR\r\n");
            }
        }
        else                                      { resp_size = snprintf(resp, 16, "-ERR\r\n"); end_session=TRUE; }
        printf("pop send (%s)\n", resp);
        send(fd, resp, resp_size, 0);
    }
    printf("pop done\n");
    close(fd);
    return NULL;
}
void *run_pop(void *servname) {
    int sockfd = start_listening((char *)servname);
    while (TRUE) {
        struct sockaddr_storage their_addr;
        socklen_t their_addr_size = sizeof their_addr;
        printf("prepopcon");
        int fd = accept(sockfd, (struct sockaddr *)&their_addr, &their_addr_size);
        printf("pop conn");
        pthread_t thread;
        pthread_create(&thread, NULL, handle_pop_session, &fd);
    }
    return NULL;
}

/*
 * SMTP
 */
void *handle_smtp_session(void *fd_p) {
    int fd = *((int *)fd_p);
    send(fd, "220\r\n", 5, 0);
    // handle the client session
    int buffer_len = 1024;
    char buffer[buffer_len], *message = NULL;
    int end_session = FALSE, message_part = -1;
    printf("smtp start\n");
    while (!end_session) {
        int len = recv(fd, buffer, 1023, 0);
        buffer[len] = '\0';
        printf("smtp recv (%s)\n", buffer);
        if (message_part != -1) {
            char *new_message = (char *)malloc((message_part+1)*buffer_len*sizeof(char));
            if (message != NULL) {
                strncpy(new_message, message, message_part*buffer_len);
                free((void *)message);
            }
            message = new_message;
            // NB: this will break if the closing sequence is split across 2 buffers!
            char *line = buffer;
            while (line != NULL) {
                char *next = strstr(line, "\r\n");
                if (next != NULL) { *next = '\0'; }
                int dot_start = line[0] == '.';
                if (dot_start) { line++; }
                strcat(message, line);
                strcat(message, "\r\n");
                if (dot_start && line == next) {
                    store_message(message);
                    line = NULL;
                    message_part = -1;
                    send(fd, "250\r\n", 5, 0);
                } else if (next == NULL) {
                    line = NULL;
                    message_part++;
                } else {
                    line = next+2;
                    message_part++;
                }
            }
        } else {
            for (int i=0; i<4 && i<len; i++) { buffer[i] = toupper(buffer[i]); }
            char resp[16];
            int resp_size;
            if      (len < 4)                         { resp_size = snprintf(resp, 16, "500\r\n"); end_session=TRUE; }
            else if (strncmp(buffer, "DATA", 4) == 0) { resp_size = snprintf(resp, 16, "354\r\n"); message_part++; }
            else if (strncmp(buffer, "EHLO", 4) == 0) { resp_size = snprintf(resp, 16, "250\r\n"); }
            else if (strncmp(buffer, "HELO", 4) == 0) { resp_size = snprintf(resp, 16, "250\r\n"); }
            else if (strncmp(buffer, "MAIL", 4) == 0) { resp_size = snprintf(resp, 16, "250\r\n"); }
            else if (strncmp(buffer, "NOOP", 4) == 0) { resp_size = snprintf(resp, 16, "250\r\n"); }
            else if (strncmp(buffer, "QUIT", 4) == 0) { resp_size = snprintf(resp, 16, "221\r\n"); end_session=TRUE; }
            else if (strncmp(buffer, "RCPT", 4) == 0) { resp_size = snprintf(resp, 16, "250\r\n"); }
            else if (strncmp(buffer, "RSET", 4) == 0) { resp_size = snprintf(resp, 16, "250\r\n"); }
            else if (strncmp(buffer, "VRFY", 4) == 0) { resp_size = snprintf(resp, 16, "250\r\n"); }
            else                                      { resp_size = snprintf(resp, 16, "500\r\n"); end_session=TRUE; }
            printf("smtp send (%s)\n", resp);
            send(fd, resp, resp_size, 0);
        }
    }
    printf("smtp done\n");
    close(fd);
    return NULL;
}
void *run_smtp(void *servname) {
    int sockfd = start_listening((char *)servname);
    while (TRUE) {
        struct sockaddr_storage their_addr;
        socklen_t their_addr_size = sizeof their_addr;
        int fd = accept(sockfd, (struct sockaddr *)&their_addr, &their_addr_size);
        pthread_t thread;
        pthread_create(&thread, NULL, handle_smtp_session, &fd);
    }
    return NULL;
}

/*
 * MAIN
 */
int main(int argc, char * const argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char *usage = "usage: %s [--pop-port <port>] [--smtp-port <port>]\n";
    static struct option longopts[] = {
        {"pop-port",  required_argument, NULL, 'p'},
        {"smtp-port", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'}
    };
    int opt;
    char *pop_port  = "110";
    char *smtp_port = "587";
    while ((opt=getopt_long(argc, argv, "p:s:h", longopts, NULL))!=-1) {
        switch (opt) {
            case 'p':
                pop_port  = optarg;
                break;
            case 's':
                smtp_port = optarg;
                break;
            case 'h':
            default:
                printf(usage, argv[0]);
                exit(0);
        }
    }
    pthread_t pop_thread, smtp_thread;
    pthread_create(&pop_thread, NULL, run_pop, pop_port);
    pthread_create(&smtp_thread, NULL, run_smtp, smtp_port);
    pthread_join(pop_thread, NULL);
    pthread_join(smtp_thread, NULL);
}
