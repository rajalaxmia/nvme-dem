/*
 * NVMe over Fabrics Distributed Endpoint Management (NVMe-oF DEM).
 * Copyright (c) 2017-2018 Intel Corporation, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "mongoose.h"
#include "common.h"
#include "curl.h"

#define DEFAULT_HTTP_ROOT	"/"

#define CURL_DEBUG		0

/* needs to be < NVMF_DISC_KATO in connect AND < 2 MIN for upstream target */
#define KEEP_ALIVE_TIMER	100000 /* ms */

static LIST_HEAD(target_list_head);

static struct mg_serve_http_opts	 s_http_server_opts;
static char				*s_http_port = DEFAULT_HTTP_PORT;
int					 stopped;
int					 debug;
struct host_iface			*interfaces;
int					 num_interfaces;
struct list_head			*target_list = &target_list_head;
static pthread_t			*listen_threads;
static int				 signalled;

void shutdown_dem(void)
{
	stopped = 1;
}

static void signal_handler(int sig_num)
{
	signalled = sig_num;

	shutdown_dem();
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
	switch (ev) {
	case MG_EV_HTTP_REQUEST:
		handle_http_request(c, ev_data);
		break;
	case MG_EV_HTTP_CHUNK:
	case MG_EV_ACCEPT:
	case MG_EV_CLOSE:
	case MG_EV_POLL:
	case MG_EV_SEND:
	case MG_EV_RECV:
		break;
	default:
		print_err("unexpected request %d", ev);
	}
}

static int keep_alive_work(struct target *target)
{
	struct discovery_queue	*dq;
	int			 ret;

	if (target->kato_countdown == 0) {
		list_for_each_entry(dq, &target->discovery_queue_list, node) {
			if (!dq->connected)
				continue;

			ret = send_keep_alive(&dq->ep);
			if (ret) {
				print_err("keep alive failed %s",
					  target->alias);
				disconnect_target(&dq->ep, 0);
				dq->connected = 0;
				target->log_page_retry_count = LOG_PAGE_RETRY;
				return ret;
			}
		}

		target->kato_countdown = KEEP_ALIVE_TIMER / IDLE_TIMEOUT;
	} else
		target->kato_countdown--;

	return 0;
}

static void periodic_work(void)
{
	struct target		*target;
	struct discovery_queue	*dq;

	list_for_each_entry(target, target_list, node) {
		if (target->mgmt_mode == IN_BAND_MGMT)
			if (keep_alive_work(target))
				continue;

		if (target->log_page_retry_count)
			if (--target->log_page_retry_count)
				continue;

		target->refresh_countdown--;
		if (target->refresh_countdown)
			continue;

		if (target->mgmt_mode != LOCAL_MGMT)
			get_config(target);

		list_for_each_entry(dq, &target->discovery_queue_list, node) {
			if (connect_target(dq)) {
				print_err("Could not connect to target %s",
					  target->alias);
				target->log_page_retry_count = LOG_PAGE_RETRY;
				continue;
			}

			fetch_log_pages(dq);

			if (target->mgmt_mode != IN_BAND_MGMT)
				disconnect_target(&dq->ep, 0);
		}

		target->refresh_countdown =
			target->refresh * MINUTES / IDLE_TIMEOUT;
	}
}

static void *poll_loop(struct mg_mgr *mgr)
{
	while (!stopped) {
		mg_mgr_poll(mgr, IDLE_TIMEOUT);

		if (!stopped)
			periodic_work();
	}

	mg_mgr_free(mgr);

	return NULL;
}

static int daemonize(void)
{
	pid_t			 pid, sid;

	if (getuid() != 0) {
		print_err("must be root to run demd as a daemon");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		print_err("fork failed %d", pid);
		return pid;
	}

	if (pid) /* if parent, exit to allow child to run as daemon */
		exit(0);

	umask(0022);

	sid = setsid();
	if (sid < 0) {
		print_err("setsid failed %d", sid);
		return sid;
	}

	if ((chdir("/")) < 0) {
		print_err("could not change dir to /");
		return -1;
	}

	freopen("/var/log/dem_debug.log", "a", stdout);
	freopen("/var/log/dem.log", "a", stderr);

	return 0;
}

static void show_help(char *app)
{
#ifdef DEV_DEBUG
	const char		*arg_list = "{-q} {-d}";
#else
	const char              *arg_list = "{-d} {-s}";
#endif

	print_info("Usage: %s %s {-p <port>} {-r <root>} {-c <cert_file>}",
		   app, arg_list);
#ifdef DEV_DEBUG
	print_info("  -q - quite mode, no debug prints");
	print_info("  -d - run as a daemon process (default is standalone)");
#else
	print_info("  -d - enable debug prints in log files");
	print_info("  -s - run as a standalone process (default is daemon)");
#endif
	print_info("  -p - HTTP interface: port (default %s)",
		   DEFAULT_HTTP_PORT);
	print_info("  -r - HTTP interface: root (default %s)",
		   DEFAULT_HTTP_ROOT);
	print_info("  -c - HTTP interface: SSL cert file (defaut no SSL)");
}

static int init_dem(int argc, char *argv[], char **ssl_cert)
{
	int			 opt;
	int			 run_as_daemon;
#ifdef DEV_DEBUG
	const char		*opt_list = "?qdp:r:c:";
#else
	const char		*opt_list = "?dsp:r:c:";
#endif

	*ssl_cert = NULL;

	if (argc > 1 && strcmp(argv[1], "--help") == 0)
		goto help;

#ifdef DEV_DEBUG
	debug = 1;
	run_as_daemon = 0;
#else
	debug = 0;
	run_as_daemon = 1;
#endif

	while ((opt = getopt(argc, argv, opt_list)) != -1) {
		switch (opt) {
#ifdef DEV_DEBUG
		case 'q':
			debug = 0;
			break;
		case 'd':
			run_as_daemon = 1;
			break;
#else
		case 'd':
			debug = 1;
			break;
		case 's':
			run_as_daemon = 0;
			break;
#endif
		case 'r':
			s_http_server_opts.document_root = optarg;
			break;
		case 'p':
			s_http_port = optarg;
			print_info("Using port %s", s_http_port);
			break;
		case 'c':
			*ssl_cert = optarg;
			break;
		case '?':
		default:
help:
			show_help(argv[0]);
			return 1;
		}
	}

	if (run_as_daemon) {
		if (daemonize())
			return 1;
	}

	return 0;
}

static int init_mg_mgr(struct mg_mgr *mgr, char *prog, char *ssl_cert)
{
	struct mg_bind_opts	 bind_opts;
	struct mg_connection	*c;
	char			*cp;
	const char		*err_str;

	mg_mgr_init(mgr, NULL);

	/* Use current binary directory as document root */
	if (!s_http_server_opts.document_root) {
		cp = strrchr(prog, DIRSEP);
		if (cp != NULL) {
			*cp = '\0';
			s_http_server_opts.document_root = prog;
		}
	}

	/* Set HTTP server options */
	memset(&bind_opts, 0, sizeof(bind_opts));
	bind_opts.error_string = &err_str;

#ifdef SSL_CERT
	if (ssl_cert != NULL)
		bind_opts.ssl_cert = ssl_cert;
#else
	(void) ssl_cert;
#endif

	c = mg_bind_opt(mgr, s_http_port, ev_handler, bind_opts);
	if (c == NULL) {
		print_err("failed to start server on port %s: %s",
			  s_http_port, *bind_opts.error_string);
		return 1;
	}

	mg_set_protocol_http_websocket(c);

	return 0;
}

static void cleanup_threads(pthread_t *listen_threads)
{
	int			i;

	for (i = 0; i < num_interfaces; i++)
		pthread_kill(listen_threads[i], SIGTERM);

	/* wait for threads to cleanup before exiting so they can properly
	 * cleanup.
	 */
	i = 100;

	while (num_interfaces && i--)
		usleep(100);

	/* even thought the threads are finished, need to call join
	 * otherwize, it will not release its memory and valgrind indicates
	 * a leak
	 */

	for (i = 0; i < num_interfaces; i++)
		pthread_join(listen_threads[i], NULL);

	free(listen_threads);
}

static int init_interface_threads(pthread_t **listen_threads)
{
	pthread_attr_t		 pthread_attr;
	pthread_t		*pthreads;
	int			 i;

	pthreads = calloc(num_interfaces, sizeof(pthread_t));
	if (!pthreads)
		return -ENOMEM;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	pthread_attr_init(&pthread_attr);

	for (i = 0; i < num_interfaces; i++) {
		if (pthread_create(&pthreads[i], &pthread_attr,
				   interface_thread, &(interfaces[i]))) {
			print_err("failed to start transport thread");
			free(pthreads);
			return 1;
		}
		usleep(25); // allow new thread to start
	}

	*listen_threads = pthreads;

	return 0;
}

static inline int check_logpage_portid(struct subsystem *subsys,
				       struct portid *portid)
{
	struct logpage		*logpage;

	list_for_each_entry(logpage, &subsys->logpage_list, node)
		if (logpage->portid == portid)
			return 1;
	return 0;
}

static void init_discovery_queue(struct target *target, struct portid *portid)
{
	struct discovery_queue	*dq;
	struct subsystem	*subsys;
	struct host		*host;
	uuid_t			 id;
	char			 uuid[40];

	while (1) {
		list_for_each_entry(subsys, &target->subsys_list, node) {
			if (check_logpage_portid(subsys, portid))
				continue;
			dq = malloc(sizeof(*dq));
			if (!dq) {
				print_err("failed to malloc dq");
				return;
			}

			memset(dq, 0, sizeof(*dq));

			dq->portid = portid;
			dq->target = target;

			if (strcmp(dq->portid->type, "rdma") == 0)
				dq->ep.ops = rdma_register_ops();
			else {
				free(dq);
				continue;
			}

			list_add_tail(&dq->node, &target->discovery_queue_list);

			if (subsys->access == ALLOW_ANY) {
				uuid_generate(id);
				uuid_unparse_lower(id, uuid);
				sprintf(dq->hostnqn, NVMF_UUID_FMT, uuid);
			} else {
				host = list_first_entry(&subsys->host_list,
							struct host, node);
				sprintf(dq->hostnqn, host->nqn);
			}

			if (connect_target(dq))
				continue;

			fetch_log_pages(dq);

			if (target->mgmt_mode != IN_BAND_MGMT)
				disconnect_target(&dq->ep, 0);
			else
				dq->connected = 1;

			continue;
		}
		break;
	}
}

void init_targets(void)
{
	struct target		*target;
	struct portid		*portid;

	build_target_list();

	list_for_each_entry(target, target_list, node) {
		target->refresh_countdown =
			target->refresh * MINUTES / IDLE_TIMEOUT;

		target->log_page_retry_count = LOG_PAGE_RETRY;

		if (target->mgmt_mode != LOCAL_MGMT)
			if (!get_config(target))
				config_target(target);

		list_for_each_entry(portid, &target->portid_list, node) {
			init_discovery_queue(target, portid);
		}
	}
}

void cleanup_targets(void)
{
	struct target		*target, *next_target;
	struct subsystem	*subsys, *next_subsys;
	struct host		*host, *next_host;
	struct logpage		*logpage, *next_logpage;
	struct discovery_queue	*dq, *next_dq;

	list_for_each_entry_safe(target, next_target, target_list, node) {
		list_for_each_entry_safe(subsys, next_subsys,
					 &target->subsys_list, node) {
			list_for_each_entry_safe(host, next_host,
						 &subsys->host_list, node)
				free(host);
			list_for_each_entry_safe(logpage, next_logpage,
						 &subsys->logpage_list, node)
				free(logpage);

			free(subsys);
		}

		list_del(&target->node);

		list_for_each_entry_safe(dq, next_dq,
					 &target->discovery_queue_list, node) {
			if (dq->connected)
				disconnect_target(&dq->ep, 0);
			free(dq);
		}

		if (target->mgmt_mode == IN_BAND_MGMT &&
		    target->sc_iface.inb.connected)
			disconnect_target(&target->sc_iface.inb.ep, 0);

		free(target);
	}
}

static void set_signature(void)
{
	FILE			*fd;
	char			 buf[256];
	char			*sig;
	int			 len;

	fd = fopen(SIGNATURE_FILE, "r");
	if (!fd)
		return;

	strcpy(buf, "Basic ");
	fgets(buf + 6, sizeof(buf) - 1, fd);
	len = strlen(buf);
	if (buf[len - 1] == '\n')
		buf[len--] = 0;

	sig = malloc(len + 1);
	if (!sig)
		return;
	strcpy(sig, buf);

	s_signature_user = mg_mk_str_n(sig, len);
	s_signature = &s_signature_user;

	fclose(fd);
}

int main(int argc, char *argv[])
{
	struct mg_mgr		 mgr;
	char			*ssl_cert = NULL;
	char			 default_root[] = DEFAULT_HTTP_ROOT;
	int			 ret = 1;

	s_http_server_opts.document_root = default_root;

	if (init_dem(argc, argv, &ssl_cert))
		goto out;

	if (init_mg_mgr(&mgr, argv[0], ssl_cert))
		goto out;

	if (init_curl(CURL_DEBUG))
		goto out;

	if (init_json(CONFIG_FILE))
		goto out1;

	set_signature();

	num_interfaces = init_interfaces();
	if (num_interfaces <= 0)
		goto out2;

	init_targets();

	signalled = stopped = 0;

	print_info("Starting server on port %s, serving '%s'",
		   s_http_port, s_http_server_opts.document_root);

	if (init_interface_threads(&listen_threads))
		goto out3;

	poll_loop(&mgr);

	cleanup_threads(listen_threads);

	if (signalled)
		printf("\n");

	ret = 0;
out3:
	free(interfaces);
	cleanup_targets();
out2:
	if (s_signature == &s_signature_user)
		free((char *) s_signature->p);

	cleanup_json();
out1:
	cleanup_curl();
out:
	return ret;
}
