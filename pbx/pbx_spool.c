/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Full-featured outgoing call spool support
 * 
 * Copyright (C) 2002, Digium
 *
 * Mark Spencer <markster@digium.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#include <asterisk/lock.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/options.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <utime.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "../astconf.h"

/*
 * pbx_spool is similar in spirit to qcall, but with substantially enhanced functionality...
 * The spool file contains a header 
 */

static char *tdesc = "Outgoing Spool Support";
static char qdir[255];

struct outgoing {
	char fn[256];
	/* Current number of retries */
	int retries;
	/* Maximum number of retries permitted */
	int maxretries;
	/* How long to wait between retries (in seconds) */
	int retrytime;
	/* How long to wait for an answer */
	int waittime;
	
	/* What to connect to outgoing */
	char tech[256];
	char dest[256];
	
	/* If application */
	char app[256];
	char data[256];

	/* If extension/context/priority */
	char exten[256];
	char context[256];
	int priority;

	/* CallerID Information */
	char callerid[256];

	/* Channel variables */
	char variable[10*256];
	/* Account code */
	char account[256];
	
	/* Maximum length of call */
	int maxlen;
	
};

static void init_outgoing(struct outgoing *o)
{
	memset(o, 0, sizeof(struct outgoing));
	o->priority = 1;
	o->retrytime = 300;
	o->waittime = 45;
}

static int apply_outgoing(struct outgoing *o, char *fn, FILE *f)
{
	char buf[256];
	char *c, *c2;
	int lineno = 0;
	while(!feof(f)) {
		fgets(buf, sizeof(buf), f);
		lineno++;
		if (!feof(f)) {
			/* Trim comments */
			c = strchr(buf, '#');
			if (c)
				 *c = '\0';
			c = strchr(buf, ';');
			if (c)
				 *c = '\0';

			/* Trim trailing white space */
			while(strlen(buf) && buf[strlen(buf) - 1] < 33)
				buf[strlen(buf) - 1] = '\0';
			if (strlen(buf)) {
				c = strchr(buf, ':');
				if (c) {
					*c = '\0';
					c++;
					while(*c < 33)
						c++;
#if 0
					printf("'%s' is '%s' at line %d\n", buf, c, lineno);
#endif					
					if (!strcasecmp(buf, "channel")) {
						strncpy(o->tech, c, sizeof(o->tech) - 1);
						if ((c2 = strchr(o->tech, '/'))) {
							*c2 = '\0';
							c2++;
							strncpy(o->dest, c2, sizeof(o->dest) - 1);
						} else {
							ast_log(LOG_NOTICE, "Channel should be in form Tech/Dest at line %d of %s\n", lineno, fn);
							strcpy(o->tech, "");
						}
					} else if (!strcasecmp(buf, "callerid")) {
						strncpy(o->callerid, c, sizeof(o->callerid) - 1);
					} else if (!strcasecmp(buf, "application")) {
						strncpy(o->app, c, sizeof(o->app) - 1);
					} else if (!strcasecmp(buf, "data")) {
						strncpy(o->data, c, sizeof(o->data) - 1);
					} else if (!strcasecmp(buf, "maxretries")) {
						if (sscanf(c, "%d", &o->maxretries) != 1) {
							ast_log(LOG_WARNING, "Invalid max retries at line %d of %s\n", lineno, fn);
							o->maxretries = 0;
						}
					} else if (!strcasecmp(buf, "context")) {
						strncpy(o->context, c, sizeof(o->context) - 1);
					} else if (!strcasecmp(buf, "extension")) {
						strncpy(o->exten, c, sizeof(o->exten) - 1);
					} else if (!strcasecmp(buf, "priority")) {
						if ((sscanf(c, "%d", &o->priority) != 1) || (o->priority < 1)) {
							ast_log(LOG_WARNING, "Invalid priority at line %d of %s\n", lineno, fn);
							o->priority = 1;
						}
					} else if (!strcasecmp(buf, "retrytime")) {
						if ((sscanf(c, "%d", &o->retrytime) != 1) || (o->retrytime < 1)) {
							ast_log(LOG_WARNING, "Invalid retrytime at line %d of %s\n", lineno, fn);
							o->retrytime = 300;
						}
					} else if (!strcasecmp(buf, "waittime")) {
						if ((sscanf(c, "%d", &o->waittime) != 1) || (o->waittime < 1)) {
							ast_log(LOG_WARNING, "Invalid retrytime at line %d of %s\n", lineno, fn);
							o->waittime = 45;
						}
					} else if (!strcasecmp(buf, "retry")) {
						o->retries++;
					} else if (!strcasecmp(buf, "setvar")) { /* JDG variable support */
						strncat(o->variable, c, sizeof(o->variable) - strlen(o->variable) - 1);
						strncat(o->variable, "|", sizeof(o->variable) - strlen(o->variable) - 1);
 
					} else if (!strcasecmp(buf, "account")) {
						strncpy(o->account, c, sizeof(o->account) - 1);
					} else {
						ast_log(LOG_WARNING, "Unknown keyword '%s' at line %d of %s\n", buf, lineno, fn);
					}
				} else
					ast_log(LOG_NOTICE, "Syntax error at line %d of %s\n", lineno, fn);
			}
		}
	}
	strncpy(o->fn, fn, sizeof(o->fn) - 1);
	/* Check sanity of times */
	if (o->retrytime < o->waittime + 5)
		o->retrytime = o->waittime + 5;
	if (!strlen(o->tech) || !strlen(o->dest) || (!strlen(o->app) && !strlen(o->exten))) {
		ast_log(LOG_WARNING, "At least one of app or extension must be specified, along with tech and dest in file %s\n", fn);
		return -1;
	}
	return 0;
}

static void *attempt_thread(void *data)
{
	struct outgoing *o = data;
	int res, reason;
	if (strlen(o->app)) {
		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_3 "Attempting call on %s/%s for application %s(%s) (Retry %d)\n", o->tech, o->dest, o->app, o->data, o->retries);
		res = ast_pbx_outgoing_app(o->tech, AST_FORMAT_SLINEAR, o->dest, o->waittime * 1000, o->app, o->data, &reason, 2 /* wait to finish */, o->callerid, o->variable, o->account);
	} else {
		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_3 "Attempting call on %s/%s for %s@%s:%d (Retry %d)\n", o->tech, o->dest, o->exten, o->context,o->priority, o->retries);
		res = ast_pbx_outgoing_exten(o->tech, AST_FORMAT_SLINEAR, o->dest, o->waittime * 1000, o->context, o->exten, o->priority, &reason, 2 /* wait to finish */, o->callerid, o->variable, o->account);
	}
	if (res) {
		ast_log(LOG_NOTICE, "Call failed to go through, reason %d\n", reason);
		if (o->retries >= o->maxretries + 1) {
			/* Max retries exceeded */
			ast_log(LOG_EVENT, "Queued call to %s/%s expired without completion after %d attempt(s)\n", o->tech, o->dest, o->retries - 1);
			unlink(o->fn);
		}
	} else {
		ast_log(LOG_NOTICE, "Call completed to %s/%s\n", o->tech, o->dest);
		ast_log(LOG_EVENT, "Queued call to %s/%s completed\n", o->tech, o->dest);
		unlink(o->fn);
	}
	free(o);
	return NULL;
}

static void launch_service(struct outgoing *o)
{
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
 	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t,&attr,attempt_thread, o) == -1) {
		ast_log(LOG_WARNING, "Unable to create thread :(\n");
		free(o);
	}
}

static int scan_service(char *fn, time_t now, time_t atime)
{
	struct outgoing *o;
	struct utimbuf tbuf;
	FILE *f;
	o = malloc(sizeof(struct outgoing));
	if (o) {
		init_outgoing(o);
		f = fopen(fn, "r+");
		if (f) {
			if (!apply_outgoing(o, fn, f)) {
				/* Increment retries */
				o->retries++;
#if 0
				printf("Retries: %d, max: %d\n", o->retries, o->maxretries);
#endif
				if (o->retries <= o->maxretries + 1) {
					/* Add a retry line at the end */
					fseek(f, 0L, SEEK_END);
					fprintf(f, "Retry: %d (%ld)\n", o->retries, (long) now);
					fclose(f);
					/* Update the file time */
					tbuf.actime = atime;
					tbuf.modtime = now + o->retrytime;
					if (utime(o->fn, &tbuf))
						ast_log(LOG_WARNING, "Unable to set utime on %s: %s\n", fn, strerror(errno));
					now += o->retrytime;
					launch_service(o);
					return now;
				} else {
					ast_log(LOG_EVENT, "Queued call to %s/%s expired without completion after %d attempt(s)\n", o->tech, o->dest, o->retries - 1);
					fclose(f);
					free(o);
					unlink(fn);
					return 0;
				}
			} else {
				free(o);
				ast_log(LOG_WARNING, "Invalid file contents in %s, deleting\n", fn);
				fclose(f);
				unlink(fn);
			}
		} else {
			free(o);
			ast_log(LOG_WARNING, "Unable to open %s: %s, deleting\n", fn, strerror(errno));
			unlink(fn);
		}
	} else
		ast_log(LOG_WARNING, "Out of memory :(\n");
	return -1;
}

static void *scan_thread(void *unused)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	char fn[256];
	int res;
	time_t last = 0, next = 0, now;
	for(;;) {
		/* Wait a sec */
		sleep(1);
		time(&now);
		if (!stat(qdir, &st)) {
			if ((st.st_mtime != last) || (next && (now > next))) {
#if 0
				printf("atime: %ld, mtime: %ld, ctime: %ld\n", st.st_atime, st.st_mtime, st.st_ctime);
				printf("Ooh, something changed / timeout\n");
#endif				
				next = 0;
				last = st.st_mtime;
				dir = opendir(qdir);
				if (dir) {
					while((de = readdir(dir))) {
						snprintf(fn, sizeof(fn), "%s/%s", qdir, de->d_name);
						if (!stat(fn, &st)) {
							if (S_ISREG(st.st_mode)) {
								if (st.st_mtime <= now) {
									res = scan_service(fn, now, st.st_atime);
									if (res > 0) {
										/* Update next service time */
										if (!next || (res < next)) {
											next = res;
										}
									} else if (res)
										ast_log(LOG_WARNING, "Failed to scan service '%s'\n", fn);
								} else {
									/* Update "next" update if necessary */
									if (!next || (st.st_mtime < next))
										next = st.st_mtime;
								}
							}
						} else
							ast_log(LOG_WARNING, "Unable to stat %s: %s\n", fn, strerror(errno));
					}
					closedir(dir);
				} else
					ast_log(LOG_WARNING, "Unable to open directory %s: %s\n", qdir, strerror(errno));
			}
		} else
			ast_log(LOG_WARNING, "Unable to stat %s\n", qdir);
	}
	return NULL;
}

int unload_module(void)
{
	return -1;
}

int load_module(void)
{
	pthread_t thread;
	pthread_attr_t attr;
	snprintf((char *)qdir,sizeof(qdir)-1,"%s/%s",(char *)ast_config_AST_SPOOL_DIR,"outgoing");
	if (mkdir(qdir, 0700) && (errno != EEXIST)) {
		ast_log(LOG_WARNING, "Unable to create queue directory %s -- outgoing spool disabled\n", qdir);
		return 0;
	}
	pthread_attr_init(&attr);
 	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread,&attr,scan_thread, NULL) == -1) {
		ast_log(LOG_WARNING, "Unable to create thread :(\n");
		return -1;
	}
	return 0;
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 1;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
