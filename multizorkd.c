#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <assert.h>

#define MULTIZORKD_VERSION "0.0.1"
#define MULTIZORKD_PORT 23  /* telnet! */
#define MULTIZORKD_MAXCONNECTIONS 1024  /* yolo...? */
#define MULTIZORKD_BACKLOG 16

static time_t GNow = 0;

static void loginfo(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("multizorkd: ");
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void panic(const char *errstr)
{
    loginfo(errstr);
    fflush(stdout);
    exit(1);
}

typedef struct Connection Connection;

typedef enum ConnectionState
{
    CONNSTATE_READY,
    CONNSTATE_DROP_REQUESTED,
    CONNSTATE_DRAINING,
    CONNSTATE_CLOSING,
    // never happens, we destroy the object once we hit this state. CONNSTATE_DISCONNECTED
} ConnectionState;

typedef void (*InputFn)(Connection *conn, const char *str);
struct Connection
{
    int sock;
    ConnectionState state;
    InputFn inputfn;
    char address[64];
    char username[16];
    int instanceidx;
    char instance[8]; // hash
    char player[8];  // hash
    char inputbuf[128];
    unsigned int inputbuf_used;
    int overlong_input;
    char *outputbuf;
    unsigned int outputbuf_len;
    unsigned int outputbuf_used;
    time_t last_activity;
};

static Connection *connections = NULL;
static size_t num_connections = 0;

// This queues a string for sending over the connection's socket when possible.
static void write_to_connection(Connection *conn, const char *str)
{
    if (conn->state != CONNSTATE_READY) {
        return;
    }

    const size_t slen = strlen(str);
    const size_t avail = conn->outputbuf_len - conn->outputbuf_used;
    if (avail < slen) {
        void *ptr = realloc(conn->outputbuf, conn->outputbuf_len + slen);
        if (!ptr) {
            panic("Uhoh, out of memory in write_to_connection");  // !!! FIXME: we could handle this more gracefully.
        }
        conn->outputbuf = (char *) ptr;
        conn->outputbuf_len += slen;
    }
    memcpy(conn->outputbuf + conn->outputbuf_used, str, slen);
    conn->outputbuf_used += slen;
}

static void drop_connection(Connection *conn)
{
    if (conn->state != CONNSTATE_READY) {
        return;  // already dropping.
    }

    loginfo("Starting drop of connection for socket %d", conn->sock);
    write_to_connection(conn, "\n\n");  // make sure we are a new line.
    conn->state = CONNSTATE_DRAINING;   // flush any pending output to the socket first.

    // !!! FIXME: find everyone else in the same instance and alert them that this connection is gone.
}

static void inpfn_new_game_or_join(Connection *conn, const char *str)
{
    if (strcmp(str, "1") == 0) {  // new game
        write_to_connection(conn, "!!! FIXME: write this part. Bye for now.");
        drop_connection(conn);
    } else if (strcmp(str, "2") == 0) {
        write_to_connection(conn, "!!! FIXME: write this part. Bye for now.");
        drop_connection(conn);
    } else if (strcmp(str, "3") == 0) {
        write_to_connection(conn, "\n\nOkay, bye for now!\n\n");
        drop_connection(conn);
    } else {
        write_to_connection(conn, "Please type '1', '2', or '3'");
    }
}

static void inpfn_enter_name(Connection *conn, const char *str)
{
    if (*str == '\0') {  // just hit enter without a specific code?
        write_to_connection(conn, "You have to enter a name. Try again.");
        return;
    }

    snprintf(conn->username, sizeof (conn->username), "%s", str);
    char msg[128];
    snprintf(msg, sizeof (msg), "Okay, we're referring to you as '%s' from now on.\n\n", conn->username);
    write_to_connection(conn, msg);
    write_to_connection(conn, "Now that that's settled:\n\n");
    write_to_connection(conn, "1) start a new game\n");
    write_to_connection(conn, "2) join someone else's game\n");
    write_to_connection(conn, "3) quit\n");
    conn->inputfn = inpfn_new_game_or_join;
}

// First prompt after connecting.
static void inpfn_hello_sailor(Connection *conn, const char *str)
{
    if (*str == '\0') {  // just hit enter without a specific code?
        write_to_connection(conn, "Okay, let's get you set up.\n\nWhat's your name? Keep it short or I'll truncate it.");
        conn->inputfn = inpfn_enter_name;
    } else {
        // look up player code.
        write_to_connection(conn, "!!! FIXME: write this part. Bye for now.");
        drop_connection(conn);
    }
}


// dump whitespace on both sides of a string.
static void trim(char *str)
{
    char *i = str;
    while ((*i == ' ') || (*i == '\t')) { i++; }
    if (i != str) {
        memmove(str, i, strlen(i) + 1);
    }
    i = (str + strlen(str)) - 1;
    while ((i != str) && ((*i == ' ') || (*i == '\t'))) { i--; }
    if ((*i != ' ') && (*i != '\t')) {
        i++;
    }
    *i = '\0';
}

static void process_connection_command(Connection *conn)
{
    conn->inputbuf[conn->inputbuf_used] = '\0';  // null-terminate the input.
    trim(conn->inputbuf);
    loginfo("New input from socket %d: '%s'", conn->sock, conn->inputbuf);
    // !!! FIXME: write to database if in an instance
    conn->inputfn(conn, conn->inputbuf);
    if (conn->state == CONNSTATE_READY) {
        write_to_connection(conn, "\n> ");  // prompt.
    }
}

// this queues data from the actual socket, and if there's a complete command,
//  we process it in here. We only read a little at a time instead of reading
//  until the socket is empty to give everyone a chance.
static void recv_from_connection(Connection *conn)
{
    if (conn->state != CONNSTATE_READY) {
        return;
    }

    char buf[128];
    const int br = recv(conn->sock, buf, sizeof (buf), 0);
loginfo("Got %d from recv on socket %d", br, conn->sock);
    if (br == -1) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            return;  // okay, just means there's nothing else to read.
        }
        loginfo("Socket %d has an error while receiving, dropping. (%s)", conn->sock, strerror(errno));
        drop_connection(conn);  // some other problem.
        return;
    } else if (br == 0) {  // socket has disconnected.
        loginfo("Socket %d has disconnected.", conn->sock);
        drop_connection(conn);
        return;
    }

    conn->last_activity = GNow;

    int avail = (int) ((sizeof (conn->inputbuf) - 1) - conn->inputbuf_used);
    for (int i = 0; i < br; i++) {
        const char ch = buf[i];
        if (((unsigned char) ch) == 255) {  // telnet Interpret As Command byte.
            i++;
            // !!! FIXME: fails on a buffer edge.
            if (i < br) {
                if (((unsigned char) buf[i]) >= 250) {
                    i++;
                }
            }
            continue;
        } else if (ch == '\n') {
            if (conn->overlong_input) {
                loginfo("Overlong input from socket %d", conn->sock);
                write_to_connection(conn, "Whoa, you're typing too much. Shorter commands, please.\n\n> ");
            } else {
                process_connection_command(conn);
            }
            conn->overlong_input = 0;
            conn->inputbuf_used = 0;
            avail = sizeof (conn->inputbuf);
        } else if ((ch >= 32) && (ch < 127)) {  // basic ASCII only, sorry.
            if (!avail) {
                conn->overlong_input = 1;  // drop this command.
            } else {
                conn->inputbuf[conn->inputbuf_used++] = ch;
                avail--;
            }
        }
    }
}

// this sends data queued by write_to_connection() down the actual socket.
static void send_to_connection(Connection *conn)
{
    if (conn->outputbuf_used == 0) {
        return;  // nothing to send atm.
    } else if (conn->state > CONNSTATE_DRAINING) {
        conn->outputbuf_used = 0;  // just make sure we don't poll() for this again.
        return;
    }

    const ssize_t bw = send(conn->sock, conn->outputbuf, conn->outputbuf_used, 0);
    if (bw == -1) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            return;  // okay, just means try again later.
        }
        loginfo("Socket %d has an error while sending, dropping. (%s)", conn->sock, strerror(errno));
        drop_connection(conn);  // some other problem.
        return;
    } else if (bw == 0) {  // socket has disconnected.
        loginfo("Socket %d has disconnected.", conn->sock);
        drop_connection(conn);
        return;
    }

    assert(bw <= conn->outputbuf_used);

    if (bw < conn->outputbuf_used) {  /* !!! FIXME: lazy, not messing with pointers here. */
        memmove(conn->outputbuf, conn->outputbuf + bw, conn->outputbuf_used - bw);
    }

    conn->outputbuf_used -= bw;

    if ((conn->state == CONNSTATE_DRAINING) && (conn->outputbuf_used == 0)) {
        loginfo("Finished draining output buffer for socket %d, moving to close.", conn->sock);
        conn->state = CONNSTATE_CLOSING;
    }
}

static int accept_new_connection(const int listensock)
{
    Connection *conn = NULL;
    struct sockaddr_storage addr;
    socklen_t addrlen = (socklen_t) sizeof (addr);
    const int sock = accept(listensock, (struct sockaddr *) &addr, &addrlen);
    if (sock == -1) {
        loginfo("accept() reported an error! We ignore it! (%s)", strerror(errno));
        return -1;
    }

    if (fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
        loginfo("Failed to set newly-accept()'d socket as non-blocking! Dropping! (%s)", strerror(errno));
        close(sock);
        return -1;
    }

    void *ptr = realloc(connections, sizeof (Connection) * (num_connections + 1));
    if (!ptr) {
        loginfo("Uhoh, out of memory, dropping new connection in socket %d!", sock);
        close(sock);
        return -1;
    }

    connections = (Connection *) ptr;
    conn = &connections[num_connections];
    num_connections++;
    memset(conn, '\0', sizeof (*conn));
    conn->sock = sock;
    conn->inputfn = inpfn_hello_sailor;
    conn->instanceidx = -1;
    conn->last_activity = GNow;

    if (getnameinfo((struct sockaddr *) &addr, addrlen, conn->address, sizeof (conn->address), NULL, 0, NI_NUMERICHOST|NI_NUMERICSERV) != 0) {
        snprintf(conn->address, sizeof (conn->address), "???");
    }

    loginfo("New connection from %s (socket %d)", conn->address, sock);

    write_to_connection(conn, "\n\nHello sailor!\n\nIf you got disconnected, go ahead and type in your access code.\nOtherwise, just press enter.\n\n> ");

    return sock;
}

static int prep_listen_socket(const int port, const int backlog)
{
    char service[32];
    const int one = 1;
    struct addrinfo hints;
    struct addrinfo *ainfo = NULL;

    memset(&hints, '\0', sizeof (hints));
    hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_PASSIVE;    // AI_PASSIVE for the "any" address.
    snprintf(service, sizeof (service), "%u", (unsigned int) port);
    const int gairc = getaddrinfo(NULL, service, &hints, &ainfo);
    if (gairc != 0) {
        loginfo("getaddrinfo() failed to find where we should bind! (%s)", gai_strerror(gairc));
        return -1;
    }

    for (struct addrinfo *i = ainfo; i != NULL; i = i->ai_next) {
        const int fd = socket(i->ai_family, i->ai_socktype, i->ai_protocol);
        if (fd == -1) {
            loginfo("socket() didn't create a listen socket! Will try other options! (%s)", strerror(errno));
            continue;
        }

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof (one)) == -1) {
            loginfo("Failed to setsockopt(SO_REUSEADDR) the listen socket! Will try other options! (%s)", strerror(errno));
            close(fd);
            continue;
        } else if (bind(fd, i->ai_addr, i->ai_addrlen) == -1) {
            loginfo("Failed to bind() the listen socket! Will try other options! (%s)", strerror(errno));
            close(fd);
            continue;
        } else if (listen(fd, backlog) == -1) {
            loginfo("Failed to listen() on the listen socket! Will try other options! (%s)", strerror(errno));
            close(fd);
            continue;
        }

        freeaddrinfo(ainfo);
        return fd;
    }

    loginfo("Failed to create a listen socket on any reasonable interface.");
    freeaddrinfo(ainfo);
    return -1;
}

// !!! FIXME: command line handling and less hardcoding.
int main(int argc, char **argv)
{
    GNow = time(NULL);

    loginfo("multizork daemon %s starting up...", MULTIZORKD_VERSION);

    struct pollfd *pollfds = NULL;

    pollfds = (struct pollfd *) calloc(1, sizeof (struct pollfd));
    if (!pollfds) {
        panic("Out of memory creating pollfd array!");
    }

    const int listensock = prep_listen_socket(MULTIZORKD_PORT, MULTIZORKD_BACKLOG);
    if (listensock == -1) {
        panic("Can't go on without a listen socket!");
    }

    //drop_privileges();

    //connect_to_database();

    loginfo("Now accepting connections on port %d (socket %d).", MULTIZORKD_PORT, listensock);

    while (1) {   // !!! FIXME: signal server to shutdown?
        pollfds[0].fd = listensock;
        pollfds[0].events = POLLIN | POLLOUT;
        for (int i = 0; i < num_connections; i++) {
            pollfds[i+1].fd = connections[i].sock;
            pollfds[i+1].events = (connections[i].outputbuf_used > 0) ? (POLLIN | POLLOUT) : POLLIN;
            pollfds[i+1].revents = 0;
        }

        const int pollrc = poll(pollfds, num_connections + 1, -1);  // !!! FIXME: timeout.
        if (pollrc == -1) {
            loginfo("poll() reported an error! (%s)", strerror(errno));
            panic("Giving up.");
        }

        GNow = time(NULL);

        for (int i = 0; i <= num_connections; i++) {
            const short revents = pollfds[i].revents;
            if (revents == 0) { continue; }  // nothing happening here.
            if (pollfds[i].fd < 0) { continue; }   // not a socket in use.

            loginfo("New activity on socket %d", pollfds[i].fd);

            if (i == 0) {  // new connection.
                assert(pollfds[0].fd == listensock);
                if (revents & POLLERR) {
                    panic("Listen socket had an error! Giving up!");
                }
                assert(revents & POLLIN);
                const int sock = accept_new_connection(listensock);
                if (sock != -1) {
                    void *ptr = realloc(pollfds, sizeof (struct pollfd) * (num_connections + 1));
                    if (ptr == NULL) {
                        close(sock);  // just drop them, oh well.
                        num_connections--;
                        loginfo("Uhoh, out of memory reallocating pollfds!");
                    } else {
                        pollfds = (struct pollfd *) ptr;
                        pollfds[num_connections].fd = sock;
                        pollfds[num_connections].events = POLLIN;
                        pollfds[num_connections].revents = 0;
                    }
                }
            } else {
                Connection *conn = &connections[i-1];
                if (revents & POLLIN) {
                    recv_from_connection(conn);
                }
                if (revents & POLLOUT) {
                    send_to_connection(conn);
                }
            }
        }

        // cleanup any done sockets.
        for (int i = 0; i < num_connections; i++) {
            Connection *conn = &connections[i];
            /*if ((conn->state == CONNSTATE_READY) && ((GNow - conn->last_activity) > IDLE_KICK_TIMEOUT))
                write_to_connection(conn, "Dropping you because you seem to be AFK.");
                drop_connection(conn);
            } else*/ if (conn->state == CONNSTATE_CLOSING) {
                const int rc = (conn->sock < 0) ? 0 : close(conn->sock);
                // closed, or failed for a reason other than still trying to flush final writes, dump it.
                if ((rc == 0) || ((errno != EAGAIN) && (errno != EWOULDBLOCK))) {
                    loginfo("Closed socket %d, removing connection object.", conn->sock);
                    free(conn->outputbuf);
                    if (i != (num_connections-1)) {
                        memmove(conn, conn+1, sizeof (*conn) * ((num_connections - i) - 1));
                    }
                    i--;
                    num_connections--;
                }
            }
        }
    }

    // shutdown!

    close(listensock);

    for (int i = 0; i < num_connections; i++) {
        if (connections[i].sock >= 0) {
            close(connections[i].sock);
        }
        free(connections[i].outputbuf);
    }

    free(connections);
    free(pollfds);

    return 0;
}

