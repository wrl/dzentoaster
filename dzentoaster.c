/*
 * This file is part of dzentoaster.
 * dzentoaster is copyright 2008 william light <visinin@gmail.com>
 *
 * dzentoaster is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details.
 *
 */

/* for asprintf */
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/msg.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <pwd.h>

#include "config.h"

#define IPC_NAME "dzentoaster"
#define MAX_MSG_SIZE 8192

enum {
	CLIENT,
	SERVER
} mode = CLIENT;

typedef struct {
	char *formatstring;
	ssize_t len;
	struct timespec expiration_time;
} bread_slice_t;

const struct passwd *passwd;

bread_slice_t **bread_slices;
bread_slice_t *untoasted = NULL;
int slices_in_toaster = 0;

int slice_quantity = DEFAULT_TOASTER_ITEMS;
int fifo_fd = -1;

pthread_mutex_t slices_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t slice_needs_toasting = PTHREAD_COND_INITIALIZER;

pid_t dzen_pid;
FILE *to_dzen;
int dzen_stdin;

void run_dzen() {
	int pipefd[2];
	
	pipe(pipefd);
	
	if( !(dzen_pid = fork()) ) {
		close(pipefd[1]);
		dup2(pipefd[0], 0);
		
		execvp(dzen_cmd[0], dzen_cmd);
	} else {
		close(pipefd[0]);
		
		dzen_stdin = pipefd[1];
		to_dzen = fdopen(dzen_stdin, "w");

		setlinebuf(to_dzen);
		fprintf(to_dzen, "^hide()\n");
	}
}

void bread_slice_pop() {
	bread_slice_t *s = bread_slices[0];
	int i;
	
	if( !s )
		return;
	
	for( i = 0; i < slices_in_toaster; i++ ) {
		if( i >= slice_quantity )
			bread_slices[i] = NULL;
		else
			bread_slices[i] = bread_slices[i + 1];
	}
	
	free(s->formatstring);
	free(s);
	
	slices_in_toaster--;
}

void bread_slice_push_untoasted() {
	while( slices_in_toaster >= slice_quantity )
		bread_slice_pop();
	
	bread_slices[slices_in_toaster] = untoasted;
	untoasted = NULL;
	slices_in_toaster++;
}

void bread_slice_stage(const char *formatstring, ssize_t len) {
	bread_slice_t *s = calloc(1, sizeof(bread_slice_t));

	s->formatstring = strndup(formatstring, len);
	s->len = len;
	
	clock_gettime(CLOCK_REALTIME, &s->expiration_time);
	s->expiration_time.tv_sec += BREAD_SLICE_TTL_SECONDS;
	
	if( untoasted ) {
		/* this shouldn't happen because of the way that 
		   slices_mutex marshalls access to untoasted */
		
		free(untoasted->formatstring);
		free(untoasted);
	}
	
	untoasted = s;
}

void *server_thread(void *arg) {
	bread_slice_t *s;
	int i;
	
	while( 1 ) {
		if( bread_slices[0] ) {
			if( pthread_cond_timedwait(&slice_needs_toasting, &slices_mutex, &bread_slices[0]->expiration_time) == ETIMEDOUT )
				bread_slice_pop();
		} else
			pthread_cond_wait(&slice_needs_toasting, &slices_mutex);
		
		if( untoasted )
			bread_slice_push_untoasted();
		
		if( !slices_in_toaster ) {
			fprintf(to_dzen, "^hide()\n");
		} else {
			fprintf(to_dzen, "^unhide()\n^cs()\n");

			s = bread_slices[slices_in_toaster - 1];
			fprintf(to_dzen, "^tw()%s\n", s->formatstring);

			if( slices_in_toaster == 1 ) {
				if( UNCOLLAPSE_AUTOMATICALLY )
					fprintf(to_dzen, "^collapse()\n");
			} else {
				if( UNCOLLAPSE_AUTOMATICALLY )
					fprintf(to_dzen, "^uncollapse()\n");
				
				for( i = 0; i < (slice_quantity - slices_in_toaster); i++ ) 
					fprintf(to_dzen, " \n");

				for( i = 0; i < slices_in_toaster - 1; i++ ) {
					s = bread_slices[i];
					
					fprintf(to_dzen, "%s\n", s->formatstring);
				}
			}
		}
	}
}

int write_pid_file() {
	char *pidfile, *pidstr;
	int pidfd, pidlen;
	
	asprintf(&pidfile, "/tmp/." IPC_NAME "-%s.pid", passwd->pw_name);
	pidlen = asprintf(&pidstr, "%d", getpid());

	if( (pidfd = open(pidfile, O_RDWR | O_CREAT | O_EXCL, S_IRWXU)) < 0 ) {
		free(pidfile);
		free(pidstr);
		return 1;
	}
	
	write(pidfd, pidstr, pidlen);
	close(pidfd);
	
	free(pidstr);
	free(pidfile);
	
	return 0;
}

void remove_pid_file() {
	char *pidfile;

	asprintf(&pidfile, "/tmp/." IPC_NAME "-%s.pid", passwd->pw_name);
	unlink(pidfile);
	free(pidfile);
}

int open_fifo() {
	char *fifo;
	int err;

	asprintf(&fifo, "/tmp/." IPC_NAME "-%s", passwd->pw_name);
	
	if( mode == SERVER ) {
		if( (err = mkfifo(fifo, S_IRWXU) < 0) ) {
			free(fifo);
			return 1;
		}
	}
	
	if( (fifo_fd = open(fifo, O_RDWR)) < 0 ) {
		free(fifo);
		return 1;
	}
	
	free(fifo);
	return 0;
}

void remove_fifo() {
	char *fifo;

	asprintf(&fifo, "/tmp/." IPC_NAME "-%s", passwd->pw_name);
	unlink(fifo);
	free(fifo);
}

void read_from_fifo() {
	char buf[MAX_MSG_SIZE + 1];
	ssize_t n;
	
	while( (n = read(fifo_fd, buf, MAX_MSG_SIZE)) ) {
		buf[n] = 0;

		pthread_mutex_lock(&slices_mutex);
		
		bread_slice_stage(buf, n);
		pthread_cond_signal(&slice_needs_toasting);
		
		pthread_mutex_unlock(&slices_mutex);
	}
}

void write_to_fifo() {
	char buf[MAX_MSG_SIZE];
	ssize_t n;
	
	while( (n = read(0, buf, MAX_MSG_SIZE)) )
		write(fifo_fd, buf, n);
}

void usage(const char *progname) {
	fprintf(stderr,
			"Usage: %s [-d [-n popups]]\n"
			"	-d 	start the %s server and listen for notifications\n"
			"	-n 	maximum number of popups to show at a time\n",
			progname, progname);
}

void cleanup() {
	if( fifo_fd < 0 )
		return;
	
	close(fifo_fd);
	
	if( mode == SERVER ) {
		remove_fifo();
		remove_pid_file();
	}
}

void exit_on_signal() {
	/* so that atexit() functions run */
	exit(1);
}

int main(int argc, char **argv) {
	pthread_t dzen_thread;
	int opt;
	
	while( (opt = getopt(argc, argv, "dn:")) != -1 ) {
		switch( opt ) {
		case 'd':
			mode = SERVER;
			break;
			
		case 'n':
			if( (slice_quantity = atoi(optarg)) )
				break;
			
		default:
			usage(argv[0]);
			return(1);
		}
	}
	
	atexit(cleanup);
	signal(SIGINT, exit_on_signal);
	signal(SIGTERM, exit_on_signal);
	
	passwd = getpwuid(getuid());
	
	switch( mode ) {
	case SERVER:
		if( write_pid_file() ) {
			printf("dzentoaster already running\n");
			return 1;
		}
		
		if( open_fifo() ) {
			perror("couldn't open fifo");
			return 1;
		}
		
		bread_slices = calloc(slice_quantity, sizeof(bread_slice_t *));
		pthread_create(&dzen_thread, NULL, server_thread, NULL);
		
		run_dzen();
		read_from_fifo();
		
		break;
		
	case CLIENT:
		if( open_fifo() ) {
			perror("couldn't open fifo");
			printf("\"dzentoaster -d\" probably isn't running.\n");
			return 1;
		}
		
		write_to_fifo();

		break;
	}
	
	return 0;
}
