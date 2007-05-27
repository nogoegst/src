/*	$OpenBSD: relayd.c,v 1.23 2007/05/27 20:53:10 pyr Exp $	*/

/*
 * Copyright (c) 2006 Pierre-Yves Ritschard <pyr@spootnik.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <net/if.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>

#include <openssl/ssl.h>

#include "hoststated.h"

__dead void	 usage(void);

void		 main_sig_handler(int, short, void *);
void		 main_shutdown(struct hoststated *);
void		 main_dispatch_pfe(int, short, void *);
void		 main_dispatch_hce(int, short, void *);
void		 main_dispatch_relay(int, short, void *);
int		 check_child(pid_t, const char *);
int		 send_all(struct hoststated *, enum imsg_type,
		    void *, u_int16_t);

int		 pipe_parent2pfe[2];
int		 pipe_parent2hce[2];
int		 pipe_pfe2hce[2];
int		 pipe_parent2relay[RELAY_MAXPROC][2];
int		 pipe_pfe2relay[RELAY_MAXPROC][2];

struct imsgbuf	*ibuf_pfe;
struct imsgbuf	*ibuf_hce;
struct imsgbuf	*ibuf_relay;

pid_t		 pfe_pid = 0;
pid_t		 hce_pid = 0;
pid_t		 relay_pid = 0;

void
main_sig_handler(int sig, short event, void *arg)
{
	struct hoststated	*env = arg;
	int			 die = 0;

	log_debug("signal %d", sig);

	switch (sig) {
	case SIGTERM:
	case SIGINT:
		die = 1;
	case SIGCHLD:
		if (check_child(pfe_pid, "pf udpate engine")) {
			pfe_pid = 0;
			die  = 1;
		}
		if (check_child(hce_pid, "host check engine")) {
			hce_pid = 0;
			die  = 1;
		}
		if (check_child(relay_pid, "socket relay engine")) {
			relay_pid = 0;
			die  = 1;
		}
		if (die)
			main_shutdown(env);
		break;
	case SIGHUP:
		/* reconfigure */
		break;
	default:
		fatalx("unexpected signal");
	}
}

/* __dead is for lint */
__dead void
usage(void)
{
	extern char	*__progname;

	fprintf(stderr, "%s [-dnv] [-f file]\n", __progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	int			 c;
	int			 debug;
	u_int32_t		 opts;
	struct hoststated	*env;
	const char		*conffile;
	struct event		 ev_sigint;
	struct event		 ev_sigterm;
	struct event		 ev_sigchld;
	struct event		 ev_sighup;
	struct imsgbuf		*ibuf;

	opts = 0;
	debug = 0;
	conffile = CONF_FILE;

	for (;(c = getopt(argc, argv, "dD:nf:v")) != -1;) {
		switch (c) {
		case 'd':
			debug = 1;
			break;
		case 'D':
			if (cmdline_symset(optarg) < 0)
				log_warnx("could not parse macro definition %s",
				    optarg);
			break;
		case 'n':
			opts |= HOSTSTATED_OPT_NOACTION;
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'v':
			opts |= HOSTSTATED_OPT_VERBOSE;
			break;
		default:
			usage();
		}
	}

	log_init(debug);

	if ((env = parse_config(conffile, opts)) == NULL)
		exit(1);

	if (env->opts & HOSTSTATED_OPT_NOACTION) {
		fprintf(stderr, "configuration OK\n");
		exit(0);
	}
	if (debug)
		env->opts |= HOSTSTATED_OPT_LOGUPDATE;

	if (geteuid())
		errx(1, "need root privileges");

	if (getpwnam(HOSTSTATED_USER) == NULL)
		errx(1, "unknown user %s", HOSTSTATED_USER);

	if (!debug) {
		if (daemon(1, 0) == -1)
			err(1, "failed to daemonize");
	}

	log_info("startup");

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC,
	    pipe_parent2pfe) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC,
	    pipe_parent2hce) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC,
	    pipe_pfe2hce) == -1)
		fatal("socketpair");
	for (c = 0; c < env->prefork_relay; c++) {
		if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC,
		    pipe_parent2relay[c]) == -1)
			fatal("socketpair");
		if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC,
		    pipe_pfe2relay[c]) == -1)
			fatal("socketpair");
		session_socket_blockmode(pipe_pfe2relay[c][0], BM_NONBLOCK);
		session_socket_blockmode(pipe_pfe2relay[c][1], BM_NONBLOCK);
		session_socket_blockmode(pipe_parent2relay[c][0], BM_NONBLOCK);
		session_socket_blockmode(pipe_parent2relay[c][1], BM_NONBLOCK);
	}

	session_socket_blockmode(pipe_parent2pfe[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_parent2pfe[1], BM_NONBLOCK);
	session_socket_blockmode(pipe_parent2hce[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_parent2hce[1], BM_NONBLOCK);
	session_socket_blockmode(pipe_pfe2hce[0], BM_NONBLOCK);
	session_socket_blockmode(pipe_pfe2hce[1], BM_NONBLOCK);

	pfe_pid = pfe(env, pipe_parent2pfe, pipe_parent2hce,
	    pipe_parent2relay, pipe_pfe2hce, pipe_pfe2relay);
	hce_pid = hce(env, pipe_parent2pfe, pipe_parent2hce,
	    pipe_parent2relay, pipe_pfe2hce, pipe_pfe2relay);
	relay_pid = relay(env, pipe_parent2pfe, pipe_parent2hce,
	    pipe_parent2relay, pipe_pfe2hce, pipe_pfe2relay);

	setproctitle("parent");

	event_init();

	signal_set(&ev_sigint, SIGINT, main_sig_handler, env);
	signal_set(&ev_sigterm, SIGTERM, main_sig_handler, env);
	signal_set(&ev_sigchld, SIGCHLD, main_sig_handler, env);
	signal_set(&ev_sighup, SIGHUP, main_sig_handler, env);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal_add(&ev_sigchld, NULL);
	signal_add(&ev_sighup, NULL);
	signal(SIGPIPE, SIG_IGN);

	close(pipe_parent2pfe[1]);
	close(pipe_parent2hce[1]);
	close(pipe_pfe2hce[0]);
	close(pipe_pfe2hce[1]);
	for (c = 0; c < env->prefork_relay; c++) {
		close(pipe_pfe2relay[c][0]);
		close(pipe_pfe2relay[c][1]);
		close(pipe_parent2relay[c][0]);
	}

	if ((ibuf_pfe = calloc(1, sizeof(struct imsgbuf))) == NULL ||
	    (ibuf_hce = calloc(1, sizeof(struct imsgbuf))) == NULL ||
	    (ibuf_relay = calloc(env->prefork_relay,
	    sizeof(struct imsgbuf))) == NULL)
		fatal(NULL);

	imsg_init(ibuf_pfe, pipe_parent2pfe[0], main_dispatch_pfe);
	imsg_init(ibuf_hce, pipe_parent2hce[0], main_dispatch_hce);
	for (c = 0; c < env->prefork_relay; c++) {
		ibuf = &ibuf_relay[c];
		imsg_init(ibuf, pipe_parent2relay[c][1], main_dispatch_relay);
		ibuf->events = EV_READ;
		event_set(&ibuf->ev, ibuf->fd, ibuf->events,
		    ibuf->handler, ibuf);
		event_add(&ibuf->ev, NULL);
	}

	ibuf_pfe->events = EV_READ;
	event_set(&ibuf_pfe->ev, ibuf_pfe->fd, ibuf_pfe->events,
	    ibuf_pfe->handler, ibuf_pfe);
	event_add(&ibuf_pfe->ev, NULL);

	ibuf_hce->events = EV_READ;
	event_set(&ibuf_hce->ev, ibuf_hce->fd, ibuf_hce->events,
	    ibuf_hce->handler, ibuf_hce);
	event_add(&ibuf_hce->ev, NULL);

	if (env->flags & F_DEMOTE)
		carp_demote_reset(env->demote_group, 0);

	event_dispatch();

	return (0);
}

void
main_shutdown(struct hoststated *env)
{
	pid_t	pid;

	if (pfe_pid)
		kill(pfe_pid, SIGTERM);
	if (hce_pid)
		kill(hce_pid, SIGTERM);
	if (relay_pid)
		kill(relay_pid, SIGTERM);

	do {
		if ((pid = wait(NULL)) == -1 &&
		    errno != EINTR && errno != ECHILD)
			fatal("wait");
	} while (pid != -1 || (pid == -1 && errno == EINTR));

	control_cleanup();
	carp_demote_shutdown();
	if (env->flags & F_DEMOTE)
		carp_demote_reset(env->demote_group, 128);
	log_info("terminating");
	exit(0);
}

int
check_child(pid_t pid, const char *pname)
{
	int	status;

	if (waitpid(pid, &status, WNOHANG) > 0) {
		if (WIFEXITED(status)) {
			log_warnx("check_child: lost child: %s exited", pname);
			return (1);
		}
		if (WIFSIGNALED(status)) {
			log_warnx("check_child: lost child: %s terminated; "
			    "signal %d", pname, WTERMSIG(status));
			return (1);
		}
	}

	return (0);
}

int
send_all(struct hoststated *env, enum imsg_type type, void *buf, u_int16_t len)
{
	int		 i;

	if (imsg_compose(ibuf_pfe, type, 0, 0, buf, len) == -1)
		return (-1);
	if (imsg_compose(ibuf_hce, type, 0, 0, buf, len) == -1)
		return (-1);
	for (i = 0; i < env->prefork_relay; i++) {
		if (imsg_compose(&ibuf_relay[i], type, 0, 0, buf, len) == -1)
			return (-1);
	}
	return (0);
}

void
imsg_event_add(struct imsgbuf *ibuf)
{
	ibuf->events = EV_READ;
	if (ibuf->w.queued)
		ibuf->events |= EV_WRITE;

	event_del(&ibuf->ev);
	event_set(&ibuf->ev, ibuf->fd, ibuf->events, ibuf->handler, ibuf);
	event_add(&ibuf->ev, NULL);
}

void
main_dispatch_pfe(int fd, short event, void *ptr)
{
	struct imsgbuf		*ibuf;
	struct imsg		 imsg;
	ssize_t			 n;
	struct ctl_demote	 demote;

	ibuf = ptr;
	switch (event) {
	case EV_READ:
		if ((n = imsg_read(ibuf)) == -1)
			fatal("imsg_read_error");
		if (n == 0)
			fatalx("main_dispatch_pfe: pipe closed");
		break;
	case EV_WRITE:
		if (msgbuf_write(&ibuf->w) == -1)
			fatal("msgbuf_write");
		imsg_event_add(ibuf);
		return;
	default:
		fatalx("unknown event");
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("main_dispatch_pfe: imsg_read error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_DEMOTE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(demote))
				fatalx("main_dispatch_pfe: "
				    "invalid size of demote request");
			memcpy(&demote, imsg.data, sizeof(demote));
			carp_demote_set(demote.group, demote.level);
			break;
		default:
			log_debug("main_dispatch_pfe: unexpected imsg %d",
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	imsg_event_add(ibuf);
}

void
main_dispatch_hce(int fd, short event, void * ptr)
{
	struct imsgbuf		*ibuf;
	struct imsg		 imsg;
	ssize_t			 n;

	ibuf = ptr;
	switch (event) {
	case EV_READ:
		if ((n = imsg_read(ibuf)) == -1)
			fatal("imsg_read error");
		if (n == 0)
			fatalx("main_dispatch_hce: pipe closed");
		break;
	case EV_WRITE:
		if (msgbuf_write(&ibuf->w) == -1)
			fatal("msgbuf_write");
		imsg_event_add(ibuf);
		return;
	default:
		fatalx("unknown event");
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("main_dispatch_hce: imsg_read error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		default:
			log_debug("main_dispatch_hce: unexpected imsg %d",
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
}

void
main_dispatch_relay(int fd, short event, void * ptr)
{
	struct imsgbuf		*ibuf;
	struct imsg		 imsg;
	ssize_t			 n;

	ibuf = ptr;
	switch (event) {
	case EV_READ:
		if ((n = imsg_read(ibuf)) == -1)
			fatal("imsg_read error");
		if (n == 0)
			fatalx("main_dispatch_relay: pipe closed");
		break;
	case EV_WRITE:
		if (msgbuf_write(&ibuf->w) == -1)
			fatal("msgbuf_write");
		imsg_event_add(ibuf);
		return;
	default:
		fatalx("unknown event");
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("main_dispatch_relay: imsg_read error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		default:
			log_debug("main_dispatch_relay: unexpected imsg %d",
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	imsg_event_add(ibuf);
}

struct host *
host_find(struct hoststated *env, objid_t id)
{
	struct table	*table;
	struct host	*host;

	TAILQ_FOREACH(table, &env->tables, entry)
		TAILQ_FOREACH(host, &table->hosts, entry)
			if (host->conf.id == id)
				return (host);
	return (NULL);
}

struct table *
table_find(struct hoststated *env, objid_t id)
{
	struct table	*table;

	TAILQ_FOREACH(table, &env->tables, entry)
		if (table->conf.id == id)
			return (table);
	return (NULL);
}

struct service *
service_find(struct hoststated *env, objid_t id)
{
	struct service	*service;

	TAILQ_FOREACH(service, &env->services, entry)
		if (service->conf.id == id)
			return (service);
	return (NULL);
}

struct relay *
relay_find(struct hoststated *env, objid_t id)
{
	struct relay	*rlay;

	TAILQ_FOREACH(rlay, &env->relays, entry)
		if (rlay->id == id)
			return (rlay);
	return (NULL);
}

struct session *
session_find(struct hoststated *env, objid_t id)
{
	struct relay		*rlay;
	struct session		*con;

	TAILQ_FOREACH(rlay, &env->relays, entry)
		TAILQ_FOREACH(con, &rlay->sessions, entry)
			if (con->id == id)
				return (con);
	return (NULL);
}

struct host *
host_findbyname(struct hoststated *env, const char *name)
{
	struct table	*table;
	struct host	*host;

	TAILQ_FOREACH(table, &env->tables, entry)
		TAILQ_FOREACH(host, &table->hosts, entry)
			if (strcmp(host->conf.name, name) == 0)
				return (host);
	return (NULL);
}

struct table *
table_findbyname(struct hoststated *env, const char *name)
{
	struct table	*table;

	TAILQ_FOREACH(table, &env->tables, entry)
		if (strcmp(table->conf.name, name) == 0)
			return (table);
	return (NULL);
}

struct service *
service_findbyname(struct hoststated *env, const char *name)
{
	struct service	*service;

	TAILQ_FOREACH(service, &env->services, entry)
		if (strcmp(service->conf.name, name) == 0)
			return (service);
	return (NULL);
}

struct relay *
relay_findbyname(struct hoststated *env, const char *name)
{
	struct relay	*rlay;

	TAILQ_FOREACH(rlay, &env->relays, entry)
		if (strcmp(rlay->name, name) == 0)
			return (rlay);
	return (NULL);
}

void
event_again(struct event *ev, int fd, short event,
    void (*fn)(int, short, void *),
    struct timeval *start, struct timeval *end, void *arg)
{
	struct timeval tv_next, tv_now, tv;

	if (gettimeofday(&tv_now, NULL))
		fatal("event_again: gettimeofday");

	bcopy(end, &tv_next, sizeof(tv_next));
	timersub(&tv_now, start, &tv_now);
	timersub(&tv_next, &tv_now, &tv_next);

	bzero(&tv, sizeof(tv));
	if (timercmp(&tv_next, &tv, >))
		bcopy(&tv_next, &tv, sizeof(tv));

	event_set(ev, fd, event, fn, arg);
	event_add(ev, &tv);
}

int
expand_string(char *label, size_t len, const char *srch, const char *repl)
{
	char *tmp;
	char *p, *q;

	if ((tmp = calloc(1, len)) == NULL) {
		log_debug("expand_string: calloc");
		return (-1);
	}
	p = q = label;
	while ((q = strstr(p, srch)) != NULL) {
		*q = '\0';
		if ((strlcat(tmp, p, len) >= len) ||
		    (strlcat(tmp, repl, len) >= len)) {
			log_debug("expand_string: string too long");
			return (-1);
		}
		q += strlen(srch);
		p = q;
	}
	if (strlcat(tmp, p, len) >= len) {
		log_debug("expand_string: string too long");
		return (-1);
	}
	(void)strlcpy(label, tmp, len);	/* always fits */
	free(tmp);

	return (0);
}
