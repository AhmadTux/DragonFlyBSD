/*
 * Copyright (c) 2005 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * $DragonFly: src/usr.sbin/dntpd/main.c,v 1.3 2005/04/24 23:09:32 dillon Exp $
 */

#include "defs.h"

static void usage(const char *av0);
static void dotest(const char *target);
static void add_server(const char *target);

static struct server_info **servers;
static int nservers;
static int maxservers;

int debug_opt;
int min_sleep_opt = 5;		/* 5 seconds minimum poll interval */
int nom_sleep_opt = 300;	/* 5 minutes nominal poll interval */
int max_sleep_opt = 1800;	/* 30 minutes maximum poll interval */

int
main(int ac, char **av)
{
    int test_opt = 0;
    int rc;
    int ch;
    int i;

    /*
     * Really randomize
     */
    srandomdev();
    rc = 0;

    /*
     * Process Options
     */
    while ((ch = getopt(ac, av, "dtT:L:")) != -1) {
	switch(ch) {
	case 'T':
	    nom_sleep_opt = strtol(optarg, NULL, 0);
	    if (nom_sleep_opt < 1) {
		fprintf(stderr, "Warning: nominal poll interval too small, "
				"limiting to 1 second\n");
		nom_sleep_opt = 1;
	    }
	    if (nom_sleep_opt > 24 * 60 * 60) {
		fprintf(stderr, "Warning: nominal poll interval too large, "
				"limiting to 24 hours\n");
		nom_sleep_opt = 24 * 60 * 60;
	    }
	    if (min_sleep_opt > nom_sleep_opt)
		min_sleep_opt = nom_sleep_opt;
	    if (max_sleep_opt < nom_sleep_opt * 5)
		max_sleep_opt = nom_sleep_opt * 5;
	    break;
	case 'L':
	    max_sleep_opt = strtol(optarg, NULL, 0);
	    break;
	case 'd':
	    debug_opt = 1;
	    break;
	case 't':
	    test_opt = 1;
	    break;
	case 'h':
	default:
	    usage(av[0]);
	    /* not reached */
	}
    }

    if (test_opt) {
	if (optind != ac - 1)
	    usage(av[0]);
	dotest(av[optind]);
	/* not reached */
    }

    /*
     * Add additional hosts.
     */
    for (i = optind; i < ac; ++i) {
	add_server(av[i]);
    }
    if (nservers == 0) {
	usage(av[0]);
	/* not reached */
    }

    /*
     * And go.
     */
    client_init();
    rc = client_main(servers, nservers);
    return(rc);
}

static
void
usage(const char *av0)
{
    fprintf(stderr, "%s [-dt] [-T poll_interval] [additional_targets]\n", av0);
    fprintf(stderr, "\t-d\tforeground operation, debugging turned on\n");
    fprintf(stderr, "\t-t\ttest mode (specify one target on command line)\n");
    exit(1);
}

static
void
dotest(const char *target)
{
    struct server_info info;

    bzero(&info, sizeof(info));
    info.fd = udp_socket(target, 123);
    if (info.fd < 0) {
	logerrstr("unable to create UDP socket for %s", target);
	return;
    }
    info.target = strdup(target);
    client_init();

    debug_opt = 1;

    fprintf(stderr, 
	    "Will run %d-second polls until interrupted.\n", nom_sleep_opt);

    for (;;) {
	client_poll(&info, nom_sleep_opt);
	sleep(nom_sleep_opt);
    }
    /* not reached */
}

static void
add_server(const char *target)
{
    server_info_t info;

    if (nservers == maxservers) {
	maxservers += 16;
	servers = realloc(servers, maxservers * sizeof(server_info_t));
	assert(servers != NULL);
    }
    info = malloc(sizeof(struct server_info));
    servers[nservers] = info;
    bzero(info, sizeof(struct server_info));
    info->fd = udp_socket(target, 123);
    if (info->fd < 0) {
	logerrstr("Unable to add server %s", target);
    } else {
	info->target = strdup(target);
	++nservers;
    }
}

