#include "tcpserver.h"
#include "log.h"

#include <stdio.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ev.h>

#define WAIT_TIME 5000000

static tcpsession_t *tcpsession_create(tcplistener_t *listener) {
	tcpsession_t *session;

	session = malloc(sizeof(tcpsession_t));
	if (session == NULL) {
		return NULL;
	}

	session->watcher = malloc(sizeof(struct ev_io));
	if (session->watcher == NULL) {
		free(session);
		return NULL;
	}

	session->loop = listener->loop;
	session->data = listener->data;
	// Save a ptr to the Socket descriptor vector
	// in the session object, to allow cleanup.
	session->sdPtr = &listener->sdList;
	session->sd = -1;
	session->cb_recv = listener->cb_recv;
	session->watcher->data = (void *)session;
	session->ctx = NULL;
	session->ctx_dealloc = NULL;
	return session;
}

// debug routine for monitoring the vector contents
static void vector_print(vector_t *v, int vector_sz) {
	tcpsession_t *session;
	for (int i = 0; i < vector_sz; i++) {
		session = (tcpsession_t *)vector_fetch(v, i);
		if (session != NULL) {
			stats_debug_log("tcpserver: socket descriptor connected %d", session->sd);
		}
	}
}

// searches and removes all the items with session
// structure socket descriptor in the vector we maintain
// also resizes the vector
static void vector_remove(vector_t *v, int sd) {
	tcpsession_t *session;
	int vector_sz = vector_count(v);
	for (int v_index = 0; v_index < vector_sz; v_index++) {
		session = (tcpsession_t *)v->items[v_index];
		if (session != NULL && session->sd == sd) {
			stats_debug_log("tcpserver: removing %d, from SD vector", session->sd);
			vector_delete_at(v, v_index);
		}
	}
	return;
}

// Called everytime session close is initiated
// also removes the session socket descriptor from
// the vector we maintain
static void tcpsession_destroy(tcpsession_t *session) {
	if (session->sd > 0) {
		close(session->sd);
	}
    /**
     * Loop over the cached descriptors
     * and remove it from the list
     */
	if (session->sdPtr != NULL) {
		stats_debug_log("tcpsession: removing %d, from SD vector", session->sd);
		vector_remove(session->sdPtr, session->sd);
	}

	ev_io_stop(session->loop, session->watcher);
	free(session->watcher);
	free(session);
}

// Called every time the session socket is readable (data available)
// if you don't consume it, it'll get called again very quickly
static void tcpsession_recv_callback(struct ev_loop *loop,
		struct ev_io *watcher,
		int revents) {
	tcpsession_t *session;

	if (revents & EV_ERROR) {
		// ev(3) says this is an error of "unspecified" type, so
		// that's bloody useful.
		stats_error_log("tcpsession: libev server socket error");
		return;
	}

	session = (tcpsession_t *)watcher->data;
	if (session == NULL) {
		stats_error_log("tcpsession: Unable to determine tcpsession, not calling recv callback");
		return;
	}

	if (session->cb_recv == NULL) {
		stats_error_log("tcpsession: No recv callback registered for session, ignoring event");
		return;
	}

	if (session->cb_recv(session->sd, session->data, session->ctx) != 0) {
		stats_error_log("tcpsession: recv callback returned non-zero, closing connection");
		tcpsession_destroy(session);
		return;
	}
}

// Called every time the server socket is readable (new connection to be accepted)
// if you don't consume it, it'll get called again very quickly
static void tcplistener_accept_callback(struct ev_loop *loop,
		struct ev_io *watcher,
		int revents) {
	stats_debug_log("in tcplistener_accept_callback pid:%d, parentpid:%d", getpid(), getppid());
	socklen_t sin_size;
	tcplistener_t *listener;
	tcpsession_t *session;
	int err, vector_sz;

	if (revents & EV_ERROR) {
		// ev(3) says this is an error of "unspecified" type, so
		// that's bloody useful.
		stats_error_log("tcplistener: libev server socket error");
		return;
	}

	listener = (tcplistener_t *) watcher->data;
	session = tcpsession_create(listener);
	if (session == NULL) {
		stats_error_log("tcplistener: Unable to allocate tcpsession, not calling accept()");
		return;
	}

	sin_size = sizeof(session->client_addr);

	stats_debug_log("tcplistener: accept on %d mypid: %d, parentpid: %d\n", watcher->fd, getpid(), getppid());
	session->sd = accept(watcher->fd, (struct sockaddr *)&session->client_addr, &sin_size);
	stats_debug_log("tcpserver: accepted new tcp client connection, client fd = %d, tcp server fd = %d", session->sd, watcher->fd);
	if (session->sd < 0) {
		stats_error_log("tcplistener: Error accepting connection: %s", strerror(errno));
		return;
	}

	err = fcntl(session->sd, F_SETFL, (fcntl(session->sd, F_GETFL) | O_NONBLOCK));
	if (err != 0) {
		stats_error_log("tcplistener: Error setting socket to non-blocking: %s", strerror(errno));
		return;
	}

	session->ctx = listener->cb_conn(session->sd, session->data);

    // Save the new connection socket descriptor
    // into the vector, for future cleanup!
	vector_insert(&listener->sdList, (void *)session);

	ev_io_init(session->watcher, tcpsession_recv_callback, session->sd, EV_READ);
	ev_io_start(loop, session->watcher);
}

tcpserver_t *tcpserver_create(struct ev_loop *loop, void *data) {
	tcpserver_t *server;
	server = (tcpserver_t *) malloc(sizeof(tcpserver_t));
	server->loop = ev_default_loop(0);
	server->listeners_len = 0;
	server->data = data;
	return server;
}

static tcplistener_t *tcplistener_create(tcpserver_t *server,
		struct addrinfo *addr,
		bool rebind,
		void *(*cb_conn)(int, void *),
		int (*cb_recv)(int, void *, void *)) {
	tcplistener_t *listener;
	char addr_string[INET6_ADDRSTRLEN];
	char sd_buffer[10];
	void *ip;
	int port;
	int yes = 1;
	int err;

	listener = malloc(sizeof(tcplistener_t));
	listener->loop = server->loop;
	listener->data = server->data;
	listener->cb_conn = cb_conn;
	/**
	 * Do not pass it to the child
	 * let the child get it own copy
	 * since we are going to close
	 * all the session sockets on USR2
	 */
	vector_init(&listener->sdList, SESSION_SDS_VECTOR_INITIAL_SIZE);
	listener->cb_recv = cb_recv;

	/**
	 * not a hot restart, create and bind
	 */
	if (rebind) {
		listener->sd = socket(
				addr->ai_family,
				addr->ai_socktype,
				addr->ai_protocol);
		snprintf(sd_buffer, 10, "%d", listener->sd);
		sd_buffer[9] = '\0';

		stats_log("statsrelay: master set to listen on tcp socket fd %d", listener->sd);

		/** setenv for hotrestart **/
		setenv("STATSRELAY_LISTENER_TCP_SD", sd_buffer, 1);
	} else {
		listener->sd = atoi(getenv("STATSRELAY_LISTENER_TCP_SD"));
		stats_log("statsrelay: new master reusing tcp socket descriptor %ld", listener->sd);
	}

	memset(addr_string, 0, INET6_ADDRSTRLEN);
	if (addr->ai_family == AF_INET) {
		struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr->ai_addr;
		ip = &(ipv4->sin_addr);
		port = ntohs(ipv4->sin_port);
	} else {
		struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)addr->ai_addr;
		ip = &(ipv6->sin6_addr);
		port = ntohs(ipv6->sin6_port);
	}
	if (inet_ntop(addr->ai_family, ip, addr_string, addr->ai_addrlen) == NULL) {
		stats_error_log("tcplistener: Unable to format network address string");
		free(listener);
		return NULL;
	}

	if (listener->sd < 0) {
		stats_error_log("tcplistener: Error creating socket %s[:%i]: %s", addr_string, port, strerror(errno));
		free(listener);
		return NULL;
	}

	err = setsockopt(listener->sd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
	if (err != 0) {
		stats_error_log("tcplistener: Error setting SO_REUSEADDR on %s[:%i]: %s", addr_string, port, strerror(errno));
		free(listener);
		return NULL;
	}

	err = fcntl(listener->sd, F_SETFL, (fcntl(listener->sd, F_GETFL) | O_NONBLOCK));
	if (err != 0) {
		stats_error_log("tcplistener: Error setting socket to non-blocking for %s[:%i]: %s", addr_string, port, strerror(errno));
		free(listener);
		return NULL;
	}

	if (rebind) {
		/**
		 * Bind only once in the original master
		 */
		err = bind(listener->sd, addr->ai_addr, addr->ai_addrlen);
		if (err != 0) {
			stats_error_log("tcplistener: Error binding socket for %s[:%i]: %s", addr_string, port, strerror(errno));
			free(listener);
			return NULL;
		}
	}

	err = listen(listener->sd, LISTEN_BACKLOG);
	if (err != 0) {
		stats_error_log("tcplistener: Error listening to socket %s[:%i]: %s", addr_string, port, strerror(errno));
		free(listener);
		return NULL;
	}

	listener->watcher = malloc(sizeof(struct ev_io));
	listener->watcher->data = (void *) listener;

	stats_debug_log("tcpserver: pid:%d ppid:%d has listener sd:%d", getpid(), getppid(), listener->sd);

	ev_io_init(listener->watcher, tcplistener_accept_callback, listener->sd, EV_READ);
	stats_log("tcpserver: Listening on frontend %s[:%i], fd = %d",
			addr_string, port, listener->sd);

	return listener;
}

int tcpserver_bind(tcpserver_t *server,
		const char *address_and_port,
		bool rebind,
		void *(*cb_conn)(int, void *),
		int (*cb_recv)(int, void *, void *)) {
	tcplistener_t *listener;
	struct addrinfo hints;
	struct addrinfo *addrs, *p;
	int err;

	char *address = strdup(address_and_port);
	if (address == NULL) {
		stats_error_log("tcpserver: strdup(3) failed");
		return 1;
	}

	char *ptr = strrchr(address_and_port, ':');
	if (ptr == NULL) {
		free(address);
		stats_error_log("tcpserver: missing port");
		return 1;
	}
	const char *port = ptr + 1;
	address[ptr - address_and_port] = '\0';

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	err = getaddrinfo(address, port, &hints, &addrs);
	if (err != 0) {
		free(address);
		stats_error_log("tcpserver: getaddrinfo error: %s", gai_strerror(err));
		return 1;
	}

	for (p = addrs; p != NULL; p = p->ai_next) {
		if (server->listeners_len >= MAX_TCP_HANDLERS) {
			stats_error_log("tcpserver: Unable to create more than %i TCP listeners", MAX_TCP_HANDLERS);
			free(address);
			freeaddrinfo(addrs);
			return 1;
		}
		listener = tcplistener_create(server, p, rebind, cb_conn, cb_recv);
		if (listener == NULL) {
			continue;
		}
		server->listeners[server->listeners_len] = listener;
		server->listener_fds[server->listeners_len] = listener->sd;
		server->listeners_len++;
		ev_io_start(server->loop, listener->watcher);
	}

	free(address);
	freeaddrinfo(addrs);
	return 0;
}

// This step is used to send a shutdown signal to the connected
// clients and allowing time to drain read buffer of whatever
// was already send over the tcp socket
static void tcpsession_client_close(tcplistener_t *listener) {
	tcpsession_t *session;
	vector_t *sdRef;
	int vector_sz;

	sdRef = &listener->sdList;
	vector_sz = vector_count(sdRef);

	stats_debug_log("tcpserver: number of sockets to shutdown %d", vector_sz);
	for (int v_index = 0; v_index < vector_sz; v_index++) {
		session = (tcpsession_t *)vector_fetch(sdRef, v_index);
		if (session != NULL) {
			stats_log("Send close to %d", session->sd);
			if (shutdown(session->sd, SHUT_WR) < 0) {
				stats_error_log("tcpserver: shutdown socket close error %s", strerror(errno));
			}
		}
	}
}

static void tcpsession_kill_watchers(tcplistener_t *listener) {
    /**
     * Sleep for a second to allow
     * the data to be transferred out
     * to the backend client
     */
    usleep(WAIT_TIME);

	tcpsession_t *session;
	vector_t *sdRef;
	int vector_sz;

	sdRef = &listener->sdList;
	vector_sz = vector_count(sdRef);

	stats_debug_log("tcpserver: number of sockets to kill watchers on %d", vector_sz);
	for (int v_index = 0; v_index < vector_sz; v_index++) {
    		session = (tcpsession_t *)vector_fetch(sdRef, v_index);

	        if (session != NULL) {
                ev_io_stop(session->loop, session->watcher);
                free(session->watcher);
                vector_delete_at(sdRef, v_index);
                free(session);
            }
    }
	vector_free(sdRef);
}

static void tcplistener_free(tcplistener_t *listener) {
    stats_log("tcplistener: close tcp socket %d", listener->sd);
    if (close(listener->sd) < 0) {
        stats_error_log("tcplistener: attempting close tcp socket %s", strerror(errno));
    }
	free(listener);
}

// helper that actually shutsdown the listener
static void tcplistener_destroy(tcpserver_t *server, tcplistener_t *listener) {
	if (listener->watcher != NULL) {
		ev_io_stop(server->loop, listener->watcher);
		free(listener->watcher);
	}
}

// kills the accept watcher, to prevent further incoming connection
// on the tcp socket. Then we allow the read buffer to drain.
void tcpserver_stop_accepting_connections(tcpserver_t *server) {
	for (int i = 0; i < server->listeners_len; i++) {
		tcplistener_destroy(server, server->listeners[i]);
	}
}

// this should send shutdown(sd, 1) to the connected clients
// and free up the listeners
void tcpserver_destroy_session_sockets(tcpserver_t *server) {
	for (int i = 0; i < server->listeners_len; i++) {
		tcpsession_client_close(server->listeners[i]);
        tcpsession_kill_watchers(server->listeners[i]);
		tcplistener_free(server->listeners[i]);
	}
	server->listeners_len = -1;	
}

// reliquishes the server object
void tcpserver_destroy(tcpserver_t *server) {
	free(server);
}
