/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

/** @file ndsctl_thread.c
    @brief Monitoring and control of nodogsplash, server part
    @author Copyright (C) 2004 Alexandre Carmel-Veilleux <acv@acv.ca>
    trivially modified for nodogsplash
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "common.h"
#include "util.h"
#include "conf.h"
#include "debug.h"
#include "auth.h"
#include "safe.h"
#include "client_list.h"
#include "fw_iptables.h"
#include "gateway.h"

#include "ndsctl_thread.h"

#define MAX_EVENT_SIZE 30

/* Defined in clientlist.c */
extern pthread_mutex_t client_list_mutex;
extern pthread_mutex_t config_mutex;

static void ndsctl_handler(int fd);
static void ndsctl_stop();
static void ndsctl_block(FILE *fp, char *arg);
static void ndsctl_unblock(FILE *fp, char *arg);
static void ndsctl_allow(FILE *fp, char *arg);
static void ndsctl_unallow(FILE *fp, char *arg);
static void ndsctl_trust(FILE *fp, char *arg);
static void ndsctl_untrust(FILE *fp, char *arg);
static void ndsctl_auth(FILE *fp, char *arg);
static void ndsctl_deauth(FILE *fp, char *arg);
static void ndsctl_loglevel(FILE *fp, char *arg);

static int socket_set_non_blocking(int sockfd);

/** Launches a thread that monitors the control socket for request
@param arg Must contain a pointer to a string containing the Unix domain socket to open
@todo This thread loops infinitely, need a watchdog to verify that it is still running?
*/
void*
thread_ndsctl(void *arg)
{
	int sock, fd, epoll_fd;
	const char *sock_name;
	struct sockaddr_un sa_un;
	socklen_t len;
	struct epoll_event ev;
	struct epoll_event *events;
	int current_fd_count;
	int number_of_count;
	int i;

	debug(LOG_DEBUG, "Starting ndsctl.");

	memset(&sa_un, 0, sizeof(sa_un));
	sock_name = (char *)arg;
	debug(LOG_DEBUG, "Socket name: %s", sock_name);

	if (strlen(sock_name) > (sizeof(sa_un.sun_path) - 1)) {
		/* TODO: Die handler with logging.... */
		debug(LOG_ERR, "NDSCTL socket name too long");
		exit(1);
	}

	debug(LOG_DEBUG, "Creating socket");
	sock = socket(PF_UNIX, SOCK_STREAM, 0);

	debug(LOG_DEBUG, "Got server socket %d", sock);

	/* If it exists, delete... Not the cleanest way to deal. */
	unlink(sock_name);

	debug(LOG_DEBUG, "Filling sockaddr_un");
	strcpy(sa_un.sun_path, sock_name); /* XXX No size check because we check a few lines before. */
	sa_un.sun_family = AF_UNIX;

	debug(LOG_DEBUG, "Binding socket (%s) (%d)", sa_un.sun_path, strlen(sock_name));

	/* Which to use, AF_UNIX, PF_UNIX, AF_LOCAL, PF_LOCAL? */
	if (bind(sock, (struct sockaddr *)&sa_un, strlen(sock_name) + sizeof(sa_un.sun_family))) {
		debug(LOG_ERR, "Could not bind control socket: %s", strerror(errno));
		pthread_exit(NULL);
	}

	if (listen(sock, 5)) {
		debug(LOG_ERR, "Could not listen on control socket: %s", strerror(errno));
		pthread_exit(NULL);
	}

	memset(&ev, 0, sizeof(struct epoll_event));
	epoll_fd = epoll_create(MAX_EVENT_SIZE);

	ev.events = EPOLLIN|EPOLLET;
	ev.data.fd = sock;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) < 0) {
		debug(LOG_ERR, "Could not insert socket fd to epoll set: %s", strerror(errno));
		pthread_exit(NULL);
	}

	events = (struct epoll_event*)calloc(MAX_EVENT_SIZE, sizeof(struct epoll_event));

	if (!events) {
		close(sock);
		pthread_exit(NULL);
	}

	current_fd_count = 1;

	while (1) {
		memset(&sa_un, 0, sizeof(sa_un));
		len = (socklen_t) sizeof(sa_un);

		number_of_count = epoll_wait(epoll_fd, events, current_fd_count, -1);

		if (number_of_count == -1) {
			debug(LOG_ERR, "Failed to wait epoll events: %s", strerror(errno));
			free(events);
			pthread_exit(NULL);
		}

		for (i = 0; i < number_of_count; i++) {

			if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) ||
				(!(events[i].events & EPOLLIN))) {
				debug(LOG_ERR, "Socket is not ready for communication : %s", strerror(errno));

				if (events[i].data.fd > 0) {
					shutdown(events[i].data.fd, 2);
					close(events[i].data.fd);
					events[i].data.fd = 0;
				}
				continue;
			}

			if (events[i].data.fd == sock) {
				if ((fd = accept(events[i].data.fd, (struct sockaddr *)&sa_un, &len)) == -1) {
					debug(LOG_ERR, "Accept failed on control socket: %s", strerror(errno));
					free(events);
					pthread_exit(NULL);
				} else {
					socket_set_non_blocking(fd);
					ev.events = EPOLLIN|EPOLLET;
					ev.data.fd = fd;

					if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
						debug(LOG_ERR, "Could not insert socket fd to epoll set: %s", strerror(errno));
						free(events);
						pthread_exit(NULL);
					}

					current_fd_count += 1;
				}

			} else {
				ndsctl_handler(fd);
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, &ev);
				current_fd_count -= 1;

				/* socket was closed on 'ndsctl_handler' */
				if (events[i].data.fd > 0) {
					events[i].data.fd = 0;
				}

			}
		}
	}

	free(events);

	return NULL;
}

static void
ndsctl_handler(int fd)
{
	int done, i;
	char request[MAX_BUF];
	ssize_t read_bytes, len;
	FILE* fp;

	debug(LOG_DEBUG, "Entering thread_ndsctl_handler....");
	debug(LOG_DEBUG, "Read bytes and stuff from descriptor %d", fd);

	/* Init variables */
	read_bytes = 0;
	done = 0;
	memset(request, 0, sizeof(request));
	fp = fdopen(fd, "w");

	/* Read.... */
	while (!done && read_bytes < (sizeof(request) - 1)) {
		len = read(fd, request + read_bytes, sizeof(request) - read_bytes);

		/* Have we gotten a command yet? */
		for (i = read_bytes; i < (read_bytes + len); i++) {
			if (request[i] == '\r' || request[i] == '\n') {
				request[i] = '\0';
				done = 1;
			}
		}

		/* Increment position */
		read_bytes += len;
	}

	debug(LOG_DEBUG, "ndsctl request received: [%s]", request);

	if (strncmp(request, "status", 6) == 0) {
		ndsctl_status(fp);
	} else if (strncmp(request, "clients", 7) == 0) {
		ndsctl_clients(fp);
	} else if (strncmp(request, "json", 4) == 0) {
		ndsctl_json(fp);
	} else if (strncmp(request, "stop", 4) == 0) {
		ndsctl_stop();
	} else if (strncmp(request, "block", 5) == 0) {
		ndsctl_block(fp, (request + 6));
	} else if (strncmp(request, "unblock", 7) == 0) {
		ndsctl_unblock(fp, (request + 8));
	} else if (strncmp(request, "allow", 5) == 0) {
		ndsctl_allow(fp, (request + 6));
	} else if (strncmp(request, "unallow", 7) == 0) {
		ndsctl_unallow(fp, (request + 8));
	} else if (strncmp(request, "trust", 5) == 0) {
		ndsctl_trust(fp, (request + 6));
	} else if (strncmp(request, "untrust", 7) == 0) {
		ndsctl_untrust(fp, (request + 8));
	} else if (strncmp(request, "auth", 4) == 0) {
		ndsctl_auth(fp, (request + 5));
	} else if (strncmp(request, "deauth", 6) == 0) {
		ndsctl_deauth(fp, (request + 7));
	} else if (strncmp(request, "loglevel", 8) == 0) {
		ndsctl_loglevel(fp, (request + 9));
	}

	if (!done) {
		debug(LOG_ERR, "Invalid ndsctl request.");
	}

	debug(LOG_DEBUG, "ndsctl request processed: [%s]", request);
	debug(LOG_DEBUG, "Exiting thread_ndsctl_handler....");

	/* Close and flush fp, also closes underlying fd */
	fclose(fp);
}

static t_client *find_client(const char *arg)
{
	t_client *client;

	if ((client = client_list_find_by_ip(arg)) != NULL);
	else if ((client = client_list_find_by_mac(arg)) != NULL);
	else if ((client = client_list_find_by_token(arg)) != NULL);

	return client;
}

/** A bit of an hack, self kills.... */
static void
ndsctl_stop()
{
	pthread_exit(NULL);
}

static void
ndsctl_auth(FILE *fp, char *arg)
{
	t_client *client;
	s_config *config;
	char ip[INET6_ADDRSTRLEN+1];
	char mac[18];

	debug(LOG_DEBUG, "Entering ndsctl_auth [%s]", arg);
	config = config_get_config();

	LOCK_CLIENT_LIST();
	client = find_client(arg);

	if (client && client->fw_connection_state != FW_MARK_AUTHENTICATED) {
		client->session_start = time(NULL);
		if (config->session_timeout) {
			client->session_end = client->session_start + config->session_timeout;
		} else {
			client->session_end = 0;
		}

		if (client && config->bin_auth) {
			// Client will be authenticated...
			execute("%s manual_auth %s %llu %llu %llu",
				config->bin_auth,
				client->mac,
				client->counters.incoming,
				client->counters.outgoing,
				client->session_start,
				client->session_end
			);
		}
	}

	UNLOCK_CLIENT_LIST();

	if (client) {
		auth_client_authenticate(ip, mac);
		fprintf(fp, "Yes");
	} else {
		debug(LOG_DEBUG, "Client not found.");
		fprintf(fp, "No");
	}

	debug(LOG_DEBUG, "Exiting ndsctl_auth...");
}

static void
ndsctl_deauth(FILE *fp, char *arg)
{
	t_client *client;
	s_config *config;
	char ip[INET6_ADDRSTRLEN+1];
	char mac[18];

	debug(LOG_DEBUG, "Entering ndsctl_deauth [%s]", arg);
	config = config_get_config();

	LOCK_CLIENT_LIST();
	client = find_client(arg);
	UNLOCK_CLIENT_LIST();

	if (client && config->bin_auth) {
		// Client will be deauthenticated...
		execute("%s manual_deauth %s %llu %llu %llu %llu",
			config->bin_auth,
			client->mac,
			client->counters.incoming,
			client->counters.outgoing,
			client->session_start,
			client->session_end
		);
	}

	if (client) {
		auth_client_deauthenticate(ip, mac);
		fprintf(fp, "Yes");
	} else {
		debug(LOG_DEBUG, "Client not found.");
		fprintf(fp, "No");
	}

	debug(LOG_DEBUG, "Exiting ndsctl_deauth...");
}

static void
ndsctl_block(FILE *fp, char *arg)
{
	debug(LOG_DEBUG, "Entering ndsctl_block [%s]", arg);

	LOCK_CONFIG();

	if (!add_to_blocked_mac_list(arg) && !iptables_block_mac(arg)) {
		fprintf(fp, "Yes");
	} else {
		fprintf(fp, "No");
	}

	UNLOCK_CONFIG();

	debug(LOG_DEBUG, "Exiting ndsctl_block.");
}

static void
ndsctl_unblock(FILE *fp, char *arg)
{
	debug(LOG_DEBUG, "Entering ndsctl_unblock [%s]", arg);

	LOCK_CONFIG();

	if (!remove_from_blocked_mac_list(arg) && !iptables_unblock_mac(arg)) {
		fprintf(fp, "Yes");
	} else {
		fprintf(fp, "No");
	}

	UNLOCK_CONFIG();

	debug(LOG_DEBUG, "Exiting ndsctl_unblock.");
}

static void
ndsctl_allow(FILE *fp, char *arg)
{
	debug(LOG_DEBUG, "Entering ndsctl_allow [%s]", arg);

	LOCK_CONFIG();

	if (!add_to_allowed_mac_list(arg) && !iptables_allow_mac(arg)) {
		fprintf(fp, "Yes");
	} else {
		fprintf(fp, "No");
	}

	UNLOCK_CONFIG();

	debug(LOG_DEBUG, "Exiting ndsctl_allow.");
}

static void
ndsctl_unallow(FILE *fp, char *arg)
{
	debug(LOG_DEBUG, "Entering ndsctl_unallow [%s]", arg);

	LOCK_CONFIG();

	if (!remove_from_allowed_mac_list(arg) && !iptables_unallow_mac(arg)) {
		fprintf(fp, "Yes");
	} else {
		fprintf(fp, "No");
	}

	UNLOCK_CONFIG();

	debug(LOG_DEBUG, "Exiting ndsctl_unallow.");
}

static void
ndsctl_trust(FILE *fp, char *arg)
{
	debug(LOG_DEBUG, "Entering ndsctl_trust [%s]", arg);

	LOCK_CONFIG();

	if (!add_to_trusted_mac_list(arg) && !iptables_trust_mac(arg)) {
		fprintf(fp, "Yes");
	} else {
		fprintf(fp, "No");
	}

	UNLOCK_CONFIG();

	debug(LOG_DEBUG, "Exiting ndsctl_trust.");
}

static void
ndsctl_untrust(FILE *fp, char *arg)
{
	debug(LOG_DEBUG, "Entering ndsctl_untrust [%s]", arg);

	LOCK_CONFIG();

	if (!remove_from_trusted_mac_list(arg) && !iptables_untrust_mac(arg)) {
		fprintf(fp, "Yes");
	} else {
		fprintf(fp, "No");
	}

	UNLOCK_CONFIG();

	debug(LOG_DEBUG, "Exiting ndsctl_untrust.");
}

static void
ndsctl_loglevel(FILE *fp, char *arg)
{
	int level = atoi(arg);

	debug(LOG_DEBUG, "Entering ndsctl_loglevel [%s]", arg);

	LOCK_CONFIG();

	if (!set_log_level(level)) {
		fprintf(fp, "Yes");
		debug(LOG_NOTICE, "Set debug loglevel to %d.", level);
	} else {
		fprintf(fp, "No");
	}

	UNLOCK_CONFIG();

	debug(LOG_DEBUG, "Exiting ndsctl_loglevel.");
}

static int
socket_set_non_blocking(int sockfd)
{
	int rc;

	rc = fcntl(sockfd, F_GETFL, 0);

	if (rc) {
		rc |= O_NONBLOCK;
		rc = fcntl(sockfd, F_SETFL, rc);
	}

	return rc;
}
