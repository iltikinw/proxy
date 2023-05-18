/**
 * @file tinyproxy.c
 * @brief A tiny concurrent web proxy with caching.
 *
 * A web proxy acts as an intermediary between client web browsers and server
 * web servers providing web content. When a browser uses a proxy, it contacts
 * the proxy instead of the server; the proxy forwards requests and responses
 * between client and server.
 *
 * My implementation uses the main function to continuously accept client
 * connections, and serves those connections via the serve function.
 *
 * High-level overview:
 *     1. Client connection request accepted; served in peer thread.
 *     2. Request line parsed.
 *     3. If request response exists in cache, served directly to client.
 *     4. If not, connect to server, write response, serve back to client.
 *     5. Server response then saved in the cache.
 *
 * I use threads to allow for the proxy to serve clients concurrently.
 * Additionally, I cache server responses in a LRU cache implemented with a
 * doubly-linked list. More cache details can be found in cache.c and cache.h
 *
 * Descriptions of individual functions, data structures, and global variables
 * are provided in their respective leading comments.
 *
 * @author Iltikin Wayet
 */

#include "cache.h"
#include "csapp.h"
#include "http_parser.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

#define HOSTLEN 256
#define SERVLEN 8

// Typedef for convenience
typedef struct sockaddr SA;

// String to use for the User-Agent header.
static const char *header_user_agent = "Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20230411 Firefox/63.0.1";

/**
 * @brief Data structure with client connection information.
 */
typedef struct {
    struct sockaddr_in addr; // Socket address
    socklen_t addrlen;       // Socket address length
    int connfd;              // Client connection file descriptor
    char host[HOSTLEN];      // Client host
    char serv[SERVLEN];      // Client service (port)
} client_info;

/**
 * @brief Data structure with client request information.
 */
typedef struct {
    const char *host;   // A network host, e.g. cs.cmu.edu
    const char *port;   // The port to connect on, by default 80
    const char *path;   // The path to find a resource, e.g. index.html
    const char *method; // HTTP request method, e.g. GET or POST
    const char *uri;    // Entire universal resource identifier
} request_info;

// ---------- FUNCTION PROTOTYPES ---------- //
void *thread(void *vargp);
static void serve(client_info *client);
static void confirm_connection(client_info *client);
static int parse_request(client_info *client, request_info *request,
                         parser_t *parser);
static int write_header(int fd_server, client_info *client,
                        request_info *request, parser_t *parser);
static void clienterror(int fd, const char *errnum, const char *shortmsg,
                        const char *longmsg);

// ---------- FUNCTION ROUTINES ---------- //

/**
 * @brief Continuously accepts and serves client connections.
 *     Each accepted connection served in peer thread.
 *     Robust; not all errors cause function termination.
 *
 * @param[in] argc : number of command line arguments.
 * @param[in] argv : command line input.
 *
 * @return 0 is successful execution, nonzero if error/failure.
 */
int main(int argc, char **argv) {
    // Ignore SIGPIPE signals.
    signal(SIGPIPE, SIG_IGN);

    // Check command line args
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // Open listening file descriptor
    int listenfd = open_listenfd(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "Failed to listen on port: %s\n", argv[1]);
        exit(1);
    }

    cache_init();
    pthread_t tid;
    while (1) {
        // Make space on the stack for client info
        client_info *client = malloc_w(sizeof(client_info));

        // Initialize the length of the address
        client->addrlen = sizeof(client->addr);

        // accept() will block until a client connects to the port
        client->connfd =
            accept(listenfd, (SA *)&client->addr, &client->addrlen);
        // If error, then skip to next loop
        if (client->connfd < 0) {
            perror("accept");
            continue;
        }

        // Connection is established; serve client
        pthread_create(&tid, NULL, thread, (void *)(client));
    }
    cache_free();
    return 0;
}

/**
 * @brief Peer thread function.
 *     Creates copy of client info structure for each thread.
 *     Serves client request, after which closing connection.
 *
 * @param[in] vargp : void* pointer to client_info struct.
 */
void *thread(void *vargp) {
    client_info *prev = (client_info *)(vargp);

    // Store client info for the thread.
    client_info client_data;
    client_info *client = &client_data;

    client->addr = prev->addr;
    client->addrlen = prev->addrlen;
    client->connfd = prev->connfd;

    // Detach thread and begin serving.
    pthread_detach(pthread_self());
    free(vargp);
    serve(client);
    close(client->connfd);
    return NULL;
}

/**
 * @brief Serves client requests; in order:
 *     1. Confirms client connection.
 *     2. Parses client request line, storing info.
 *     3. Searches cache for cached value.
 *     4. If cached, serves to client directly.
 *     5. If not cached, connects to server, writing response to client.
 *     6. Response from server then cached.
 *
 * @param[in] client : information regarding client connection.
 */
static void serve(client_info *client) {
    // Confirms connection accepted from client.
    confirm_connection(client);

    int fd_server;
    parser_t *parser = parser_new();
    // Creating request_info struct to store info.
    request_info request_data;
    request_info *request = &request_data;

    // Parse request line and store relevant info.
    if (parse_request(client, request, parser) < 0) {
        parser_free(parser);
        return;
    }

    // If cache hit, serve text directly to client.
    if (cache_gettext(request->uri, client->connfd)) {
        parser_free(parser);
        return;
    }

    // No cache hit, connect with server, server file descriptor to write to.
    if ((fd_server = open_clientfd(request->host, request->port)) < 0) {
        fprintf(stderr, "Could not connect to %s:%s", request->host,
                request->port);
        close(fd_server);
        parser_free(parser);
        return;
    }

    // Write headers to file descriptor.
    if (write_header(fd_server, client, request, parser) < 0) {
        close(fd_server);
        parser_free(parser);
        return;
    }

    // Have all necessary information, begin serving now.
    // Initialize server rio.
    rio_t rio_server;
    rio_readinitb(&rio_server, fd_server);

    // Send header to server and write response to client.
    // Buffer to scan server response.
    char buf[MAXLINE];
    ssize_t buf_len;

    // Input text to potentially save to cache.
    char cache_input[MAX_OBJECT_SIZE];
    size_t input_len = 0;
    char *writep = cache_input;

    // Scan response lines and write to client/cache input.
    while ((buf_len = rio_readnb(&rio_server, buf, MAXLINE)) > 0) {
        if (rio_writen(client->connfd, buf, buf_len) < 0) {
            clienterror(client->connfd, "400", "Write error",
                        "Error writing response to client");
            return;
        } else {
            // Write response to text to send to cache.
            if ((input_len += buf_len) < MAX_OBJECT_SIZE) {
                memcpy(writep, buf, buf_len);
                writep += buf_len;
            }
        }
    }
    if (input_len < MAX_OBJECT_SIZE)
        cache_insert(request->uri, cache_input, input_len);

    close(fd_server);
    parser_free(parser);
    return;
}

/**
 * @brief Outputs text confirming client connection.
 *
 * @param[in] client : information regarding client connection.
 */
static void confirm_connection(client_info *client) {
    // Get client request info.
    int res = getnameinfo((SA *)&client->addr, client->addrlen, client->host,
                          sizeof(client->host), client->serv,
                          sizeof(client->serv), 0);
    // Output client request info.
    if (res == 0) {
        printf("Accepted connection from %s:%s\n", client->host, client->serv);
    } else {
        fprintf(stderr, "getnameinfo failed: %s\n", gai_strerror(res));
    }
}

/**
 * @brief Parses client request line and headers.
 *     Stores all request information in parser to request data structure.
 *     Encapsulates all parsing operations.
 *
 * @param[in] client  : information regarding client connection.
 * @param[in] request : information regarding request header line.
 * @param[in] parser  : HTTP parser, stores & parses request header lines.
 *
 * @return 0 if successful, -1 if error.
 */
static int parse_request(client_info *client, request_info *request,
                         parser_t *parser) {
    char buf[MAXLINE];
    // Read request line, check, and print.
    rio_t rio;
    rio_readinitb(&rio, client->connfd);
    // Read request line
    if (rio_readlineb(&rio, buf, sizeof(buf)) <= 0) {
        fprintf(stderr, "File read error.\n");
        return -1;
    }

    // Check parse request.
    parser_state parse_state = parser_parse_line(parser, buf);
    if (parse_state != REQUEST) {
        clienterror(client->connfd, "400", "Bad Request",
                    "Tiny received a malformed request");
        return -1;
    }

    // First, parse request line for header line info.
    parser_retrieve(parser, HOST, &request->host);
    parser_retrieve(parser, PORT, &request->port);
    parser_retrieve(parser, PATH, &request->path);
    parser_retrieve(parser, METHOD, &request->method);
    parser_retrieve(parser, URI, &request->uri);

    // Second, parse request header lines--load into parser.
    int strcmp_val;
    do {
        if (rio_readlineb(&rio, buf, sizeof(buf)) <= 0) {
            fprintf(stderr, "File read error.\n");
            return -1;
        }
        if (!!(strcmp_val = strcmp(buf, "\r\n")))
            parser_parse_line(parser, buf);
    } while (!!strcmp_val);
    return 0;
}

/**
 * @brief Writes client request to server file descriptor.
 *     Encapsulates all fd write operations.
 *
 * @param[in] fd_server : file descriptor used for server connection.
 * @param[in] client    : information regarding client connection.
 * @param[in] request   : information regarding request header line.
 * @param[in] parser    : HTTP parser, stores & parses request header lines.
 *
 * @return 0 if successful, -1 if errors.
 */
static int write_header(int fd_server, client_info *client,
                        request_info *request, parser_t *parser) {
    char buf[MAXLINE];
    // Start writing header.
    int buf_len =
        sprintf(buf, "%s %s HTTP/1.0\r\n", request->method, request->path);
    if (rio_writen(fd_server, buf, buf_len) < 0) {
        return -1;
    }

    // Write host header to text.
    header_t *line = parser_lookup_header(parser, "Host");
    char *connection = "Connection: close\r\n";
    char *proxy_conn = "Proxy-Connection: close\r\n";
    // Use existing host header or make one.
    buf_len =
        (line == NULL)
            ? sprintf(buf, "Host: %s:%s\r\nUser-Agent: %s\r\n%s%s",
                      request->host, request->port, header_user_agent,
                      connection, proxy_conn)
            : sprintf(buf, "Host: %s\r\nUser-Agent: %s\r\n%s%s", line->value,
                      header_user_agent, connection, proxy_conn);
    // Write host header to text.
    if (rio_writen(fd_server, buf, buf_len) < 0) {
        return -1;
    }

    // Write remaining request header lines.
    while ((line = parser_retrieve_next_header(parser)) != NULL) {
        int has_host = strcmp(line->name, "Host");
        int has_usag = strcmp(line->name, "User-Agent");
        int has_conn = strcmp(line->name, "Connection");
        int has_pxyc = strcmp(line->name, "Proxy-Connection");
        // Must not have any of the above headers.
        if (!!has_host && !!has_usag && !!has_conn && !!has_pxyc) {
            buf_len = sprintf(buf, "%s: %s\r\n", line->name, line->value);
            if (rio_writen(fd_server, buf, buf_len) < 0) {
                return -1;
            }
        }
    }
    // Write last line with \r\n
    sprintf(buf, "\r\n");
    if (rio_writen(fd_server, buf, buf_len) < 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief Returns an error message to the client.
 *
 * @param[in] fd       : file descriptor to write error message.
 * @param[in] errnum   : HTTP error number to output.
 * @param[in] shortmsg : short error message.
 * @param[in] longmsg  : long error message.
 */
static void clienterror(int fd, const char *errnum, const char *shortmsg,
                        const char *longmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];
    size_t buflen;
    size_t bodylen;
    // Build the HTTP response body
    bodylen = snprintf(body, MAXBUF,
                       "<!DOCTYPE html>\r\n"
                       "<html>\r\n"
                       "<head><title>Tiny Error</title></head>\r\n"
                       "<body bgcolor=\"ffffff\">\r\n"
                       "<h1>%s: %s</h1>\r\n"
                       "<p>%s</p>\r\n"
                       "<hr /><em>The Tiny Web server</em>\r\n"
                       "</body></html>\r\n",
                       errnum, shortmsg, longmsg);
    if (bodylen >= MAXBUF) {
        return; // Overflow!
    }
    // Build the HTTP response headers
    buflen = snprintf(buf, MAXLINE,
                      "HTTP/1.0 %s %s\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      errnum, shortmsg, bodylen);
    if (buflen >= MAXLINE) {
        return; // Overflow!
    }
    // Write the headers
    if (rio_writen(fd, buf, buflen) < 0) {
        fprintf(stderr, "Error writing error response headers to client\n");
        return;
    }
    // Write the body
    if (rio_writen(fd, body, bodylen) < 0) {
        fprintf(stderr, "Error writing error response body to client\n");
        return;
    }
}
