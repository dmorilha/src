/*	$OpenBSD: session.c,v 1.466 2024/03/22 15:41:34 claudio Exp $ */

/*
 * Copyright (c) 2003, 2004, 2005 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 2017 Peter van Dijk <peter.van.dijk@powerdns.com>
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

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <limits.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "bgpd.h"
#include "session.h"
#include "log.h"

#define PFD_PIPE_MAIN		0
#define PFD_PIPE_ROUTE		1
#define PFD_PIPE_ROUTE_CTL	2
#define PFD_SOCK_CTL		3
#define PFD_SOCK_RCTL		4
#define PFD_LISTENERS_START	5

void	session_sighdlr(int);
int	setup_listeners(u_int *);
void	init_peer(struct peer *);
void	start_timer_holdtime(struct peer *);
void	start_timer_keepalive(struct peer *);
void	session_close_connection(struct peer *);
void	change_state(struct peer *, enum session_state, enum session_events);
int	session_setup_socket(struct peer *);
void	session_accept(int);
int	session_connect(struct peer *);
void	session_tcp_established(struct peer *);
void	session_capa_ann_none(struct peer *);
int	session_capa_add(struct ibuf *, uint8_t, uint8_t);
int	session_capa_add_mp(struct ibuf *, uint8_t);
int	session_capa_add_afi(struct ibuf *, uint8_t, uint8_t);
struct bgp_msg	*session_newmsg(enum msg_type, uint16_t);
int	session_sendmsg(struct bgp_msg *, struct peer *);
void	session_open(struct peer *);
void	session_keepalive(struct peer *);
void	session_update(uint32_t, struct ibuf *);
void	session_notification(struct peer *, uint8_t, uint8_t, struct ibuf *);
void	session_notification_data(struct peer *, uint8_t, uint8_t, void *,
	    size_t);
void	session_rrefresh(struct peer *, uint8_t, uint8_t);
int	session_graceful_restart(struct peer *);
int	session_graceful_stop(struct peer *);
int	session_dispatch_msg(struct pollfd *, struct peer *);
void	session_process_msg(struct peer *);
int	parse_header(struct peer *, u_char *, uint16_t *, uint8_t *);
int	parse_open(struct peer *);
int	parse_update(struct peer *);
int	parse_rrefresh(struct peer *);
int	parse_notification(struct peer *);
int	parse_capabilities(struct peer *, u_char *, uint16_t, uint32_t *);
int	capa_neg_calc(struct peer *, uint8_t *);
void	session_dispatch_imsg(struct imsgbuf *, int, u_int *);
void	session_up(struct peer *);
void	session_down(struct peer *);
int	imsg_rde(int, uint32_t, void *, uint16_t);
void	session_demote(struct peer *, int);
void	merge_peers(struct bgpd_config *, struct bgpd_config *);

int		 la_cmp(struct listen_addr *, struct listen_addr *);
void		 session_template_clone(struct peer *, struct sockaddr *,
		    uint32_t, uint32_t);
int		 session_match_mask(struct peer *, struct bgpd_addr *);

static struct bgpd_config	*conf, *nconf;
static struct imsgbuf		*ibuf_rde;
static struct imsgbuf		*ibuf_rde_ctl;
static struct imsgbuf		*ibuf_main;

struct bgpd_sysdep	 sysdep;
volatile sig_atomic_t	 session_quit;
int			 pending_reconf;
int			 csock = -1, rcsock = -1;
u_int			 peer_cnt;

struct mrt_head		 mrthead;
time_t			 pauseaccept;

static inline int
peer_compare(const struct peer *a, const struct peer *b)
{
	return a->conf.id - b->conf.id;
}

RB_GENERATE(peer_head, peer, entry, peer_compare);

void
session_sighdlr(int sig)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		session_quit = 1;
		break;
	}
}

int
setup_listeners(u_int *la_cnt)
{
	int			 ttl = 255;
	struct listen_addr	*la;
	u_int			 cnt = 0;

	TAILQ_FOREACH(la, conf->listen_addrs, entry) {
		la->reconf = RECONF_NONE;
		cnt++;

		if (la->flags & LISTENER_LISTENING)
			continue;

		if (la->fd == -1) {
			log_warn("cannot establish listener on %s: invalid fd",
			    log_sockaddr((struct sockaddr *)&la->sa,
			    la->sa_len));
			continue;
		}

		if (tcp_md5_prep_listener(la, &conf->peers) == -1)
			fatal("tcp_md5_prep_listener");

		/* set ttl to 255 so that ttl-security works */
		if (la->sa.ss_family == AF_INET && setsockopt(la->fd,
		    IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) == -1) {
			log_warn("setup_listeners setsockopt TTL");
			continue;
		}
		if (la->sa.ss_family == AF_INET6 && setsockopt(la->fd,
		    IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl)) == -1) {
			log_warn("setup_listeners setsockopt hoplimit");
			continue;
		}

		if (listen(la->fd, MAX_BACKLOG)) {
			close(la->fd);
			fatal("listen");
		}

		la->flags |= LISTENER_LISTENING;

		log_info("listening on %s",
		    log_sockaddr((struct sockaddr *)&la->sa, la->sa_len));
	}

	*la_cnt = cnt;

	return (0);
}

void
session_main(int debug, int verbose)
{
	int			 timeout;
	unsigned int		 i, j, idx_peers, idx_listeners, idx_mrts;
	u_int			 pfd_elms = 0, peer_l_elms = 0, mrt_l_elms = 0;
	u_int			 listener_cnt, ctl_cnt, mrt_cnt;
	u_int			 new_cnt;
	struct passwd		*pw;
	struct peer		*p, **peer_l = NULL, *next;
	struct mrt		*m, *xm, **mrt_l = NULL;
	struct pollfd		*pfd = NULL;
	struct listen_addr	*la;
	void			*newp;
	time_t			 now;
	short			 events;

	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);

	log_procinit(log_procnames[PROC_SE]);

	if ((pw = getpwnam(BGPD_USER)) == NULL)
		fatal(NULL);

	if (chroot(pw->pw_dir) == -1)
		fatal("chroot");
	if (chdir("/") == -1)
		fatal("chdir(\"/\")");

	setproctitle("session engine");

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("can't drop privileges");

	if (pledge("stdio inet recvfd", NULL) == -1)
		fatal("pledge");

	signal(SIGTERM, session_sighdlr);
	signal(SIGINT, session_sighdlr);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGALRM, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);

	if ((ibuf_main = malloc(sizeof(struct imsgbuf))) == NULL)
		fatal(NULL);
	imsg_init(ibuf_main, 3);

	LIST_INIT(&mrthead);
	listener_cnt = 0;
	peer_cnt = 0;
	ctl_cnt = 0;

	conf = new_config();
	log_info("session engine ready");

	while (session_quit == 0) {
		/* check for peers to be initialized or deleted */
		if (!pending_reconf) {
			RB_FOREACH_SAFE(p, peer_head, &conf->peers, next) {
				/* cloned peer that idled out? */
				if (p->template && (p->state == STATE_IDLE ||
				    p->state == STATE_ACTIVE) &&
				    getmonotime() - p->stats.last_updown >=
				    INTERVAL_HOLD_CLONED)
					p->reconf_action = RECONF_DELETE;

				/* new peer that needs init? */
				if (p->state == STATE_NONE)
					init_peer(p);

				/* deletion due? */
				if (p->reconf_action == RECONF_DELETE) {
					if (p->demoted)
						session_demote(p, -1);
					p->conf.demote_group[0] = 0;
					session_stop(p, ERR_CEASE_PEER_UNCONF);
					timer_remove_all(&p->timers);
					tcp_md5_del_listener(conf, p);
					log_peer_warnx(&p->conf, "removed");
					RB_REMOVE(peer_head, &conf->peers, p);
					free(p);
					peer_cnt--;
					continue;
				}
				p->reconf_action = RECONF_NONE;
			}
		}

		if (peer_cnt > peer_l_elms) {
			if ((newp = reallocarray(peer_l, peer_cnt,
			    sizeof(struct peer *))) == NULL) {
				/* panic for now */
				log_warn("could not resize peer_l from %u -> %u"
				    " entries", peer_l_elms, peer_cnt);
				fatalx("exiting");
			}
			peer_l = newp;
			peer_l_elms = peer_cnt;
		}

		mrt_cnt = 0;
		for (m = LIST_FIRST(&mrthead); m != NULL; m = xm) {
			xm = LIST_NEXT(m, entry);
			if (m->state == MRT_STATE_REMOVE) {
				mrt_clean(m);
				LIST_REMOVE(m, entry);
				free(m);
				continue;
			}
			if (m->wbuf.queued)
				mrt_cnt++;
		}

		if (mrt_cnt > mrt_l_elms) {
			if ((newp = reallocarray(mrt_l, mrt_cnt,
			    sizeof(struct mrt *))) == NULL) {
				/* panic for now */
				log_warn("could not resize mrt_l from %u -> %u"
				    " entries", mrt_l_elms, mrt_cnt);
				fatalx("exiting");
			}
			mrt_l = newp;
			mrt_l_elms = mrt_cnt;
		}

		new_cnt = PFD_LISTENERS_START + listener_cnt + peer_cnt +
		    ctl_cnt + mrt_cnt;
		if (new_cnt > pfd_elms) {
			if ((newp = reallocarray(pfd, new_cnt,
			    sizeof(struct pollfd))) == NULL) {
				/* panic for now */
				log_warn("could not resize pfd from %u -> %u"
				    " entries", pfd_elms, new_cnt);
				fatalx("exiting");
			}
			pfd = newp;
			pfd_elms = new_cnt;
		}

		memset(pfd, 0, sizeof(struct pollfd) * pfd_elms);

		set_pollfd(&pfd[PFD_PIPE_MAIN], ibuf_main);
		set_pollfd(&pfd[PFD_PIPE_ROUTE], ibuf_rde);
		set_pollfd(&pfd[PFD_PIPE_ROUTE_CTL], ibuf_rde_ctl);

		if (pauseaccept == 0) {
			pfd[PFD_SOCK_CTL].fd = csock;
			pfd[PFD_SOCK_CTL].events = POLLIN;
			pfd[PFD_SOCK_RCTL].fd = rcsock;
			pfd[PFD_SOCK_RCTL].events = POLLIN;
		} else {
			pfd[PFD_SOCK_CTL].fd = -1;
			pfd[PFD_SOCK_RCTL].fd = -1;
		}

		i = PFD_LISTENERS_START;
		TAILQ_FOREACH(la, conf->listen_addrs, entry) {
			if (pauseaccept == 0) {
				pfd[i].fd = la->fd;
				pfd[i].events = POLLIN;
			} else
				pfd[i].fd = -1;
			i++;
		}
		idx_listeners = i;
		timeout = 240;	/* loop every 240s at least */

		now = getmonotime();
		RB_FOREACH(p, peer_head, &conf->peers) {
			time_t	nextaction;
			struct timer *pt;

			/* check timers */
			if ((pt = timer_nextisdue(&p->timers, now)) != NULL) {
				switch (pt->type) {
				case Timer_Hold:
					bgp_fsm(p, EVNT_TIMER_HOLDTIME);
					break;
				case Timer_SendHold:
					bgp_fsm(p, EVNT_TIMER_SENDHOLD);
					break;
				case Timer_ConnectRetry:
					bgp_fsm(p, EVNT_TIMER_CONNRETRY);
					break;
				case Timer_Keepalive:
					bgp_fsm(p, EVNT_TIMER_KEEPALIVE);
					break;
				case Timer_IdleHold:
					bgp_fsm(p, EVNT_START);
					break;
				case Timer_IdleHoldReset:
					p->IdleHoldTime =
					    INTERVAL_IDLE_HOLD_INITIAL;
					p->errcnt = 0;
					timer_stop(&p->timers,
					    Timer_IdleHoldReset);
					break;
				case Timer_CarpUndemote:
					timer_stop(&p->timers,
					    Timer_CarpUndemote);
					if (p->demoted &&
					    p->state == STATE_ESTABLISHED)
						session_demote(p, -1);
					break;
				case Timer_RestartTimeout:
					timer_stop(&p->timers,
					    Timer_RestartTimeout);
					session_graceful_stop(p);
					break;
				default:
					fatalx("King Bula lost in time");
				}
			}
			if ((nextaction = timer_nextduein(&p->timers,
			    now)) != -1 && nextaction < timeout)
				timeout = nextaction;

			/* are we waiting for a write? */
			events = POLLIN;
			if (p->wbuf.queued > 0 || p->state == STATE_CONNECT)
				events |= POLLOUT;
			/* is there still work to do? */
			if (p->rpending && p->rbuf && p->rbuf->wpos)
				timeout = 0;

			/* poll events */
			if (p->fd != -1 && events != 0) {
				pfd[i].fd = p->fd;
				pfd[i].events = events;
				peer_l[i - idx_listeners] = p;
				i++;
			}
		}

		idx_peers = i;

		LIST_FOREACH(m, &mrthead, entry)
			if (m->wbuf.queued) {
				pfd[i].fd = m->wbuf.fd;
				pfd[i].events = POLLOUT;
				mrt_l[i - idx_peers] = m;
				i++;
			}

		idx_mrts = i;

		i += control_fill_pfds(pfd + i, pfd_elms -i);

		if (i > pfd_elms)
			fatalx("poll pfd overflow");

		if (pauseaccept && timeout > 1)
			timeout = 1;
		if (timeout < 0)
			timeout = 0;
		if (poll(pfd, i, timeout * 1000) == -1) {
			if (errno == EINTR)
				continue;
			fatal("poll error");
		}

		/*
		 * If we previously saw fd exhaustion, we stop accept()
		 * for 1 second to throttle the accept() loop.
		 */
		if (pauseaccept && getmonotime() > pauseaccept + 1)
			pauseaccept = 0;

		if (handle_pollfd(&pfd[PFD_PIPE_MAIN], ibuf_main) == -1) {
			log_warnx("SE: Lost connection to parent");
			session_quit = 1;
			continue;
		} else
			session_dispatch_imsg(ibuf_main, PFD_PIPE_MAIN,
			    &listener_cnt);

		if (handle_pollfd(&pfd[PFD_PIPE_ROUTE], ibuf_rde) == -1) {
			log_warnx("SE: Lost connection to RDE");
			msgbuf_clear(&ibuf_rde->w);
			free(ibuf_rde);
			ibuf_rde = NULL;
		} else
			session_dispatch_imsg(ibuf_rde, PFD_PIPE_ROUTE,
			    &listener_cnt);

		if (handle_pollfd(&pfd[PFD_PIPE_ROUTE_CTL], ibuf_rde_ctl) ==
		    -1) {
			log_warnx("SE: Lost connection to RDE control");
			msgbuf_clear(&ibuf_rde_ctl->w);
			free(ibuf_rde_ctl);
			ibuf_rde_ctl = NULL;
		} else
			session_dispatch_imsg(ibuf_rde_ctl, PFD_PIPE_ROUTE_CTL,
			    &listener_cnt);

		if (pfd[PFD_SOCK_CTL].revents & POLLIN)
			ctl_cnt += control_accept(csock, 0);

		if (pfd[PFD_SOCK_RCTL].revents & POLLIN)
			ctl_cnt += control_accept(rcsock, 1);

		for (j = PFD_LISTENERS_START; j < idx_listeners; j++)
			if (pfd[j].revents & POLLIN)
				session_accept(pfd[j].fd);

		for (; j < idx_peers; j++)
			session_dispatch_msg(&pfd[j],
			    peer_l[j - idx_listeners]);

		RB_FOREACH(p, peer_head, &conf->peers)
			if (p->rbuf && p->rbuf->wpos)
				session_process_msg(p);

		for (; j < idx_mrts; j++)
			if (pfd[j].revents & POLLOUT)
				mrt_write(mrt_l[j - idx_peers]);

		for (; j < i; j++)
			ctl_cnt -= control_dispatch_msg(&pfd[j], &conf->peers);
	}

	RB_FOREACH_SAFE(p, peer_head, &conf->peers, next) {
		RB_REMOVE(peer_head, &conf->peers, p);
		strlcpy(p->conf.reason,
		    "bgpd shutting down",
		    sizeof(p->conf.reason));
		session_stop(p, ERR_CEASE_ADMIN_DOWN);
		timer_remove_all(&p->timers);
		free(p);
	}

	while ((m = LIST_FIRST(&mrthead)) != NULL) {
		mrt_clean(m);
		LIST_REMOVE(m, entry);
		free(m);
	}

	free_config(conf);
	free(peer_l);
	free(mrt_l);
	free(pfd);

	/* close pipes */
	if (ibuf_rde) {
		msgbuf_write(&ibuf_rde->w);
		msgbuf_clear(&ibuf_rde->w);
		close(ibuf_rde->fd);
		free(ibuf_rde);
	}
	if (ibuf_rde_ctl) {
		msgbuf_clear(&ibuf_rde_ctl->w);
		close(ibuf_rde_ctl->fd);
		free(ibuf_rde_ctl);
	}
	msgbuf_write(&ibuf_main->w);
	msgbuf_clear(&ibuf_main->w);
	close(ibuf_main->fd);
	free(ibuf_main);

	control_shutdown(csock);
	control_shutdown(rcsock);
	log_info("session engine exiting");
	exit(0);
}

void
init_peer(struct peer *p)
{
	TAILQ_INIT(&p->timers);
	p->fd = p->wbuf.fd = -1;

	if (p->conf.if_depend[0])
		imsg_compose(ibuf_main, IMSG_SESSION_DEPENDON, 0, 0, -1,
		    p->conf.if_depend, sizeof(p->conf.if_depend));
	else
		p->depend_ok = 1;

	peer_cnt++;

	change_state(p, STATE_IDLE, EVNT_NONE);
	if (p->conf.down)
		timer_stop(&p->timers, Timer_IdleHold); /* no autostart */
	else
		timer_set(&p->timers, Timer_IdleHold, SESSION_CLEAR_DELAY);

	p->stats.last_updown = getmonotime();

	/*
	 * on startup, demote if requested.
	 * do not handle new peers. they must reach ESTABLISHED beforehand.
	 * peers added at runtime have reconf_action set to RECONF_REINIT.
	 */
	if (p->reconf_action != RECONF_REINIT && p->conf.demote_group[0])
		session_demote(p, +1);
}

void
bgp_fsm(struct peer *peer, enum session_events event)
{
	switch (peer->state) {
	case STATE_NONE:
		/* nothing */
		break;
	case STATE_IDLE:
		switch (event) {
		case EVNT_START:
			timer_stop(&peer->timers, Timer_Hold);
			timer_stop(&peer->timers, Timer_SendHold);
			timer_stop(&peer->timers, Timer_Keepalive);
			timer_stop(&peer->timers, Timer_IdleHold);

			/* allocate read buffer */
			peer->rbuf = calloc(1, sizeof(struct ibuf_read));
			if (peer->rbuf == NULL)
				fatal(NULL);

			/* init write buffer */
			msgbuf_init(&peer->wbuf);

			if (!peer->depend_ok)
				timer_stop(&peer->timers, Timer_ConnectRetry);
			else if (peer->passive || peer->conf.passive ||
			    peer->conf.template) {
				change_state(peer, STATE_ACTIVE, event);
				timer_stop(&peer->timers, Timer_ConnectRetry);
			} else {
				change_state(peer, STATE_CONNECT, event);
				timer_set(&peer->timers, Timer_ConnectRetry,
				    conf->connectretry);
				session_connect(peer);
			}
			peer->passive = 0;
			break;
		default:
			/* ignore */
			break;
		}
		break;
	case STATE_CONNECT:
		switch (event) {
		case EVNT_START:
			/* ignore */
			break;
		case EVNT_CON_OPEN:
			session_tcp_established(peer);
			session_open(peer);
			timer_stop(&peer->timers, Timer_ConnectRetry);
			peer->holdtime = INTERVAL_HOLD_INITIAL;
			start_timer_holdtime(peer);
			change_state(peer, STATE_OPENSENT, event);
			break;
		case EVNT_CON_OPENFAIL:
			timer_set(&peer->timers, Timer_ConnectRetry,
			    conf->connectretry);
			session_close_connection(peer);
			change_state(peer, STATE_ACTIVE, event);
			break;
		case EVNT_TIMER_CONNRETRY:
			timer_set(&peer->timers, Timer_ConnectRetry,
			    conf->connectretry);
			session_connect(peer);
			break;
		default:
			change_state(peer, STATE_IDLE, event);
			break;
		}
		break;
	case STATE_ACTIVE:
		switch (event) {
		case EVNT_START:
			/* ignore */
			break;
		case EVNT_CON_OPEN:
			session_tcp_established(peer);
			session_open(peer);
			timer_stop(&peer->timers, Timer_ConnectRetry);
			peer->holdtime = INTERVAL_HOLD_INITIAL;
			start_timer_holdtime(peer);
			change_state(peer, STATE_OPENSENT, event);
			break;
		case EVNT_CON_OPENFAIL:
			timer_set(&peer->timers, Timer_ConnectRetry,
			    conf->connectretry);
			session_close_connection(peer);
			change_state(peer, STATE_ACTIVE, event);
			break;
		case EVNT_TIMER_CONNRETRY:
			timer_set(&peer->timers, Timer_ConnectRetry,
			    peer->holdtime);
			change_state(peer, STATE_CONNECT, event);
			session_connect(peer);
			break;
		default:
			change_state(peer, STATE_IDLE, event);
			break;
		}
		break;
	case STATE_OPENSENT:
		switch (event) {
		case EVNT_START:
			/* ignore */
			break;
		case EVNT_STOP:
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_CON_CLOSED:
			session_close_connection(peer);
			timer_set(&peer->timers, Timer_ConnectRetry,
			    conf->connectretry);
			change_state(peer, STATE_ACTIVE, event);
			break;
		case EVNT_CON_FATAL:
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_TIMER_HOLDTIME:
			session_notification(peer, ERR_HOLDTIMEREXPIRED,
			    0, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_TIMER_SENDHOLD:
			session_notification(peer, ERR_SENDHOLDTIMEREXPIRED,
			    0, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_RCVD_OPEN:
			/* parse_open calls change_state itself on failure */
			if (parse_open(peer))
				break;
			session_keepalive(peer);
			change_state(peer, STATE_OPENCONFIRM, event);
			break;
		case EVNT_RCVD_NOTIFICATION:
			if (parse_notification(peer)) {
				change_state(peer, STATE_IDLE, event);
				/* don't punish, capa negotiation */
				timer_set(&peer->timers, Timer_IdleHold, 0);
				peer->IdleHoldTime /= 2;
			} else
				change_state(peer, STATE_IDLE, event);
			break;
		default:
			session_notification(peer,
			    ERR_FSM, ERR_FSM_UNEX_OPENSENT, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		}
		break;
	case STATE_OPENCONFIRM:
		switch (event) {
		case EVNT_START:
			/* ignore */
			break;
		case EVNT_STOP:
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_CON_CLOSED:
		case EVNT_CON_FATAL:
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_TIMER_HOLDTIME:
			session_notification(peer, ERR_HOLDTIMEREXPIRED,
			    0, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_TIMER_SENDHOLD:
			session_notification(peer, ERR_SENDHOLDTIMEREXPIRED,
			    0, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_TIMER_KEEPALIVE:
			session_keepalive(peer);
			break;
		case EVNT_RCVD_KEEPALIVE:
			start_timer_holdtime(peer);
			change_state(peer, STATE_ESTABLISHED, event);
			break;
		case EVNT_RCVD_NOTIFICATION:
			parse_notification(peer);
			change_state(peer, STATE_IDLE, event);
			break;
		default:
			session_notification(peer,
			    ERR_FSM, ERR_FSM_UNEX_OPENCONFIRM, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		}
		break;
	case STATE_ESTABLISHED:
		switch (event) {
		case EVNT_START:
			/* ignore */
			break;
		case EVNT_STOP:
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_CON_CLOSED:
		case EVNT_CON_FATAL:
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_TIMER_HOLDTIME:
			session_notification(peer, ERR_HOLDTIMEREXPIRED,
			    0, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_TIMER_SENDHOLD:
			session_notification(peer, ERR_SENDHOLDTIMEREXPIRED,
			    0, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		case EVNT_TIMER_KEEPALIVE:
			session_keepalive(peer);
			break;
		case EVNT_RCVD_KEEPALIVE:
			start_timer_holdtime(peer);
			break;
		case EVNT_RCVD_UPDATE:
			start_timer_holdtime(peer);
			if (parse_update(peer))
				change_state(peer, STATE_IDLE, event);
			else
				start_timer_holdtime(peer);
			break;
		case EVNT_RCVD_NOTIFICATION:
			parse_notification(peer);
			change_state(peer, STATE_IDLE, event);
			break;
		default:
			session_notification(peer,
			    ERR_FSM, ERR_FSM_UNEX_ESTABLISHED, NULL);
			change_state(peer, STATE_IDLE, event);
			break;
		}
		break;
	}
}

void
start_timer_holdtime(struct peer *peer)
{
	if (peer->holdtime > 0)
		timer_set(&peer->timers, Timer_Hold, peer->holdtime);
	else
		timer_stop(&peer->timers, Timer_Hold);
}

void
start_timer_keepalive(struct peer *peer)
{
	if (peer->holdtime > 0)
		timer_set(&peer->timers, Timer_Keepalive, peer->holdtime / 3);
	else
		timer_stop(&peer->timers, Timer_Keepalive);
}

void
session_close_connection(struct peer *peer)
{
	if (peer->fd != -1) {
		close(peer->fd);
		pauseaccept = 0;
	}
	peer->fd = peer->wbuf.fd = -1;
}

void
change_state(struct peer *peer, enum session_state state,
    enum session_events event)
{
	struct mrt	*mrt;

	switch (state) {
	case STATE_IDLE:
		/* carp demotion first. new peers handled in init_peer */
		if (peer->state == STATE_ESTABLISHED &&
		    peer->conf.demote_group[0] && !peer->demoted)
			session_demote(peer, +1);

		/*
		 * try to write out what's buffered (maybe a notification),
		 * don't bother if it fails
		 */
		if (peer->state >= STATE_OPENSENT && peer->wbuf.queued)
			msgbuf_write(&peer->wbuf);

		/*
		 * we must start the timer for the next EVNT_START
		 * if we are coming here due to an error and the
		 * session was not established successfully before, the
		 * starttimerinterval needs to be exponentially increased
		 */
		if (peer->IdleHoldTime == 0)
			peer->IdleHoldTime = INTERVAL_IDLE_HOLD_INITIAL;
		peer->holdtime = INTERVAL_HOLD_INITIAL;
		timer_stop(&peer->timers, Timer_ConnectRetry);
		timer_stop(&peer->timers, Timer_Keepalive);
		timer_stop(&peer->timers, Timer_Hold);
		timer_stop(&peer->timers, Timer_SendHold);
		timer_stop(&peer->timers, Timer_IdleHold);
		timer_stop(&peer->timers, Timer_IdleHoldReset);
		session_close_connection(peer);
		msgbuf_clear(&peer->wbuf);
		free(peer->rbuf);
		peer->rbuf = NULL;
		peer->rpending = 0;
		memset(&peer->capa.peer, 0, sizeof(peer->capa.peer));
		if (!peer->template)
			imsg_compose(ibuf_main, IMSG_PFKEY_RELOAD,
			    peer->conf.id, 0, -1, NULL, 0);

		if (event != EVNT_STOP) {
			timer_set(&peer->timers, Timer_IdleHold,
			    peer->IdleHoldTime);
			if (event != EVNT_NONE &&
			    peer->IdleHoldTime < MAX_IDLE_HOLD/2)
				peer->IdleHoldTime *= 2;
		}
		if (peer->state == STATE_ESTABLISHED) {
			if (peer->capa.neg.grestart.restart == 2 &&
			    (event == EVNT_CON_CLOSED ||
			    event == EVNT_CON_FATAL)) {
				/* don't punish graceful restart */
				timer_set(&peer->timers, Timer_IdleHold, 0);
				peer->IdleHoldTime /= 2;
				session_graceful_restart(peer);
			} else
				session_down(peer);
		}
		if (peer->state == STATE_NONE ||
		    peer->state == STATE_ESTABLISHED) {
			/* initialize capability negotiation structures */
			memcpy(&peer->capa.ann, &peer->conf.capabilities,
			    sizeof(peer->capa.ann));
			if (!peer->conf.announce_capa)
				session_capa_ann_none(peer);
		}
		break;
	case STATE_CONNECT:
		if (peer->state == STATE_ESTABLISHED &&
		    peer->capa.neg.grestart.restart == 2) {
			/* do the graceful restart dance */
			session_graceful_restart(peer);
			peer->holdtime = INTERVAL_HOLD_INITIAL;
			timer_stop(&peer->timers, Timer_ConnectRetry);
			timer_stop(&peer->timers, Timer_Keepalive);
			timer_stop(&peer->timers, Timer_Hold);
			timer_stop(&peer->timers, Timer_SendHold);
			timer_stop(&peer->timers, Timer_IdleHold);
			timer_stop(&peer->timers, Timer_IdleHoldReset);
			session_close_connection(peer);
			msgbuf_clear(&peer->wbuf);
			memset(&peer->capa.peer, 0, sizeof(peer->capa.peer));
		}
		break;
	case STATE_ACTIVE:
		if (!peer->template)
			imsg_compose(ibuf_main, IMSG_PFKEY_RELOAD,
			    peer->conf.id, 0, -1, NULL, 0);
		break;
	case STATE_OPENSENT:
		break;
	case STATE_OPENCONFIRM:
		break;
	case STATE_ESTABLISHED:
		timer_set(&peer->timers, Timer_IdleHoldReset,
		    peer->IdleHoldTime);
		if (peer->demoted)
			timer_set(&peer->timers, Timer_CarpUndemote,
			    INTERVAL_HOLD_DEMOTED);
		session_up(peer);
		break;
	default:		/* something seriously fucked */
		break;
	}

	log_statechange(peer, state, event);
	LIST_FOREACH(mrt, &mrthead, entry) {
		if (!(mrt->type == MRT_ALL_IN || mrt->type == MRT_ALL_OUT))
			continue;
		if ((mrt->peer_id == 0 && mrt->group_id == 0) ||
		    mrt->peer_id == peer->conf.id || (mrt->group_id != 0 &&
		    mrt->group_id == peer->conf.groupid))
			mrt_dump_state(mrt, peer->state, state, peer);
	}
	peer->prev_state = peer->state;
	peer->state = state;
}

void
session_accept(int listenfd)
{
	int			 connfd;
	socklen_t		 len;
	struct sockaddr_storage	 cliaddr;
	struct peer		*p = NULL;

	len = sizeof(cliaddr);
	if ((connfd = accept4(listenfd,
	    (struct sockaddr *)&cliaddr, &len,
	    SOCK_CLOEXEC | SOCK_NONBLOCK)) == -1) {
		if (errno == ENFILE || errno == EMFILE)
			pauseaccept = getmonotime();
		else if (errno != EWOULDBLOCK && errno != EINTR &&
		    errno != ECONNABORTED)
			log_warn("accept");
		return;
	}

	p = getpeerbyip(conf, (struct sockaddr *)&cliaddr);

	if (p != NULL && p->state == STATE_IDLE && p->errcnt < 2) {
		if (timer_running(&p->timers, Timer_IdleHold, NULL)) {
			/* fast reconnect after clear */
			p->passive = 1;
			bgp_fsm(p, EVNT_START);
		}
	}

	if (p != NULL &&
	    (p->state == STATE_CONNECT || p->state == STATE_ACTIVE)) {
		if (p->fd != -1) {
			if (p->state == STATE_CONNECT)
				session_close_connection(p);
			else {
				close(connfd);
				return;
			}
		}

open:
		if (p->conf.auth.method != AUTH_NONE && sysdep.no_pfkey) {
			log_peer_warnx(&p->conf,
			    "ipsec or md5sig configured but not available");
			close(connfd);
			return;
		}

		if (tcp_md5_check(connfd, p) == -1) {
			close(connfd);
			return;
		}
		p->fd = p->wbuf.fd = connfd;
		if (session_setup_socket(p)) {
			close(connfd);
			return;
		}
		bgp_fsm(p, EVNT_CON_OPEN);
		return;
	} else if (p != NULL && p->state == STATE_ESTABLISHED &&
	    p->capa.neg.grestart.restart == 2) {
		/* first do the graceful restart dance */
		change_state(p, STATE_CONNECT, EVNT_CON_CLOSED);
		/* then do part of the open dance */
		goto open;
	} else {
		log_conn_attempt(p, (struct sockaddr *)&cliaddr, len);
		close(connfd);
	}
}

int
session_connect(struct peer *peer)
{
	struct sockaddr		*sa;
	struct bgpd_addr	*bind_addr = NULL;
	socklen_t		 sa_len;

	/*
	 * we do not need the overcomplicated collision detection RFC 1771
	 * describes; we simply make sure there is only ever one concurrent
	 * tcp connection per peer.
	 */
	if (peer->fd != -1)
		return (-1);

	if ((peer->fd = socket(aid2af(peer->conf.remote_addr.aid),
	    SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP)) == -1) {
		log_peer_warn(&peer->conf, "session_connect socket");
		bgp_fsm(peer, EVNT_CON_OPENFAIL);
		return (-1);
	}

	if (peer->conf.auth.method != AUTH_NONE && sysdep.no_pfkey) {
		log_peer_warnx(&peer->conf,
		    "ipsec or md5sig configured but not available");
		bgp_fsm(peer, EVNT_CON_OPENFAIL);
		return (-1);
	}

	tcp_md5_set(peer->fd, peer);
	peer->wbuf.fd = peer->fd;

	/* if local-address is set we need to bind() */
	switch (peer->conf.remote_addr.aid) {
	case AID_INET:
		bind_addr = &peer->conf.local_addr_v4;
		break;
	case AID_INET6:
		bind_addr = &peer->conf.local_addr_v6;
		break;
	}
	if ((sa = addr2sa(bind_addr, 0, &sa_len)) != NULL) {
		if (bind(peer->fd, sa, sa_len) == -1) {
			log_peer_warn(&peer->conf, "session_connect bind");
			bgp_fsm(peer, EVNT_CON_OPENFAIL);
			return (-1);
		}
	}

	if (session_setup_socket(peer)) {
		bgp_fsm(peer, EVNT_CON_OPENFAIL);
		return (-1);
	}

	sa = addr2sa(&peer->conf.remote_addr, peer->conf.remote_port, &sa_len);
	if (connect(peer->fd, sa, sa_len) == -1) {
		if (errno != EINPROGRESS) {
			if (errno != peer->lasterr)
				log_peer_warn(&peer->conf, "connect");
			peer->lasterr = errno;
			bgp_fsm(peer, EVNT_CON_OPENFAIL);
			return (-1);
		}
	} else
		bgp_fsm(peer, EVNT_CON_OPEN);

	return (0);
}

int
session_setup_socket(struct peer *p)
{
	int	ttl = p->conf.distance;
	int	pre = IPTOS_PREC_INTERNETCONTROL;
	int	nodelay = 1;
	int	bsize;

	switch (p->conf.remote_addr.aid) {
	case AID_INET:
		/* set precedence, see RFC 1771 appendix 5 */
		if (setsockopt(p->fd, IPPROTO_IP, IP_TOS, &pre, sizeof(pre)) ==
		    -1) {
			log_peer_warn(&p->conf,
			    "session_setup_socket setsockopt TOS");
			return (-1);
		}

		if (p->conf.ebgp) {
			/*
			 * set TTL to foreign router's distance
			 * 1=direct n=multihop with ttlsec, we always use 255
			 */
			if (p->conf.ttlsec) {
				ttl = 256 - p->conf.distance;
				if (setsockopt(p->fd, IPPROTO_IP, IP_MINTTL,
				    &ttl, sizeof(ttl)) == -1) {
					log_peer_warn(&p->conf,
					    "session_setup_socket: "
					    "setsockopt MINTTL");
					return (-1);
				}
				ttl = 255;
			}

			if (setsockopt(p->fd, IPPROTO_IP, IP_TTL, &ttl,
			    sizeof(ttl)) == -1) {
				log_peer_warn(&p->conf,
				    "session_setup_socket setsockopt TTL");
				return (-1);
			}
		}
		break;
	case AID_INET6:
		if (p->conf.ebgp) {
			/*
			 * set hoplimit to foreign router's distance
			 * 1=direct n=multihop with ttlsec, we always use 255
			 */
			if (p->conf.ttlsec) {
				ttl = 256 - p->conf.distance;
				if (setsockopt(p->fd, IPPROTO_IPV6,
				    IPV6_MINHOPCOUNT, &ttl, sizeof(ttl))
				    == -1) {
					log_peer_warn(&p->conf,
					    "session_setup_socket: "
					    "setsockopt MINHOPCOUNT");
					return (-1);
				}
				ttl = 255;
			}
			if (setsockopt(p->fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS,
			    &ttl, sizeof(ttl)) == -1) {
				log_peer_warn(&p->conf,
				    "session_setup_socket setsockopt hoplimit");
				return (-1);
			}
		}
		break;
	}

	/* set TCP_NODELAY */
	if (setsockopt(p->fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
	    sizeof(nodelay)) == -1) {
		log_peer_warn(&p->conf,
		    "session_setup_socket setsockopt TCP_NODELAY");
		return (-1);
	}

	/* limit bufsize. no biggie if it fails */
	bsize = 65535;
	while (bsize > 8192 && setsockopt(p->fd, SOL_SOCKET, SO_RCVBUF,
	    &bsize, sizeof(bsize)) == -1 && errno != EINVAL)
		bsize /= 2;
	bsize = 65535;
	while (bsize > 8192 && setsockopt(p->fd, SOL_SOCKET, SO_SNDBUF,
	    &bsize, sizeof(bsize)) == -1 && errno != EINVAL)
		bsize /= 2;

	return (0);
}

/*
 * compare the bgpd_addr with the sockaddr by converting the latter into
 * a bgpd_addr. Return true if the two are equal, including any scope
 */
static int
sa_equal(struct bgpd_addr *ba, struct sockaddr *b)
{
	struct bgpd_addr bb;

	sa2addr(b, &bb, NULL);
	return (memcmp(ba, &bb, sizeof(*ba)) == 0);
}

static void
get_alternate_addr(struct bgpd_addr *local, struct bgpd_addr *remote,
    struct bgpd_addr *alt, unsigned int *scope)
{
	struct ifaddrs	*ifap, *ifa, *match;
	int connected = 0;
	u_int8_t plen;

	if (getifaddrs(&ifap) == -1)
		fatal("getifaddrs");

	for (match = ifap; match != NULL; match = match->ifa_next) {
		if (match->ifa_addr == NULL)
			continue;
		if (match->ifa_addr->sa_family != AF_INET &&
		    match->ifa_addr->sa_family != AF_INET6)
			continue;
		if (sa_equal(local, match->ifa_addr)) {
			if (match->ifa_flags & IFF_POINTOPOINT &&
			    match->ifa_dstaddr) {
				if (sa_equal(remote, match->ifa_dstaddr))
					connected = 1;
			} else if (match->ifa_netmask) {
				plen = mask2prefixlen(
				    match->ifa_addr->sa_family,
				    match->ifa_netmask);
				if (prefix_compare(local, remote, plen) == 0)
					connected = 1;
			}
			break;
		}
	}

	if (match == NULL) {
		log_warnx("%s: local address not found", __func__);
		return;
	}
	if (connected)
		*scope = if_nametoindex(match->ifa_name);
	else
		*scope = 0;

	switch (local->aid) {
	case AID_INET6:
		for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
			if (ifa->ifa_addr != NULL &&
			    ifa->ifa_addr->sa_family == AF_INET &&
			    strcmp(ifa->ifa_name, match->ifa_name) == 0) {
				sa2addr(ifa->ifa_addr, alt, NULL);
				break;
			}
		}
		break;
	case AID_INET:
		for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
			if (ifa->ifa_addr != NULL &&
			    ifa->ifa_addr->sa_family == AF_INET6 &&
			    strcmp(ifa->ifa_name, match->ifa_name) == 0) {
				struct sockaddr_in6 *s =
				    (struct sockaddr_in6 *)ifa->ifa_addr;

				/* only accept global scope addresses */
				if (IN6_IS_ADDR_LINKLOCAL(&s->sin6_addr) ||
				    IN6_IS_ADDR_SITELOCAL(&s->sin6_addr))
					continue;
				sa2addr(ifa->ifa_addr, alt, NULL);
				break;
			}
		}
		break;
	default:
		log_warnx("%s: unsupported address family %s", __func__,
		    aid2str(local->aid));
		break;
	}

	freeifaddrs(ifap);
}

void
session_tcp_established(struct peer *peer)
{
	struct sockaddr_storage	ss;
	socklen_t		len;

	len = sizeof(ss);
	if (getsockname(peer->fd, (struct sockaddr *)&ss, &len) == -1)
		log_warn("getsockname");
	sa2addr((struct sockaddr *)&ss, &peer->local, &peer->local_port);
	len = sizeof(ss);
	if (getpeername(peer->fd, (struct sockaddr *)&ss, &len) == -1)
		log_warn("getpeername");
	sa2addr((struct sockaddr *)&ss, &peer->remote, &peer->remote_port);

	get_alternate_addr(&peer->local, &peer->remote, &peer->local_alt,
	    &peer->if_scope);
}

void
session_capa_ann_none(struct peer *peer)
{
	memset(&peer->capa.ann, 0, sizeof(peer->capa.ann));
}

int
session_capa_add(struct ibuf *opb, uint8_t capa_code, uint8_t capa_len)
{
	int errs = 0;

	errs += ibuf_add_n8(opb, capa_code);
	errs += ibuf_add_n8(opb, capa_len);
	return (errs);
}

int
session_capa_add_mp(struct ibuf *buf, uint8_t aid)
{
	uint16_t		 afi;
	uint8_t			 safi;
	int			 errs = 0;

	if (aid2afi(aid, &afi, &safi) == -1) {
		log_warn("%s: bad AID", __func__);
		return (-1);
	}

	errs += ibuf_add_n16(buf, afi);
	errs += ibuf_add_zero(buf, 1);
	errs += ibuf_add_n8(buf, safi);

	return (errs);
}

int
session_capa_add_afi(struct ibuf *b, uint8_t aid, uint8_t flags)
{
	u_int		errs = 0;
	uint16_t	afi;
	uint8_t		safi;

	if (aid2afi(aid, &afi, &safi)) {
		log_warn("%s: bad AID", __func__);
		return (-1);
	}

	errs += ibuf_add_n16(b, afi);
	errs += ibuf_add_n8(b, safi);
	errs += ibuf_add_n8(b, flags);

	return (errs);
}

struct bgp_msg *
session_newmsg(enum msg_type msgtype, uint16_t len)
{
	u_char			 marker[MSGSIZE_HEADER_MARKER];
	struct bgp_msg		*msg;
	struct ibuf		*buf;
	int			 errs = 0;

	memset(marker, 0xff, sizeof(marker));

	if ((buf = ibuf_open(len)) == NULL)
		return (NULL);

	errs += ibuf_add(buf, marker, sizeof(marker));
	errs += ibuf_add_n16(buf, len);
	errs += ibuf_add_n8(buf, msgtype);

	if (errs || (msg = calloc(1, sizeof(*msg))) == NULL) {
		ibuf_free(buf);
		return (NULL);
	}

	msg->buf = buf;
	msg->type = msgtype;
	msg->len = len;

	return (msg);
}

int
session_sendmsg(struct bgp_msg *msg, struct peer *p)
{
	struct mrt		*mrt;

	LIST_FOREACH(mrt, &mrthead, entry) {
		if (!(mrt->type == MRT_ALL_OUT || (msg->type == UPDATE &&
		    mrt->type == MRT_UPDATE_OUT)))
			continue;
		if ((mrt->peer_id == 0 && mrt->group_id == 0) ||
		    mrt->peer_id == p->conf.id || (mrt->group_id != 0 &&
		    mrt->group_id == p->conf.groupid))
			mrt_dump_bgp_msg(mrt, ibuf_data(msg->buf), msg->len, p,
			    msg->type);
	}

	ibuf_close(&p->wbuf, msg->buf);
	if (!p->throttled && p->wbuf.queued > SESS_MSG_HIGH_MARK) {
		if (imsg_rde(IMSG_XOFF, p->conf.id, NULL, 0) == -1)
			log_peer_warn(&p->conf, "imsg_compose XOFF");
		else
			p->throttled = 1;
	}

	free(msg);
	return (0);
}

/*
 * Translate between internal roles and the value expected by RFC 9234.
 */
static uint8_t
role2capa(enum role role)
{
	switch (role) {
	case ROLE_CUSTOMER:
		return CAPA_ROLE_CUSTOMER;
	case ROLE_PROVIDER:
		return CAPA_ROLE_PROVIDER;
	case ROLE_RS:
		return CAPA_ROLE_RS;
	case ROLE_RS_CLIENT:
		return CAPA_ROLE_RS_CLIENT;
	case ROLE_PEER:
		return CAPA_ROLE_PEER;
	default:
		fatalx("Unsupported role for role capability");
	}
}

static enum role
capa2role(uint8_t val)
{
	switch (val) {
	case CAPA_ROLE_PROVIDER:
		return ROLE_PROVIDER;
	case CAPA_ROLE_RS:
		return ROLE_RS;
	case CAPA_ROLE_RS_CLIENT:
		return ROLE_RS_CLIENT;
	case CAPA_ROLE_CUSTOMER:
		return ROLE_CUSTOMER;
	case CAPA_ROLE_PEER:
		return ROLE_PEER;
	default:
		return ROLE_NONE;
	}
}

void
session_open(struct peer *p)
{
	struct bgp_msg		*buf;
	struct ibuf		*opb;
	size_t			 len, optparamlen;
	uint16_t		 holdtime;
	uint8_t			 i;
	int			 errs = 0, extlen = 0;
	int			 mpcapa = 0;


	if ((opb = ibuf_dynamic(0, UINT16_MAX - 3)) == NULL) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	/* multiprotocol extensions, RFC 4760 */
	for (i = AID_MIN; i < AID_MAX; i++)
		if (p->capa.ann.mp[i]) {	/* 4 bytes data */
			errs += session_capa_add(opb, CAPA_MP, 4);
			errs += session_capa_add_mp(opb, i);
			mpcapa++;
		}

	/* route refresh, RFC 2918 */
	if (p->capa.ann.refresh)	/* no data */
		errs += session_capa_add(opb, CAPA_REFRESH, 0);

	/* BGP open policy, RFC 9234, only for ebgp sessions */
	if (p->conf.ebgp && p->capa.ann.policy &&
	    p->conf.role != ROLE_NONE &&
	    (p->capa.ann.mp[AID_INET] || p->capa.ann.mp[AID_INET6] ||
	    mpcapa == 0)) {
		errs += session_capa_add(opb, CAPA_ROLE, 1);
		errs += ibuf_add_n8(opb, role2capa(p->conf.role));
	}

	/* graceful restart and End-of-RIB marker, RFC 4724 */
	if (p->capa.ann.grestart.restart) {
		int		rst = 0;
		uint16_t	hdr = 0;

		for (i = AID_MIN; i < AID_MAX; i++) {
			if (p->capa.neg.grestart.flags[i] & CAPA_GR_RESTARTING)
				rst++;
		}

		/* Only set the R-flag if no graceful restart is ongoing */
		if (!rst)
			hdr |= CAPA_GR_R_FLAG;
		errs += session_capa_add(opb, CAPA_RESTART, sizeof(hdr));
		errs += ibuf_add_n16(opb, hdr);
	}

	/* 4-bytes AS numbers, RFC6793 */
	if (p->capa.ann.as4byte) {	/* 4 bytes data */
		errs += session_capa_add(opb, CAPA_AS4BYTE, sizeof(uint32_t));
		errs += ibuf_add_n32(opb, p->conf.local_as);
	}

	/* advertisement of multiple paths, RFC7911 */
	if (p->capa.ann.add_path[AID_MIN]) {	/* variable */
		uint8_t	aplen;

		if (mpcapa)
			aplen = 4 * mpcapa;
		else	/* AID_INET */
			aplen = 4;
		errs += session_capa_add(opb, CAPA_ADD_PATH, aplen);
		if (mpcapa) {
			for (i = AID_MIN; i < AID_MAX; i++) {
				if (p->capa.ann.mp[i]) {
					errs += session_capa_add_afi(opb,
					    i, p->capa.ann.add_path[i]);
				}
			}
		} else {	/* AID_INET */
			errs += session_capa_add_afi(opb, AID_INET,
			    p->capa.ann.add_path[AID_INET]);
		}
	}

	/* enhanced route-refresh, RFC7313 */
	if (p->capa.ann.enhanced_rr)	/* no data */
		errs += session_capa_add(opb, CAPA_ENHANCED_RR, 0);

	if (errs) {
		ibuf_free(opb);
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	optparamlen = ibuf_size(opb);
	len = MSGSIZE_OPEN_MIN + optparamlen;
	if (optparamlen == 0) {
		/* nothing */
	} else if (optparamlen + 2 >= 255) {
		/* RFC9072: use 255 as magic size and request extra header */
		optparamlen = 255;
		extlen = 1;
		/* 3 byte OPT_PARAM_EXT_LEN and OPT_PARAM_CAPABILITIES */
		len += 2 * 3;
	} else {
		/* regular capabilities header */
		optparamlen += 2;
		len += 2;
	}

	if ((buf = session_newmsg(OPEN, len)) == NULL) {
		ibuf_free(opb);
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	if (p->conf.holdtime)
		holdtime = p->conf.holdtime;
	else
		holdtime = conf->holdtime;

	errs += ibuf_add_n8(buf->buf, 4);
	errs += ibuf_add_n16(buf->buf, p->conf.local_short_as);
	errs += ibuf_add_n16(buf->buf, holdtime);
	/* is already in network byte order */
	errs += ibuf_add(buf->buf, &conf->bgpid, sizeof(conf->bgpid));
	errs += ibuf_add_n8(buf->buf, optparamlen);

	if (extlen) {
		/* RFC9072 extra header which spans over the capabilities hdr */
		errs += ibuf_add_n8(buf->buf, OPT_PARAM_EXT_LEN);
		errs += ibuf_add_n16(buf->buf, ibuf_size(opb) + 1 + 2);
	}

	if (optparamlen) {
		errs += ibuf_add_n8(buf->buf, OPT_PARAM_CAPABILITIES);

		if (extlen) {
			/* RFC9072: 2-byte extended length */
			errs += ibuf_add_n16(buf->buf, ibuf_size(opb));
		} else {
			errs += ibuf_add_n8(buf->buf, ibuf_size(opb));
		}
		errs += ibuf_add_buf(buf->buf, opb);
	}

	ibuf_free(opb);

	if (errs) {
		ibuf_free(buf->buf);
		free(buf);
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	if (session_sendmsg(buf, p) == -1) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	p->stats.msg_sent_open++;
}

void
session_keepalive(struct peer *p)
{
	struct bgp_msg		*buf;

	if ((buf = session_newmsg(KEEPALIVE, MSGSIZE_KEEPALIVE)) == NULL ||
	    session_sendmsg(buf, p) == -1) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	start_timer_keepalive(p);
	p->stats.msg_sent_keepalive++;
}

void
session_update(uint32_t peerid, struct ibuf *ibuf)
{
	struct peer		*p;
	struct bgp_msg		*buf;

	if ((p = getpeerbyid(conf, peerid)) == NULL) {
		log_warnx("no such peer: id=%u", peerid);
		return;
	}

	if (p->state != STATE_ESTABLISHED)
		return;

	if ((buf = session_newmsg(UPDATE, MSGSIZE_HEADER + ibuf_size(ibuf))) ==
	    NULL) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	if (ibuf_add_buf(buf->buf, ibuf)) {
		ibuf_free(buf->buf);
		free(buf);
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	if (session_sendmsg(buf, p) == -1) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	start_timer_keepalive(p);
	p->stats.msg_sent_update++;
}

void
session_notification_data(struct peer *p, uint8_t errcode, uint8_t subcode,
    void *data, size_t datalen)
{
	struct ibuf ibuf;

	ibuf_from_buffer(&ibuf, data, datalen);
	session_notification(p, errcode, subcode, &ibuf);
}

void
session_notification(struct peer *p, uint8_t errcode, uint8_t subcode,
    struct ibuf *ibuf)
{
	struct bgp_msg		*buf;
	int			 errs = 0;
	size_t			 datalen = 0;

	if (p->stats.last_sent_errcode)	/* some notification already sent */
		return;

	log_notification(p, errcode, subcode, ibuf, "sending");

	/* cap to maximum size */
	if (ibuf != NULL) {
		if (ibuf_size(ibuf) >
		    MAX_PKTSIZE - MSGSIZE_NOTIFICATION_MIN) {
			log_peer_warnx(&p->conf,
			    "oversized notification, data trunkated");
			ibuf_truncate(ibuf, MAX_PKTSIZE -
			    MSGSIZE_NOTIFICATION_MIN);
		}
		datalen = ibuf_size(ibuf);
	}

	if ((buf = session_newmsg(NOTIFICATION,
	    MSGSIZE_NOTIFICATION_MIN + datalen)) == NULL) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	errs += ibuf_add_n8(buf->buf, errcode);
	errs += ibuf_add_n8(buf->buf, subcode);

	if (ibuf != NULL)
		errs += ibuf_add_buf(buf->buf, ibuf);

	if (errs) {
		ibuf_free(buf->buf);
		free(buf);
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	if (session_sendmsg(buf, p) == -1) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	p->stats.msg_sent_notification++;
	p->stats.last_sent_errcode = errcode;
	p->stats.last_sent_suberr = subcode;
}

int
session_neighbor_rrefresh(struct peer *p)
{
	uint8_t	i;

	if (!(p->capa.neg.refresh || p->capa.neg.enhanced_rr))
		return (-1);

	for (i = AID_MIN; i < AID_MAX; i++) {
		if (p->capa.neg.mp[i] != 0)
			session_rrefresh(p, i, ROUTE_REFRESH_REQUEST);
	}

	return (0);
}

void
session_rrefresh(struct peer *p, uint8_t aid, uint8_t subtype)
{
	struct bgp_msg		*buf;
	int			 errs = 0;
	uint16_t		 afi;
	uint8_t			 safi;

	switch (subtype) {
	case ROUTE_REFRESH_REQUEST:
		p->stats.refresh_sent_req++;
		break;
	case ROUTE_REFRESH_BEGIN_RR:
	case ROUTE_REFRESH_END_RR:
		/* requires enhanced route refresh */
		if (!p->capa.neg.enhanced_rr)
			return;
		if (subtype == ROUTE_REFRESH_BEGIN_RR)
			p->stats.refresh_sent_borr++;
		else
			p->stats.refresh_sent_eorr++;
		break;
	default:
		fatalx("session_rrefresh: bad subtype %d", subtype);
	}

	if (aid2afi(aid, &afi, &safi) == -1)
		fatalx("session_rrefresh: bad afi/safi pair");

	if ((buf = session_newmsg(RREFRESH, MSGSIZE_RREFRESH)) == NULL) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	errs += ibuf_add_n16(buf->buf, afi);
	errs += ibuf_add_n8(buf->buf, subtype);
	errs += ibuf_add_n8(buf->buf, safi);

	if (errs) {
		ibuf_free(buf->buf);
		free(buf);
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	if (session_sendmsg(buf, p) == -1) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return;
	}

	p->stats.msg_sent_rrefresh++;
}

int
session_graceful_restart(struct peer *p)
{
	uint8_t	i;

	timer_set(&p->timers, Timer_RestartTimeout,
	    p->capa.neg.grestart.timeout);

	for (i = AID_MIN; i < AID_MAX; i++) {
		if (p->capa.neg.grestart.flags[i] & CAPA_GR_PRESENT) {
			if (imsg_rde(IMSG_SESSION_STALE, p->conf.id,
			    &i, sizeof(i)) == -1)
				return (-1);
			log_peer_warnx(&p->conf,
			    "graceful restart of %s, keeping routes",
			    aid2str(i));
			p->capa.neg.grestart.flags[i] |= CAPA_GR_RESTARTING;
		} else if (p->capa.neg.mp[i]) {
			if (imsg_rde(IMSG_SESSION_NOGRACE, p->conf.id,
			    &i, sizeof(i)) == -1)
				return (-1);
			log_peer_warnx(&p->conf,
			    "graceful restart of %s, flushing routes",
			    aid2str(i));
		}
	}
	return (0);
}

int
session_graceful_stop(struct peer *p)
{
	uint8_t	i;

	for (i = AID_MIN; i < AID_MAX; i++) {
		/*
		 * Only flush if the peer is restarting and the timeout fired.
		 * In all other cases the session was already flushed when the
		 * session went down or when the new open message was parsed.
		 */
		if (p->capa.neg.grestart.flags[i] & CAPA_GR_RESTARTING) {
			log_peer_warnx(&p->conf, "graceful restart of %s, "
			    "time-out, flushing", aid2str(i));
			if (imsg_rde(IMSG_SESSION_FLUSH, p->conf.id,
			    &i, sizeof(i)) == -1)
				return (-1);
		}
		p->capa.neg.grestart.flags[i] &= ~CAPA_GR_RESTARTING;
	}
	return (0);
}

int
session_dispatch_msg(struct pollfd *pfd, struct peer *p)
{
	ssize_t		n;
	socklen_t	len;
	int		error;

	if (p->state == STATE_CONNECT) {
		if (pfd->revents & POLLOUT) {
			if (pfd->revents & POLLIN) {
				/* error occurred */
				len = sizeof(error);
				if (getsockopt(pfd->fd, SOL_SOCKET, SO_ERROR,
				    &error, &len) == -1 || error) {
					if (error)
						errno = error;
					if (errno != p->lasterr) {
						log_peer_warn(&p->conf,
						    "socket error");
						p->lasterr = errno;
					}
					bgp_fsm(p, EVNT_CON_OPENFAIL);
					return (1);
				}
			}
			bgp_fsm(p, EVNT_CON_OPEN);
			return (1);
		}
		if (pfd->revents & POLLHUP) {
			bgp_fsm(p, EVNT_CON_OPENFAIL);
			return (1);
		}
		if (pfd->revents & (POLLERR|POLLNVAL)) {
			bgp_fsm(p, EVNT_CON_FATAL);
			return (1);
		}
		return (0);
	}

	if (pfd->revents & POLLHUP) {
		bgp_fsm(p, EVNT_CON_CLOSED);
		return (1);
	}
	if (pfd->revents & (POLLERR|POLLNVAL)) {
		bgp_fsm(p, EVNT_CON_FATAL);
		return (1);
	}

	if (pfd->revents & POLLOUT && p->wbuf.queued) {
		if ((error = msgbuf_write(&p->wbuf)) <= 0 && errno != EAGAIN) {
			if (error == 0)
				log_peer_warnx(&p->conf, "Connection closed");
			else if (error == -1)
				log_peer_warn(&p->conf, "write error");
			bgp_fsm(p, EVNT_CON_FATAL);
			return (1);
		}
		p->stats.last_write = getmonotime();
		if (p->holdtime > 0)
			timer_set(&p->timers, Timer_SendHold,
			    p->holdtime < INTERVAL_HOLD ? INTERVAL_HOLD :
			    p->holdtime);
		if (p->throttled && p->wbuf.queued < SESS_MSG_LOW_MARK) {
			if (imsg_rde(IMSG_XON, p->conf.id, NULL, 0) == -1)
				log_peer_warn(&p->conf, "imsg_compose XON");
			else
				p->throttled = 0;
		}
		if (!(pfd->revents & POLLIN))
			return (1);
	}

	if (p->rbuf && pfd->revents & POLLIN) {
		if ((n = read(p->fd, p->rbuf->buf + p->rbuf->wpos,
		    sizeof(p->rbuf->buf) - p->rbuf->wpos)) == -1) {
			if (errno != EINTR && errno != EAGAIN) {
				log_peer_warn(&p->conf, "read error");
				bgp_fsm(p, EVNT_CON_FATAL);
			}
			return (1);
		}
		if (n == 0) {	/* connection closed */
			bgp_fsm(p, EVNT_CON_CLOSED);
			return (1);
		}

		p->rbuf->wpos += n;
		p->stats.last_read = getmonotime();
		return (1);
	}
	return (0);
}

void
session_process_msg(struct peer *p)
{
	struct mrt	*mrt;
	ssize_t		rpos, av, left;
	int		processed = 0;
	uint16_t	msglen;
	uint8_t		msgtype;

	rpos = 0;
	av = p->rbuf->wpos;
	p->rpending = 0;

	/*
	 * session might drop to IDLE -> buffers deallocated
	 * we MUST check rbuf != NULL before use
	 */
	for (;;) {
		if (p->rbuf == NULL)
			return;
		if (rpos + MSGSIZE_HEADER > av)
			break;
		if (parse_header(p, p->rbuf->buf + rpos, &msglen,
		    &msgtype) == -1)
			return;
		if (rpos + msglen > av)
			break;
		p->rbuf->rptr = p->rbuf->buf + rpos;

		/* dump to MRT as soon as we have a full packet */
		LIST_FOREACH(mrt, &mrthead, entry) {
			if (!(mrt->type == MRT_ALL_IN || (msgtype == UPDATE &&
			    mrt->type == MRT_UPDATE_IN)))
				continue;
			if ((mrt->peer_id == 0 && mrt->group_id == 0) ||
			    mrt->peer_id == p->conf.id || (mrt->group_id != 0 &&
			    mrt->group_id == p->conf.groupid))
				mrt_dump_bgp_msg(mrt, p->rbuf->rptr, msglen, p,
				    msgtype);
		}

		switch (msgtype) {
		case OPEN:
			bgp_fsm(p, EVNT_RCVD_OPEN);
			p->stats.msg_rcvd_open++;
			break;
		case UPDATE:
			bgp_fsm(p, EVNT_RCVD_UPDATE);
			p->stats.msg_rcvd_update++;
			break;
		case NOTIFICATION:
			bgp_fsm(p, EVNT_RCVD_NOTIFICATION);
			p->stats.msg_rcvd_notification++;
			break;
		case KEEPALIVE:
			bgp_fsm(p, EVNT_RCVD_KEEPALIVE);
			p->stats.msg_rcvd_keepalive++;
			break;
		case RREFRESH:
			parse_rrefresh(p);
			p->stats.msg_rcvd_rrefresh++;
			break;
		default:	/* cannot happen */
			session_notification_data(p, ERR_HEADER, ERR_HDR_TYPE,
			    &msgtype, 1);
			log_warnx("received message with unknown type %u",
			    msgtype);
			bgp_fsm(p, EVNT_CON_FATAL);
		}
		rpos += msglen;
		if (++processed > MSG_PROCESS_LIMIT) {
			p->rpending = 1;
			break;
		}
	}

	if (p->rbuf == NULL)
		return;
	if (rpos < av) {
		left = av - rpos;
		memmove(&p->rbuf->buf, p->rbuf->buf + rpos, left);
		p->rbuf->wpos = left;
	} else
		p->rbuf->wpos = 0;
}

int
parse_header(struct peer *peer, u_char *data, uint16_t *len, uint8_t *type)
{
	u_char			*p;
	uint16_t		 olen;
	static const uint8_t	 marker[MSGSIZE_HEADER_MARKER] = { 0xff, 0xff,
				    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
				    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	/* caller MUST make sure we are getting 19 bytes! */
	p = data;
	if (memcmp(p, marker, sizeof(marker))) {
		log_peer_warnx(&peer->conf, "sync error");
		session_notification(peer, ERR_HEADER, ERR_HDR_SYNC, NULL);
		bgp_fsm(peer, EVNT_CON_FATAL);
		return (-1);
	}
	p += MSGSIZE_HEADER_MARKER;

	memcpy(&olen, p, 2);
	*len = ntohs(olen);
	p += 2;
	memcpy(type, p, 1);

	if (*len < MSGSIZE_HEADER || *len > MAX_PKTSIZE) {
		log_peer_warnx(&peer->conf,
		    "received message: illegal length: %u byte", *len);
		session_notification_data(peer, ERR_HEADER, ERR_HDR_LEN,
		    &olen, sizeof(olen));
		bgp_fsm(peer, EVNT_CON_FATAL);
		return (-1);
	}

	switch (*type) {
	case OPEN:
		if (*len < MSGSIZE_OPEN_MIN) {
			log_peer_warnx(&peer->conf,
			    "received OPEN: illegal len: %u byte", *len);
			session_notification_data(peer, ERR_HEADER, ERR_HDR_LEN,
			    &olen, sizeof(olen));
			bgp_fsm(peer, EVNT_CON_FATAL);
			return (-1);
		}
		break;
	case NOTIFICATION:
		if (*len < MSGSIZE_NOTIFICATION_MIN) {
			log_peer_warnx(&peer->conf,
			    "received NOTIFICATION: illegal len: %u byte",
			    *len);
			session_notification_data(peer, ERR_HEADER, ERR_HDR_LEN,
			    &olen, sizeof(olen));
			bgp_fsm(peer, EVNT_CON_FATAL);
			return (-1);
		}
		break;
	case UPDATE:
		if (*len < MSGSIZE_UPDATE_MIN) {
			log_peer_warnx(&peer->conf,
			    "received UPDATE: illegal len: %u byte", *len);
			session_notification_data(peer, ERR_HEADER, ERR_HDR_LEN,
			    &olen, sizeof(olen));
			bgp_fsm(peer, EVNT_CON_FATAL);
			return (-1);
		}
		break;
	case KEEPALIVE:
		if (*len != MSGSIZE_KEEPALIVE) {
			log_peer_warnx(&peer->conf,
			    "received KEEPALIVE: illegal len: %u byte", *len);
			session_notification_data(peer, ERR_HEADER, ERR_HDR_LEN,
			    &olen, sizeof(olen));
			bgp_fsm(peer, EVNT_CON_FATAL);
			return (-1);
		}
		break;
	case RREFRESH:
		if (*len < MSGSIZE_RREFRESH_MIN) {
			log_peer_warnx(&peer->conf,
			    "received RREFRESH: illegal len: %u byte", *len);
			session_notification_data(peer, ERR_HEADER, ERR_HDR_LEN,
			    &olen, sizeof(olen));
			bgp_fsm(peer, EVNT_CON_FATAL);
			return (-1);
		}
		break;
	default:
		log_peer_warnx(&peer->conf,
		    "received msg with unknown type %u", *type);
		session_notification_data(peer, ERR_HEADER, ERR_HDR_TYPE,
		    type, 1);
		bgp_fsm(peer, EVNT_CON_FATAL);
		return (-1);
	}
	return (0);
}

int
parse_open(struct peer *peer)
{
	u_char		*p, *op_val;
	uint8_t		 version, rversion;
	uint16_t	 short_as, msglen;
	uint16_t	 holdtime, oholdtime, myholdtime;
	uint32_t	 as, bgpid;
	uint16_t	 optparamlen, extlen, plen, op_len;
	uint8_t		 op_type, suberr = 0;

	p = peer->rbuf->rptr;
	p += MSGSIZE_HEADER_MARKER;
	memcpy(&msglen, p, sizeof(msglen));
	msglen = ntohs(msglen);

	p = peer->rbuf->rptr;
	p += MSGSIZE_HEADER;	/* header is already checked */

	memcpy(&version, p, sizeof(version));
	p += sizeof(version);

	if (version != BGP_VERSION) {
		log_peer_warnx(&peer->conf,
		    "peer wants unrecognized version %u", version);
		if (version > BGP_VERSION)
			rversion = version - BGP_VERSION;
		else
			rversion = BGP_VERSION;
		session_notification_data(peer, ERR_OPEN, ERR_OPEN_VERSION,
		    &rversion, sizeof(rversion));
		change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
		return (-1);
	}

	memcpy(&short_as, p, sizeof(short_as));
	p += sizeof(short_as);
	as = peer->short_as = ntohs(short_as);
	if (as == 0) {
		log_peer_warnx(&peer->conf,
		    "peer requests unacceptable AS %u", as);
		session_notification(peer, ERR_OPEN, ERR_OPEN_AS, NULL);
		change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
		return (-1);
	}

	memcpy(&oholdtime, p, sizeof(oholdtime));
	p += sizeof(oholdtime);

	holdtime = ntohs(oholdtime);
	if (holdtime && holdtime < peer->conf.min_holdtime) {
		log_peer_warnx(&peer->conf,
		    "peer requests unacceptable holdtime %u", holdtime);
		session_notification(peer, ERR_OPEN, ERR_OPEN_HOLDTIME, NULL);
		change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
		return (-1);
	}

	myholdtime = peer->conf.holdtime;
	if (!myholdtime)
		myholdtime = conf->holdtime;
	if (holdtime < myholdtime)
		peer->holdtime = holdtime;
	else
		peer->holdtime = myholdtime;

	memcpy(&bgpid, p, sizeof(bgpid));
	p += sizeof(bgpid);

	/* check bgpid for validity - just disallow 0 */
	if (ntohl(bgpid) == 0) {
		log_peer_warnx(&peer->conf, "peer BGPID %u unacceptable",
		    ntohl(bgpid));
		session_notification(peer, ERR_OPEN, ERR_OPEN_BGPID, NULL);
		change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
		return (-1);
	}
	peer->remote_bgpid = bgpid;

	extlen = 0;
	optparamlen = *p++;

	if (optparamlen == 0) {
		if (msglen != MSGSIZE_OPEN_MIN) {
bad_len:
			log_peer_warnx(&peer->conf,
			    "corrupt OPEN message received: length mismatch");
			session_notification(peer, ERR_OPEN, 0, NULL);
			change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
			return (-1);
		}
	} else {
		if (msglen < MSGSIZE_OPEN_MIN + 1)
			goto bad_len;

		op_type = *p;
		if (op_type == OPT_PARAM_EXT_LEN) {
			p++;
			memcpy(&optparamlen, p, sizeof(optparamlen));
			optparamlen = ntohs(optparamlen);
			p += sizeof(optparamlen);
			extlen = 1;
		}

		/* RFC9020 encoding has 3 extra bytes */
		if (optparamlen + 3 * extlen != msglen - MSGSIZE_OPEN_MIN)
			goto bad_len;
	}

	plen = optparamlen;
	while (plen > 0) {
		if (plen < 2 + extlen)
			goto bad_len;

		memcpy(&op_type, p, sizeof(op_type));
		p += sizeof(op_type);
		plen -= sizeof(op_type);
		if (!extlen) {
			op_len = *p++;
			plen--;
		} else {
			memcpy(&op_len, p, sizeof(op_len));
			op_len = ntohs(op_len);
			p += sizeof(op_len);
			plen -= sizeof(op_len);
		}
		if (op_len > 0) {
			if (plen < op_len)
				goto bad_len;
			op_val = p;
			p += op_len;
			plen -= op_len;
		} else
			op_val = NULL;

		switch (op_type) {
		case OPT_PARAM_CAPABILITIES:		/* RFC 3392 */
			if (parse_capabilities(peer, op_val, op_len,
			    &as) == -1) {
				session_notification(peer, ERR_OPEN, 0, NULL);
				change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
				return (-1);
			}
			break;
		case OPT_PARAM_AUTH:			/* deprecated */
		default:
			/*
			 * unsupported type
			 * the RFCs tell us to leave the data section empty
			 * and notify the peer with ERR_OPEN, ERR_OPEN_OPT.
			 * How the peer should know _which_ optional parameter
			 * we don't support is beyond me.
			 */
			log_peer_warnx(&peer->conf,
			    "received OPEN message with unsupported optional "
			    "parameter: type %u", op_type);
			session_notification(peer, ERR_OPEN, ERR_OPEN_OPT,
				NULL);
			change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
			/* no punish */
			timer_set(&peer->timers, Timer_IdleHold, 0);
			peer->IdleHoldTime /= 2;
			return (-1);
		}
	}

	/* if remote-as is zero and it's a cloned neighbor, accept any */
	if (peer->template && !peer->conf.remote_as && as != AS_TRANS) {
		peer->conf.remote_as = as;
		peer->conf.ebgp = (peer->conf.remote_as != peer->conf.local_as);
		if (!peer->conf.ebgp)
			/* force enforce_as off for iBGP sessions */
			peer->conf.enforce_as = ENFORCE_AS_OFF;
	}

	if (peer->conf.remote_as != as) {
		log_peer_warnx(&peer->conf, "peer sent wrong AS %s",
		    log_as(as));
		session_notification(peer, ERR_OPEN, ERR_OPEN_AS, NULL);
		change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
		return (-1);
	}

	/* on iBGP sessions check for bgpid collision */
	if (!peer->conf.ebgp && peer->remote_bgpid == conf->bgpid) {
		log_peer_warnx(&peer->conf, "peer BGPID %u conflicts with ours",
		    ntohl(bgpid));
		session_notification(peer, ERR_OPEN, ERR_OPEN_BGPID, NULL);
		change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
		return (-1);
	}

	if (capa_neg_calc(peer, &suberr) == -1) {
		session_notification(peer, ERR_OPEN, suberr, NULL);
		change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
		return (-1);
	}

	return (0);
}

int
parse_update(struct peer *peer)
{
	u_char		*p;
	uint16_t	 datalen;

	/*
	 * we pass the message verbatim to the rde.
	 * in case of errors the whole session is reset with a
	 * notification anyway, we only need to know the peer
	 */
	p = peer->rbuf->rptr;
	p += MSGSIZE_HEADER_MARKER;
	memcpy(&datalen, p, sizeof(datalen));
	datalen = ntohs(datalen);

	p = peer->rbuf->rptr;
	p += MSGSIZE_HEADER;	/* header is already checked */
	datalen -= MSGSIZE_HEADER;

	if (imsg_rde(IMSG_UPDATE, peer->conf.id, p, datalen) == -1)
		return (-1);

	return (0);
}

int
parse_rrefresh(struct peer *peer)
{
	struct route_refresh rr;
	uint16_t afi, datalen;
	uint8_t aid, safi, subtype;
	u_char *p;

	p = peer->rbuf->rptr;
	p += MSGSIZE_HEADER_MARKER;
	memcpy(&datalen, p, sizeof(datalen));
	datalen = ntohs(datalen);

	p = peer->rbuf->rptr;
	p += MSGSIZE_HEADER;	/* header is already checked */

	/*
	 * We could check if we actually announced the capability but
	 * as long as the message is correctly encoded we don't care.
	 */

	/* afi, 2 byte */
	memcpy(&afi, p, sizeof(afi));
	afi = ntohs(afi);
	p += 2;
	/* subtype, 1 byte */
	subtype = *p;
	p += 1;
	/* safi, 1 byte */
	safi = *p;

	/* check subtype if peer announced enhanced route refresh */
	if (peer->capa.neg.enhanced_rr) {
		switch (subtype) {
		case ROUTE_REFRESH_REQUEST:
			/* no ORF support, so no oversized RREFRESH msgs */
			if (datalen != MSGSIZE_RREFRESH) {
				log_peer_warnx(&peer->conf,
				    "received RREFRESH: illegal len: %u byte",
				    datalen);
				datalen = htons(datalen);
				session_notification_data(peer, ERR_HEADER,
				    ERR_HDR_LEN, &datalen, sizeof(datalen));
				bgp_fsm(peer, EVNT_CON_FATAL);
				return (-1);
			}
			peer->stats.refresh_rcvd_req++;
			break;
		case ROUTE_REFRESH_BEGIN_RR:
		case ROUTE_REFRESH_END_RR:
			/* special handling for RFC7313 */
			if (datalen != MSGSIZE_RREFRESH) {
				log_peer_warnx(&peer->conf,
				    "received RREFRESH: illegal len: %u byte",
				    datalen);
				p = peer->rbuf->rptr;
				p += MSGSIZE_HEADER;
				datalen -= MSGSIZE_HEADER;
				session_notification_data(peer, ERR_RREFRESH,
				    ERR_RR_INV_LEN, p, datalen);
				bgp_fsm(peer, EVNT_CON_FATAL);
				return (-1);
			}
			if (subtype == ROUTE_REFRESH_BEGIN_RR)
				peer->stats.refresh_rcvd_borr++;
			else
				peer->stats.refresh_rcvd_eorr++;
			break;
		default:
			log_peer_warnx(&peer->conf, "peer sent bad refresh, "
			    "bad subtype %d", subtype);
			return (0);
		}
	} else {
		/* force subtype to default */
		subtype = ROUTE_REFRESH_REQUEST;
		peer->stats.refresh_rcvd_req++;
	}

	/* afi/safi unchecked -	unrecognized values will be ignored anyway */
	if (afi2aid(afi, safi, &aid) == -1) {
		log_peer_warnx(&peer->conf, "peer sent bad refresh, "
		    "invalid afi/safi pair");
		return (0);
	}

	if (!peer->capa.neg.refresh && !peer->capa.neg.enhanced_rr) {
		log_peer_warnx(&peer->conf, "peer sent unexpected refresh");
		return (0);
	}

	rr.aid = aid;
	rr.subtype = subtype;

	if (imsg_rde(IMSG_REFRESH, peer->conf.id, &rr, sizeof(rr)) == -1)
		return (-1);

	return (0);
}

int
parse_notification(struct peer *peer)
{
	struct ibuf	 ibuf;
	u_char		*p;
	uint16_t	 datalen;
	uint8_t		 errcode, subcode;
	uint8_t		 reason_len;

	/* just log */
	p = peer->rbuf->rptr;
	p += MSGSIZE_HEADER_MARKER;
	memcpy(&datalen, p, sizeof(datalen));
	datalen = ntohs(datalen);

	p = peer->rbuf->rptr;
	p += MSGSIZE_HEADER;	/* header is already checked */
	datalen -= MSGSIZE_HEADER;

	/* XXX */
	ibuf_from_buffer(&ibuf, p, datalen);
	
	if (ibuf_get_n8(&ibuf, &errcode) == -1 ||
	    ibuf_get_n8(&ibuf, &subcode) == -1) {
		log_peer_warnx(&peer->conf, "received bad notification");
		return (-1);
	}

	peer->errcnt++;
	peer->stats.last_rcvd_errcode = errcode;
	peer->stats.last_rcvd_suberr = subcode;

	log_notification(peer, errcode, subcode, &ibuf, "received");

	CTASSERT(sizeof(peer->stats.last_reason) > UINT8_MAX);
	memset(peer->stats.last_reason, 0, sizeof(peer->stats.last_reason));
	if (errcode == ERR_CEASE &&
	    (subcode == ERR_CEASE_ADMIN_DOWN ||
	     subcode == ERR_CEASE_ADMIN_RESET)) {
		/* check if shutdown reason is included */
		if (ibuf_get_n8(&ibuf, &reason_len) != -1 && reason_len != 0) {
			if (ibuf_get(&ibuf, peer->stats.last_reason,
			    reason_len) == -1)
				log_peer_warnx(&peer->conf,
				    "received truncated shutdown reason");
		}
	}

	if (errcode == ERR_OPEN && subcode == ERR_OPEN_OPT) {
		session_capa_ann_none(peer);
		return (1);
	}

	return (0);
}

int
parse_capabilities(struct peer *peer, u_char *d, uint16_t dlen, uint32_t *as)
{
	u_char		*capa_val;
	uint32_t	 remote_as;
	uint16_t	 len;
	uint16_t	 afi;
	uint16_t	 gr_header;
	uint8_t		 safi;
	uint8_t		 aid;
	uint8_t		 flags;
	uint8_t		 capa_code;
	uint8_t		 capa_len;
	uint8_t		 i;

	len = dlen;
	while (len > 0) {
		if (len < 2) {
			log_peer_warnx(&peer->conf, "Bad capabilities attr "
			    "length: %u, too short", len);
			return (-1);
		}
		memcpy(&capa_code, d, sizeof(capa_code));
		d += sizeof(capa_code);
		len -= sizeof(capa_code);
		memcpy(&capa_len, d, sizeof(capa_len));
		d += sizeof(capa_len);
		len -= sizeof(capa_len);
		if (capa_len > 0) {
			if (len < capa_len) {
				log_peer_warnx(&peer->conf,
				    "Bad capabilities attr length: "
				    "len %u smaller than capa_len %u",
				    len, capa_len);
				return (-1);
			}
			capa_val = d;
			d += capa_len;
			len -= capa_len;
		} else
			capa_val = NULL;

		switch (capa_code) {
		case CAPA_MP:			/* RFC 4760 */
			if (capa_len != 4) {
				log_peer_warnx(&peer->conf,
				    "Bad multi protocol capability length: "
				    "%u", capa_len);
				break;
			}
			memcpy(&afi, capa_val, sizeof(afi));
			afi = ntohs(afi);
			memcpy(&safi, capa_val + 3, sizeof(safi));
			if (afi2aid(afi, safi, &aid) == -1) {
				log_peer_warnx(&peer->conf,
				    "Received multi protocol capability: "
				    " unknown AFI %u, safi %u pair",
				    afi, safi);
				break;
			}
			peer->capa.peer.mp[aid] = 1;
			break;
		case CAPA_REFRESH:
			peer->capa.peer.refresh = 1;
			break;
		case CAPA_ROLE:
			if (capa_len != 1) {
				log_peer_warnx(&peer->conf,
				    "Bad role capability length: %u", capa_len);
				break;
			}
			if (!peer->conf.ebgp) {
				log_peer_warnx(&peer->conf,
				    "Received role capability on iBGP session");
				break;
			}
			peer->capa.peer.policy = 1;
			peer->remote_role = capa2role(*capa_val);
			break;
		case CAPA_RESTART:
			if (capa_len == 2) {
				/* peer only supports EoR marker */
				peer->capa.peer.grestart.restart = 1;
				peer->capa.peer.grestart.timeout = 0;
				break;
			} else if (capa_len % 4 != 2) {
				log_peer_warnx(&peer->conf,
				    "Bad graceful restart capability length: "
				    "%u", capa_len);
				peer->capa.peer.grestart.restart = 0;
				peer->capa.peer.grestart.timeout = 0;
				break;
			}

			memcpy(&gr_header, capa_val, sizeof(gr_header));
			gr_header = ntohs(gr_header);
			peer->capa.peer.grestart.timeout =
			    gr_header & CAPA_GR_TIMEMASK;
			if (peer->capa.peer.grestart.timeout == 0) {
				log_peer_warnx(&peer->conf, "Received "
				    "graceful restart timeout is zero");
				peer->capa.peer.grestart.restart = 0;
				break;
			}

			for (i = 2; i <= capa_len - 4; i += 4) {
				memcpy(&afi, capa_val + i, sizeof(afi));
				afi = ntohs(afi);
				safi = capa_val[i + 2];
				flags = capa_val[i + 3];
				if (afi2aid(afi, safi, &aid) == -1) {
					log_peer_warnx(&peer->conf,
					    "Received graceful restart capa: "
					    " unknown AFI %u, safi %u pair",
					    afi, safi);
					continue;
				}
				peer->capa.peer.grestart.flags[aid] |=
				    CAPA_GR_PRESENT;
				if (flags & CAPA_GR_F_FLAG)
					peer->capa.peer.grestart.flags[aid] |=
					    CAPA_GR_FORWARD;
				if (gr_header & CAPA_GR_R_FLAG)
					peer->capa.peer.grestart.flags[aid] |=
					    CAPA_GR_RESTART;
				peer->capa.peer.grestart.restart = 2;
			}
			break;
		case CAPA_AS4BYTE:
			if (capa_len != 4) {
				log_peer_warnx(&peer->conf,
				    "Bad AS4BYTE capability length: "
				    "%u", capa_len);
				peer->capa.peer.as4byte = 0;
				break;
			}
			memcpy(&remote_as, capa_val, sizeof(remote_as));
			*as = ntohl(remote_as);
			if (*as == 0) {
				log_peer_warnx(&peer->conf,
				    "peer requests unacceptable AS %u", *as);
				session_notification(peer, ERR_OPEN,
				    ERR_OPEN_AS, NULL);
				change_state(peer, STATE_IDLE, EVNT_RCVD_OPEN);
				return (-1);
			}
			peer->capa.peer.as4byte = 1;
			break;
		case CAPA_ADD_PATH:
			if (capa_len % 4 != 0) {
				log_peer_warnx(&peer->conf,
				    "Bad ADD-PATH capability length: "
				    "%u", capa_len);
				memset(peer->capa.peer.add_path, 0,
				    sizeof(peer->capa.peer.add_path));
				break;
			}
			for (i = 0; i <= capa_len - 4; i += 4) {
				memcpy(&afi, capa_val + i, sizeof(afi));
				afi = ntohs(afi);
				safi = capa_val[i + 2];
				flags = capa_val[i + 3];
				if (afi2aid(afi, safi, &aid) == -1) {
					log_peer_warnx(&peer->conf,
					    "Received ADD-PATH capa: "
					    " unknown AFI %u, safi %u pair",
					    afi, safi);
					memset(peer->capa.peer.add_path, 0,
					    sizeof(peer->capa.peer.add_path));
					break;
				}
				if (flags & ~CAPA_AP_BIDIR) {
					log_peer_warnx(&peer->conf,
					    "Received ADD-PATH capa: "
					    " bad flags %x", flags);
					memset(peer->capa.peer.add_path, 0,
					    sizeof(peer->capa.peer.add_path));
					break;
				}
				peer->capa.peer.add_path[aid] = flags;
			}
			break;
		case CAPA_ENHANCED_RR:
			peer->capa.peer.enhanced_rr = 1;
			break;
		default:
			break;
		}
	}

	return (0);
}

int
capa_neg_calc(struct peer *p, uint8_t *suberr)
{
	uint8_t	i, hasmp = 0;

	/* a capability is accepted only if both sides announced it */

	p->capa.neg.refresh =
	    (p->capa.ann.refresh && p->capa.peer.refresh) != 0;
	p->capa.neg.enhanced_rr =
	    (p->capa.ann.enhanced_rr && p->capa.peer.enhanced_rr) != 0;
	p->capa.neg.as4byte =
	    (p->capa.ann.as4byte && p->capa.peer.as4byte) != 0;

	/* MP: both side must agree on the AFI,SAFI pair */
	for (i = AID_MIN; i < AID_MAX; i++) {
		if (p->capa.ann.mp[i] && p->capa.peer.mp[i])
			p->capa.neg.mp[i] = 1;
		else
			p->capa.neg.mp[i] = 0;
		if (p->capa.ann.mp[i])
			hasmp = 1;
	}
	/* if no MP capability present default to IPv4 unicast mode */
	if (!hasmp)
		p->capa.neg.mp[AID_INET] = 1;

	/*
	 * graceful restart: the peer capabilities are of interest here.
	 * It is necessary to compare the new values with the previous ones
	 * and act accordingly. AFI/SAFI that are not part in the MP capability
	 * are treated as not being present.
	 * Also make sure that a flush happens if the session stopped
	 * supporting graceful restart.
	 */

	for (i = AID_MIN; i < AID_MAX; i++) {
		int8_t	negflags;

		/* disable GR if the AFI/SAFI is not present */
		if ((p->capa.peer.grestart.flags[i] & CAPA_GR_PRESENT &&
		    p->capa.neg.mp[i] == 0))
			p->capa.peer.grestart.flags[i] = 0;	/* disable */
		/* look at current GR state and decide what to do */
		negflags = p->capa.neg.grestart.flags[i];
		p->capa.neg.grestart.flags[i] = p->capa.peer.grestart.flags[i];
		if (negflags & CAPA_GR_RESTARTING) {
			if (p->capa.ann.grestart.restart != 0 &&
			    p->capa.peer.grestart.flags[i] & CAPA_GR_FORWARD) {
				p->capa.neg.grestart.flags[i] |=
				    CAPA_GR_RESTARTING;
			} else {
				if (imsg_rde(IMSG_SESSION_FLUSH, p->conf.id,
				    &i, sizeof(i)) == -1) {
					log_peer_warnx(&p->conf,
					    "imsg send failed");
					return (-1);
				}
				log_peer_warnx(&p->conf, "graceful restart of "
				    "%s, not restarted, flushing", aid2str(i));
			}
		}
	}
	p->capa.neg.grestart.timeout = p->capa.peer.grestart.timeout;
	p->capa.neg.grestart.restart = p->capa.peer.grestart.restart;
	if (p->capa.ann.grestart.restart == 0)
		p->capa.neg.grestart.restart = 0;

	/*
	 * ADD-PATH: set only those bits where both sides agree.
	 * For this compare our send bit with the recv bit from the peer
	 * and vice versa.
	 * The flags are stored from this systems view point.
	 * At index 0 the flags are set if any per-AID flag is set.
	 */
	memset(p->capa.neg.add_path, 0, sizeof(p->capa.neg.add_path));
	for (i = AID_MIN; i < AID_MAX; i++) {
		if ((p->capa.ann.add_path[i] & CAPA_AP_RECV) &&
		    (p->capa.peer.add_path[i] & CAPA_AP_SEND)) {
			p->capa.neg.add_path[i] |= CAPA_AP_RECV;
			p->capa.neg.add_path[0] |= CAPA_AP_RECV;
		}
		if ((p->capa.ann.add_path[i] & CAPA_AP_SEND) &&
		    (p->capa.peer.add_path[i] & CAPA_AP_RECV)) {
			p->capa.neg.add_path[i] |= CAPA_AP_SEND;
			p->capa.neg.add_path[0] |= CAPA_AP_SEND;
		}
	}

	/*
	 * Open policy: check that the policy is sensible.
	 *
	 * Make sure that the roles match and set the negotiated capability
	 * to the role of the peer. So the RDE can inject the OTC attribute.
	 * See RFC 9234, section 4.2.
	 * These checks should only happen on ebgp sessions.
	 */
	if (p->capa.ann.policy != 0 && p->capa.peer.policy != 0 &&
	    p->conf.ebgp) {
		switch (p->conf.role) {
		case ROLE_PROVIDER:
			if (p->remote_role != ROLE_CUSTOMER)
				goto fail;
			break;
		case ROLE_RS:
			if (p->remote_role != ROLE_RS_CLIENT)
				goto fail;
			break;
		case ROLE_RS_CLIENT:
			if (p->remote_role != ROLE_RS)
				goto fail;
			break;
		case ROLE_CUSTOMER:
			if (p->remote_role != ROLE_PROVIDER)
				goto fail;
			break;
		case ROLE_PEER:
			if (p->remote_role != ROLE_PEER)
				goto fail;
			break;
		default:
 fail:
			log_peer_warnx(&p->conf, "open policy role mismatch: "
			    "our role %s, their role %s",
			    log_policy(p->conf.role),
			    log_policy(p->remote_role));
			*suberr = ERR_OPEN_ROLE;
			return (-1);
		}
		p->capa.neg.policy = 1;
	} else if (p->capa.ann.policy == 2 && p->conf.ebgp) {
		/* enforce presence of open policy role capability */
		log_peer_warnx(&p->conf, "open policy role enforced but "
		    "not present");
		*suberr = ERR_OPEN_ROLE;
		return (-1);
	}

	return (0);
}

void
session_dispatch_imsg(struct imsgbuf *imsgbuf, int idx, u_int *listener_cnt)
{
	struct imsg		 imsg;
	struct ibuf		 ibuf;
	struct mrt		 xmrt;
	struct route_refresh	 rr;
	struct mrt		*mrt;
	struct imsgbuf		*i;
	struct peer		*p;
	struct listen_addr	*la, *next, nla;
	struct session_dependon	 sdon;
	struct bgpd_config	 tconf;
	size_t			 len;
	uint32_t		 peerid;
	int			 n, fd, depend_ok, restricted;
	uint16_t		 t;
	uint8_t			 aid, errcode, subcode;

	while (imsgbuf) {
		if ((n = imsg_get(imsgbuf, &imsg)) == -1)
			fatal("session_dispatch_imsg: imsg_get error");

		if (n == 0)
			break;

		peerid = imsg_get_id(&imsg);
		switch (imsg_get_type(&imsg)) {
		case IMSG_SOCKET_CONN:
		case IMSG_SOCKET_CONN_CTL:
			if (idx != PFD_PIPE_MAIN)
				fatalx("reconf request not from parent");
			if ((fd = imsg_get_fd(&imsg)) == -1) {
				log_warnx("expected to receive imsg fd to "
				    "RDE but didn't receive any");
				break;
			}
			if ((i = malloc(sizeof(struct imsgbuf))) == NULL)
				fatal(NULL);
			imsg_init(i, fd);
			if (imsg_get_type(&imsg) == IMSG_SOCKET_CONN) {
				if (ibuf_rde) {
					log_warnx("Unexpected imsg connection "
					    "to RDE received");
					msgbuf_clear(&ibuf_rde->w);
					free(ibuf_rde);
				}
				ibuf_rde = i;
			} else {
				if (ibuf_rde_ctl) {
					log_warnx("Unexpected imsg ctl "
					    "connection to RDE received");
					msgbuf_clear(&ibuf_rde_ctl->w);
					free(ibuf_rde_ctl);
				}
				ibuf_rde_ctl = i;
			}
			break;
		case IMSG_RECONF_CONF:
			if (idx != PFD_PIPE_MAIN)
				fatalx("reconf request not from parent");
			if (imsg_get_data(&imsg, &tconf, sizeof(tconf)) == -1)
				fatal("imsg_get_data");

			nconf = new_config();
			copy_config(nconf, &tconf);
			pending_reconf = 1;
			break;
		case IMSG_RECONF_PEER:
			if (idx != PFD_PIPE_MAIN)
				fatalx("reconf request not from parent");
			if ((p = calloc(1, sizeof(struct peer))) == NULL)
				fatal("new_peer");
			if (imsg_get_data(&imsg, &p->conf, sizeof(p->conf)) ==
			    -1)
				fatal("imsg_get_data");
			p->state = p->prev_state = STATE_NONE;
			p->reconf_action = RECONF_REINIT;
			if (RB_INSERT(peer_head, &nconf->peers, p) != NULL)
				fatalx("%s: peer tree is corrupt", __func__);
			break;
		case IMSG_RECONF_LISTENER:
			if (idx != PFD_PIPE_MAIN)
				fatalx("reconf request not from parent");
			if (nconf == NULL)
				fatalx("IMSG_RECONF_LISTENER but no config");
			if (imsg_get_data(&imsg, &nla, sizeof(nla)) == -1)
				fatal("imsg_get_data");
			TAILQ_FOREACH(la, conf->listen_addrs, entry)
				if (!la_cmp(la, &nla))
					break;

			if (la == NULL) {
				if (nla.reconf != RECONF_REINIT)
					fatalx("king bula sez: "
					    "expected REINIT");

				if ((nla.fd = imsg_get_fd(&imsg)) == -1)
					log_warnx("expected to receive fd for "
					    "%s but didn't receive any",
					    log_sockaddr((struct sockaddr *)
					    &nla.sa, nla.sa_len));

				la = calloc(1, sizeof(struct listen_addr));
				if (la == NULL)
					fatal(NULL);
				memcpy(&la->sa, &nla.sa, sizeof(la->sa));
				la->flags = nla.flags;
				la->fd = nla.fd;
				la->reconf = RECONF_REINIT;
				TAILQ_INSERT_TAIL(nconf->listen_addrs, la,
				    entry);
			} else {
				if (nla.reconf != RECONF_KEEP)
					fatalx("king bula sez: expected KEEP");
				la->reconf = RECONF_KEEP;
			}

			break;
		case IMSG_RECONF_CTRL:
			if (idx != PFD_PIPE_MAIN)
				fatalx("reconf request not from parent");

			if (imsg_get_data(&imsg, &restricted,
			    sizeof(restricted)) == -1)
				fatal("imsg_get_data");
			if ((fd = imsg_get_fd(&imsg)) == -1) {
				log_warnx("expected to receive fd for control "
				    "socket but didn't receive any");
				break;
			}
			if (restricted) {
				control_shutdown(rcsock);
				rcsock = fd;
			} else {
				control_shutdown(csock);
				csock = fd;
			}
			break;
		case IMSG_RECONF_DRAIN:
			switch (idx) {
			case PFD_PIPE_ROUTE:
				if (nconf != NULL)
					fatalx("got unexpected %s from RDE",
					    "IMSG_RECONF_DONE");
				imsg_compose(ibuf_main, IMSG_RECONF_DONE, 0, 0,
				    -1, NULL, 0);
				break;
			case PFD_PIPE_MAIN:
				if (nconf == NULL)
					fatalx("got unexpected %s from parent",
					    "IMSG_RECONF_DONE");
				imsg_compose(ibuf_main, IMSG_RECONF_DRAIN, 0, 0,
				    -1, NULL, 0);
				break;
			default:
				fatalx("reconf request not from parent or RDE");
			}
			break;
		case IMSG_RECONF_DONE:
			if (idx != PFD_PIPE_MAIN)
				fatalx("reconf request not from parent");
			if (nconf == NULL)
				fatalx("got IMSG_RECONF_DONE but no config");
			copy_config(conf, nconf);
			merge_peers(conf, nconf);

			/* delete old listeners */
			TAILQ_FOREACH_SAFE(la, conf->listen_addrs, entry,
			    next) {
				if (la->reconf == RECONF_NONE) {
					log_info("not listening on %s any more",
					    log_sockaddr((struct sockaddr *)
					    &la->sa, la->sa_len));
					TAILQ_REMOVE(conf->listen_addrs, la,
					    entry);
					close(la->fd);
					free(la);
				}
			}

			/* add new listeners */
			TAILQ_CONCAT(conf->listen_addrs, nconf->listen_addrs,
			    entry);

			setup_listeners(listener_cnt);
			free_config(nconf);
			nconf = NULL;
			pending_reconf = 0;
			log_info("SE reconfigured");
			/*
			 * IMSG_RECONF_DONE is sent when the RDE drained
			 * the peer config sent in merge_peers().
			 */
			break;
		case IMSG_SESSION_DEPENDON:
			if (idx != PFD_PIPE_MAIN)
				fatalx("IFINFO message not from parent");
			if (imsg_get_data(&imsg, &sdon, sizeof(sdon)) == -1)
				fatalx("DEPENDON imsg with wrong len");
			depend_ok = sdon.depend_state;

			RB_FOREACH(p, peer_head, &conf->peers)
				if (!strcmp(p->conf.if_depend, sdon.ifname)) {
					if (depend_ok && !p->depend_ok) {
						p->depend_ok = depend_ok;
						bgp_fsm(p, EVNT_START);
					} else if (!depend_ok && p->depend_ok) {
						p->depend_ok = depend_ok;
						session_stop(p,
						    ERR_CEASE_OTHER_CHANGE);
					}
				}
			break;
		case IMSG_MRT_OPEN:
		case IMSG_MRT_REOPEN:
			if (idx != PFD_PIPE_MAIN)
				fatalx("mrt request not from parent");
			if (imsg_get_data(&imsg, &xmrt, sizeof(xmrt)) == -1) {
				log_warnx("mrt open, wrong imsg len");
				break;
			}

			if ((xmrt.wbuf.fd = imsg_get_fd(&imsg)) == -1) {
				log_warnx("expected to receive fd for mrt dump "
				    "but didn't receive any");
				break;
			}

			mrt = mrt_get(&mrthead, &xmrt);
			if (mrt == NULL) {
				/* new dump */
				mrt = calloc(1, sizeof(struct mrt));
				if (mrt == NULL)
					fatal("session_dispatch_imsg");
				memcpy(mrt, &xmrt, sizeof(struct mrt));
				TAILQ_INIT(&mrt->wbuf.bufs);
				LIST_INSERT_HEAD(&mrthead, mrt, entry);
			} else {
				/* old dump reopened */
				close(mrt->wbuf.fd);
				mrt->wbuf.fd = xmrt.wbuf.fd;
			}
			break;
		case IMSG_MRT_CLOSE:
			if (idx != PFD_PIPE_MAIN)
				fatalx("mrt request not from parent");
			if (imsg_get_data(&imsg, &xmrt, sizeof(xmrt)) == -1) {
				log_warnx("mrt close, wrong imsg len");
				break;
			}

			mrt = mrt_get(&mrthead, &xmrt);
			if (mrt != NULL)
				mrt_done(mrt);
			break;
		case IMSG_CTL_KROUTE:
		case IMSG_CTL_KROUTE_ADDR:
		case IMSG_CTL_SHOW_NEXTHOP:
		case IMSG_CTL_SHOW_INTERFACE:
		case IMSG_CTL_SHOW_FIB_TABLES:
		case IMSG_CTL_SHOW_RTR:
		case IMSG_CTL_SHOW_TIMER:
			if (idx != PFD_PIPE_MAIN)
				fatalx("ctl kroute request not from parent");
			control_imsg_relay(&imsg, NULL);
			break;
		case IMSG_CTL_SHOW_NEIGHBOR:
			if (idx != PFD_PIPE_ROUTE_CTL)
				fatalx("ctl rib request not from RDE");
			p = getpeerbyid(conf, peerid);
			control_imsg_relay(&imsg, p);
			break;
		case IMSG_CTL_SHOW_RIB:
		case IMSG_CTL_SHOW_RIB_PREFIX:
		case IMSG_CTL_SHOW_RIB_COMMUNITIES:
		case IMSG_CTL_SHOW_RIB_ATTR:
		case IMSG_CTL_SHOW_RIB_MEM:
		case IMSG_CTL_SHOW_NETWORK:
		case IMSG_CTL_SHOW_FLOWSPEC:
		case IMSG_CTL_SHOW_SET:
			if (idx != PFD_PIPE_ROUTE_CTL)
				fatalx("ctl rib request not from RDE");
			control_imsg_relay(&imsg, NULL);
			break;
		case IMSG_CTL_END:
		case IMSG_CTL_RESULT:
			control_imsg_relay(&imsg, NULL);
			break;
		case IMSG_UPDATE:
			if (idx != PFD_PIPE_ROUTE)
				fatalx("update request not from RDE");
			len = imsg_get_len(&imsg);
			if (imsg_get_ibuf(&imsg, &ibuf) == -1 ||
			    len > MAX_PKTSIZE - MSGSIZE_HEADER ||
			    len < MSGSIZE_UPDATE_MIN - MSGSIZE_HEADER)
				log_warnx("RDE sent invalid update");
			else
				session_update(peerid, &ibuf);
			break;
		case IMSG_UPDATE_ERR:
			if (idx != PFD_PIPE_ROUTE)
				fatalx("update request not from RDE");
			if ((p = getpeerbyid(conf, peerid)) == NULL) {
				log_warnx("no such peer: id=%u", peerid);
				break;
			}
			if (imsg_get_ibuf(&imsg, &ibuf) == -1 ||
			    ibuf_get_n8(&ibuf, &errcode) == -1 ||
			    ibuf_get_n8(&ibuf, &subcode) == -1) {
				log_warnx("RDE sent invalid notification");
				break;
			}

			session_notification(p, errcode, subcode, &ibuf);
			switch (errcode) {
			case ERR_CEASE:
				switch (subcode) {
				case ERR_CEASE_MAX_PREFIX:
				case ERR_CEASE_MAX_SENT_PREFIX:
					t = p->conf.max_out_prefix_restart;
					if (subcode == ERR_CEASE_MAX_PREFIX)
						t = p->conf.max_prefix_restart;

					bgp_fsm(p, EVNT_STOP);
					if (t)
						timer_set(&p->timers,
						    Timer_IdleHold, 60 * t);
					break;
				default:
					bgp_fsm(p, EVNT_CON_FATAL);
					break;
				}
				break;
			default:
				bgp_fsm(p, EVNT_CON_FATAL);
				break;
			}
			break;
		case IMSG_REFRESH:
			if (idx != PFD_PIPE_ROUTE)
				fatalx("route refresh request not from RDE");
			if (imsg_get_data(&imsg, &rr, sizeof(rr)) == -1) {
				log_warnx("RDE sent invalid refresh msg");
				break;
			}
			if ((p = getpeerbyid(conf, peerid)) == NULL) {
				log_warnx("no such peer: id=%u", peerid);
				break;
			}
			if (rr.aid < AID_MIN || rr.aid >= AID_MAX)
				fatalx("IMSG_REFRESH: bad AID");
			session_rrefresh(p, rr.aid, rr.subtype);
			break;
		case IMSG_SESSION_RESTARTED:
			if (idx != PFD_PIPE_ROUTE)
				fatalx("session restart not from RDE");
			if (imsg_get_data(&imsg, &aid, sizeof(aid)) == -1) {
				log_warnx("RDE sent invalid restart msg");
				break;
			}
			if ((p = getpeerbyid(conf, peerid)) == NULL) {
				log_warnx("no such peer: id=%u", peerid);
				break;
			}
			if (aid < AID_MIN || aid >= AID_MAX)
				fatalx("IMSG_SESSION_RESTARTED: bad AID");
			if (p->capa.neg.grestart.flags[aid] &
			    CAPA_GR_RESTARTING) {
				log_peer_warnx(&p->conf,
				    "graceful restart of %s finished",
				    aid2str(aid));
				p->capa.neg.grestart.flags[aid] &=
				    ~CAPA_GR_RESTARTING;
				timer_stop(&p->timers, Timer_RestartTimeout);

				/* signal back to RDE to cleanup stale routes */
				if (imsg_rde(IMSG_SESSION_RESTARTED,
				    peerid, &aid, sizeof(aid)) == -1)
					fatal("imsg_compose: "
					    "IMSG_SESSION_RESTARTED");
			}
			break;
		default:
			break;
		}
		imsg_free(&imsg);
	}
}

int
la_cmp(struct listen_addr *a, struct listen_addr *b)
{
	struct sockaddr_in	*in_a, *in_b;
	struct sockaddr_in6	*in6_a, *in6_b;

	if (a->sa.ss_family != b->sa.ss_family)
		return (1);

	switch (a->sa.ss_family) {
	case AF_INET:
		in_a = (struct sockaddr_in *)&a->sa;
		in_b = (struct sockaddr_in *)&b->sa;
		if (in_a->sin_addr.s_addr != in_b->sin_addr.s_addr)
			return (1);
		if (in_a->sin_port != in_b->sin_port)
			return (1);
		break;
	case AF_INET6:
		in6_a = (struct sockaddr_in6 *)&a->sa;
		in6_b = (struct sockaddr_in6 *)&b->sa;
		if (memcmp(&in6_a->sin6_addr, &in6_b->sin6_addr,
		    sizeof(struct in6_addr)))
			return (1);
		if (in6_a->sin6_port != in6_b->sin6_port)
			return (1);
		break;
	default:
		fatal("king bula sez: unknown address family");
		/* NOTREACHED */
	}

	return (0);
}

struct peer *
getpeerbydesc(struct bgpd_config *c, const char *descr)
{
	struct peer	*p, *res = NULL;
	int		 match = 0;

	RB_FOREACH(p, peer_head, &c->peers)
		if (!strcmp(p->conf.descr, descr)) {
			res = p;
			match++;
		}

	if (match > 1)
		log_info("neighbor description \"%s\" not unique, request "
		    "aborted", descr);

	if (match == 1)
		return (res);
	else
		return (NULL);
}

struct peer *
getpeerbyip(struct bgpd_config *c, struct sockaddr *ip)
{
	struct bgpd_addr addr;
	struct peer	*p, *newpeer, *loose = NULL;
	uint32_t	 id;

	sa2addr(ip, &addr, NULL);

	/* we might want a more effective way to find peers by IP */
	RB_FOREACH(p, peer_head, &c->peers)
		if (!p->conf.template &&
		    !memcmp(&addr, &p->conf.remote_addr, sizeof(addr)))
			return (p);

	/* try template matching */
	RB_FOREACH(p, peer_head, &c->peers)
		if (p->conf.template &&
		    p->conf.remote_addr.aid == addr.aid &&
		    session_match_mask(p, &addr))
			if (loose == NULL || loose->conf.remote_masklen <
			    p->conf.remote_masklen)
				loose = p;

	if (loose != NULL) {
		/* clone */
		if ((newpeer = malloc(sizeof(struct peer))) == NULL)
			fatal(NULL);
		memcpy(newpeer, loose, sizeof(struct peer));
		for (id = PEER_ID_DYN_MAX; id > PEER_ID_STATIC_MAX; id--) {
			if (getpeerbyid(c, id) == NULL)	/* we found a free id */
				break;
		}
		newpeer->template = loose;
		session_template_clone(newpeer, ip, id, 0);
		newpeer->state = newpeer->prev_state = STATE_NONE;
		newpeer->reconf_action = RECONF_KEEP;
		newpeer->rbuf = NULL;
		newpeer->rpending = 0;
		init_peer(newpeer);
		bgp_fsm(newpeer, EVNT_START);
		if (RB_INSERT(peer_head, &c->peers, newpeer) != NULL)
			fatalx("%s: peer tree is corrupt", __func__);
		return (newpeer);
	}

	return (NULL);
}

struct peer *
getpeerbyid(struct bgpd_config *c, uint32_t peerid)
{
	static struct peer lookup;

	lookup.conf.id = peerid;

	return RB_FIND(peer_head, &c->peers, &lookup);
}

int
peer_matched(struct peer *p, struct ctl_neighbor *n)
{
	char *s;

	if (n && n->addr.aid) {
		if (memcmp(&p->conf.remote_addr, &n->addr,
		    sizeof(p->conf.remote_addr)))
			return 0;
	} else if (n && n->descr[0]) {
		s = n->is_group ? p->conf.group : p->conf.descr;
		/* cannot trust n->descr to be properly terminated */
		if (strncmp(s, n->descr, sizeof(n->descr)))
			return 0;
	}
	return 1;
}

void
session_template_clone(struct peer *p, struct sockaddr *ip, uint32_t id,
    uint32_t as)
{
	struct bgpd_addr	remote_addr;

	if (ip)
		sa2addr(ip, &remote_addr, NULL);
	else
		memcpy(&remote_addr, &p->conf.remote_addr, sizeof(remote_addr));

	memcpy(&p->conf, &p->template->conf, sizeof(struct peer_config));

	p->conf.id = id;

	if (as) {
		p->conf.remote_as = as;
		p->conf.ebgp = (p->conf.remote_as != p->conf.local_as);
		if (!p->conf.ebgp)
			/* force enforce_as off for iBGP sessions */
			p->conf.enforce_as = ENFORCE_AS_OFF;
	}

	memcpy(&p->conf.remote_addr, &remote_addr, sizeof(remote_addr));
	switch (p->conf.remote_addr.aid) {
	case AID_INET:
		p->conf.remote_masklen = 32;
		break;
	case AID_INET6:
		p->conf.remote_masklen = 128;
		break;
	}
	p->conf.template = 0;
}

int
session_match_mask(struct peer *p, struct bgpd_addr *a)
{
	struct bgpd_addr masked;

	applymask(&masked, a, p->conf.remote_masklen);
	if (memcmp(&masked, &p->conf.remote_addr, sizeof(masked)) == 0)
		return (1);
	return (0);
}

void
session_down(struct peer *peer)
{
	memset(&peer->capa.neg, 0, sizeof(peer->capa.neg));
	peer->stats.last_updown = getmonotime();
	/*
	 * session_down is called in the exit code path so check
	 * if the RDE is still around, if not there is no need to
	 * send the message.
	 */
	if (ibuf_rde == NULL)
		return;
	if (imsg_rde(IMSG_SESSION_DOWN, peer->conf.id, NULL, 0) == -1)
		fatalx("imsg_compose error");
}

void
session_up(struct peer *p)
{
	struct session_up	 sup;

	/* clear last errors, now that the session is up */
	p->stats.last_sent_errcode = 0;
	p->stats.last_sent_suberr = 0;
	p->stats.last_rcvd_errcode = 0;
	p->stats.last_rcvd_suberr = 0;
	memset(p->stats.last_reason, 0, sizeof(p->stats.last_reason));

	if (imsg_rde(IMSG_SESSION_ADD, p->conf.id,
	    &p->conf, sizeof(p->conf)) == -1)
		fatalx("imsg_compose error");

	if (p->local.aid == AID_INET) {
		sup.local_v4_addr = p->local;
		sup.local_v6_addr = p->local_alt;
	} else {
		sup.local_v6_addr = p->local;
		sup.local_v4_addr = p->local_alt;
	}
	sup.remote_addr = p->remote;
	sup.if_scope = p->if_scope;

	sup.remote_bgpid = p->remote_bgpid;
	sup.short_as = p->short_as;
	memcpy(&sup.capa, &p->capa.neg, sizeof(sup.capa));
	p->stats.last_updown = getmonotime();
	if (imsg_rde(IMSG_SESSION_UP, p->conf.id, &sup, sizeof(sup)) == -1)
		fatalx("imsg_compose error");
}

int
imsg_ctl_parent(struct imsg *imsg)
{
	return imsg_forward(ibuf_main, imsg);
}

int
imsg_ctl_rde(struct imsg *imsg)
{
	if (ibuf_rde_ctl == NULL)
		return (0);
	/*
	 * Use control socket to talk to RDE to bypass the queue of the
	 * regular imsg socket.
	 */
	return imsg_forward(ibuf_rde_ctl, imsg);
}

int
imsg_ctl_rde_msg(int type, uint32_t peerid, pid_t pid)
{
	if (ibuf_rde_ctl == NULL)
		return (0);

	/*
	 * Use control socket to talk to RDE to bypass the queue of the
	 * regular imsg socket.
	 */
	return imsg_compose(ibuf_rde_ctl, type, peerid, pid, -1, NULL, 0);
}

int
imsg_rde(int type, uint32_t peerid, void *data, uint16_t datalen)
{
	if (ibuf_rde == NULL)
		return (0);

	return imsg_compose(ibuf_rde, type, peerid, 0, -1, data, datalen);
}

void
session_demote(struct peer *p, int level)
{
	struct demote_msg	msg;

	strlcpy(msg.demote_group, p->conf.demote_group,
	    sizeof(msg.demote_group));
	msg.level = level;
	if (imsg_compose(ibuf_main, IMSG_DEMOTE, p->conf.id, 0, -1,
	    &msg, sizeof(msg)) == -1)
		fatalx("imsg_compose error");

	p->demoted += level;
}

void
session_stop(struct peer *peer, uint8_t subcode)
{
	struct ibuf *ibuf;
	char *communication;

	communication = peer->conf.reason;

	ibuf = ibuf_dynamic(0, REASON_LEN);

	if ((subcode == ERR_CEASE_ADMIN_DOWN ||
	    subcode == ERR_CEASE_ADMIN_RESET) &&
	    communication != NULL && *communication != '\0' &&
	    ibuf != NULL) {
		if (ibuf_add_n8(ibuf, strlen(communication)) == -1 ||
		    ibuf_add(ibuf, communication, strlen(communication))) {
			log_peer_warnx(&peer->conf,
			    "trying to send overly long shutdown reason");
			ibuf_free(ibuf);
			ibuf = NULL;
		}
	}
	switch (peer->state) {
	case STATE_OPENSENT:
	case STATE_OPENCONFIRM:
	case STATE_ESTABLISHED:
		session_notification(peer, ERR_CEASE, subcode, ibuf);
		break;
	default:
		/* session not open, no need to send notification */
		break;
	}
	ibuf_free(ibuf);
	bgp_fsm(peer, EVNT_STOP);
}

void
merge_peers(struct bgpd_config *c, struct bgpd_config *nc)
{
	struct peer *p, *np, *next;

	RB_FOREACH(p, peer_head, &c->peers) {
		/* templates are handled specially */
		if (p->template != NULL)
			continue;
		np = getpeerbyid(nc, p->conf.id);
		if (np == NULL) {
			p->reconf_action = RECONF_DELETE;
			continue;
		}

		/* peer no longer uses TCP MD5SIG so deconfigure */
		if (p->conf.auth.method == AUTH_MD5SIG &&
		    np->conf.auth.method != AUTH_MD5SIG)
			tcp_md5_del_listener(c, p);
		else if (np->conf.auth.method == AUTH_MD5SIG)
			tcp_md5_add_listener(c, np);

		memcpy(&p->conf, &np->conf, sizeof(p->conf));
		RB_REMOVE(peer_head, &nc->peers, np);
		free(np);

		p->reconf_action = RECONF_KEEP;

		/* had demotion, is demoted, demote removed? */
		if (p->demoted && !p->conf.demote_group[0])
			session_demote(p, -1);

		/* if session is not open then refresh pfkey data */
		if (p->state < STATE_OPENSENT && !p->template)
			imsg_compose(ibuf_main, IMSG_PFKEY_RELOAD,
			    p->conf.id, 0, -1, NULL, 0);

		/* sync the RDE in case we keep the peer */
		if (imsg_rde(IMSG_SESSION_ADD, p->conf.id,
		    &p->conf, sizeof(struct peer_config)) == -1)
			fatalx("imsg_compose error");

		/* apply the config to all clones of a template */
		if (p->conf.template) {
			struct peer *xp;
			RB_FOREACH(xp, peer_head, &c->peers) {
				if (xp->template != p)
					continue;
				session_template_clone(xp, NULL, xp->conf.id,
				    xp->conf.remote_as);
				if (imsg_rde(IMSG_SESSION_ADD, xp->conf.id,
				    &xp->conf, sizeof(xp->conf)) == -1)
					fatalx("imsg_compose error");
			}
		}
	}

	if (imsg_rde(IMSG_RECONF_DRAIN, 0, NULL, 0) == -1)
		fatalx("imsg_compose error");

	/* pfkeys of new peers already loaded by the parent process */
	RB_FOREACH_SAFE(np, peer_head, &nc->peers, next) {
		RB_REMOVE(peer_head, &nc->peers, np);
		if (RB_INSERT(peer_head, &c->peers, np) != NULL)
			fatalx("%s: peer tree is corrupt", __func__);
		if (np->conf.auth.method == AUTH_MD5SIG)
			tcp_md5_add_listener(c, np);
	}
}
