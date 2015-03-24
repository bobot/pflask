/*
 * The process in the flask.
 *
 * Copyright (c) 2013, Alessandro Ghedini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <sys/syscall.h>

#include <sched.h>
#include <linux/sched.h>

#include <signal.h>
#include <getopt.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "dev.h"
#include "pty.h"
#include "user.h"
#include "mount.h"
#include "cgroup.h"
#include "netif.h"
#include "printf.h"
#include "util.h"

static int clone_flags = SIGCHLD      |
                         CLONE_NEWNS  |
                         CLONE_NEWIPC |
                         CLONE_NEWPID |
                         CLONE_NEWUTS |
  			 CLONE_NEWUSER;


static const char *short_opts = "+m:o:n::u:r:c:g:da:s:kUMNIHPh?";

static struct option long_opts[] = {
	{ "mount",     required_argument, NULL, 'm' },
	{ "map",       required_argument, NULL, 'o' },
	{ "netif",     optional_argument, NULL, 'n' },
	{ "user",      required_argument, NULL, 'u' },
	{ "chroot",    required_argument, NULL, 'r' },
	{ "chdir",     required_argument, NULL, 'c' },
	{ "cgroup",    required_argument, NULL, 'g' },
	{ "detach",    no_argument,       NULL, 'd' },
	{ "attach",    required_argument, NULL, 'a' },
	{ "setenv",    required_argument, NULL, 's' },
	{ "keepenv",   no_argument,       NULL, 'k' },
	{ "no-userns", no_argument,       NULL, 'U' },
	{ "no-mountns", no_argument,      NULL, 'M' },
	{ "no-netns",  no_argument,       NULL, 'N' },
	{ "no-ipcns",  no_argument,       NULL, 'I' },
	{ "no-utsns",  no_argument,       NULL, 'H' },
	{ "no-pidns",  no_argument,       NULL, 'P' },
	{ "help",      no_argument,       NULL, 'h' },
	{ 0, 0, 0, 0 }
};

static void do_daemonize(void);
static void do_chroot(char *dest);
static pid_t do_clone(void);

static inline void help(void);

int main(int argc, char *argv[]) {
	int rc, i;

	pid_t pid  = -1;
	pid_t ppid = getpid();

	uid_t uid = -1;
	gid_t gid = -1;

	uid_t pw_uid = 0;
	gid_t pw_gid = 0;
	_free_ char *dest   = NULL;
	_free_ char *change = NULL;
	_free_ char *env    = NULL;
	_free_ char *cgroup = NULL;

	_close_ int master_fd = -1;

	char *master_name;

	int detach  = 0;
	int keepenv = 0;

	siginfo_t status;

        int uid_map_set = 1;
        uid_t uid_map;
        unsigned int uid_len;
        gid_t gid_map;
        unsigned int gid_len;

	while ((rc = getopt_long(argc, argv, short_opts, long_opts, &i)) !=-1) {
		switch (rc) {
			case 'm':
				validate_optlist("--mount", optarg);

				add_mount_from_spec(optarg);
				break;

			case 'n':
				clone_flags |= CLONE_NEWNET;

				if (optarg != NULL) {
					validate_optlist("--netif", optarg);

					add_netif_from_spec(optarg);
				}
				break;

			case 'u':
                                if (sscanf(optarg,"%u,%u,%u,%u",
                                           &uid_map, &uid_len, &gid_map, &gid_len)
                                    != 2){
                                  fail_printf("Invalid value '%s' for --map",optarg);
                                }

				break;

                        case 'o':
                          uid_map_set = 1;
                          if (sscanf(optarg,"%u,%u,%u,%u",
                                     &uid_map, &uid_len, &gid_map, &gid_len)
                              != 4){
                            fail_printf("Invalid value '%s' for --map",optarg);
                          }
                          break;
                        
			case 'r':
				freep(&dest);

				dest = realpath(optarg, NULL);
				if (dest == NULL) sysf_printf("realpath()");
				break;

			case 'c':
				freep(&change);

				change = strdup(optarg);
				break;

			case 'g':
				validate_optlist("--cgroup", optarg);
				validate_cgroup_spec(optarg);

				freep(&change);

				cgroup = strdup(optarg);
				break;

			case 'd':
				detach = 1;
				break;

			case 'a': {
				char *end = NULL;
				pid = strtol(optarg, &end, 10);
				if (*end != '\0')
					fail_printf("Invalid value '%s' for --attach", optarg);
				break;
			}

			case 's': {
				validate_optlist("--setenv", optarg);

				if (env != NULL) {
					char *tmp = env;

					rc = asprintf(&env, "%s,%s", env, optarg);
					if (rc < 0) fail_printf("OOM");

					freep(&tmp);
				} else {
					env = strdup(optarg);
				}

				break;
			}

			case 'k':
				keepenv = 1;
				break;

			case 'M':
				clone_flags &= ~(CLONE_NEWNS);
				break;

			case 'N':
				clone_flags &= ~(CLONE_NEWNET);
				break;

			case 'I':
				clone_flags &= ~(CLONE_NEWIPC);
				break;

			case 'H':
				clone_flags &= ~(CLONE_NEWUTS);
				break;

			case 'P':
				clone_flags &= ~(CLONE_NEWPID);
				break;

			case '?':
			case 'h':
				help();
				return 0;
		}
	}

	if (pid != -1) {
		master_fd = recv_pty(pid);
		if (master_fd < 0) fail_printf("Invalid PID '%u'", pid);

		pid = -1;
		goto process_fd;
	}


	open_master_pty(&master_fd, &master_name);

	uid = getuid();
	gid = getgid();

	if (detach == 1)
		do_daemonize();

        /** pipe used for the child to wait the parent
            until the parent set the uid_map and gid_map
         */
        int pipe_sync[2];
        if(pipe(pipe_sync) == -1){
          fail_printf("pipe");
        }

	pid = do_clone();

	if (pid == 0) {
                close(pipe_sync[1]);
                char buf;
                if(read(pipe_sync[0],&buf,1) != 1){
                  fail_printf("synchro failed");
                }
                close(pipe_sync[0]);

                closep(&master_fd);

		open_slave_pty(master_name);

		rc = setsid();
		if (rc < 0) sysf_printf("setsid()");

		rc = prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (rc < 0) sysf_printf("prctl(PR_SET_PDEATHSIG)");

		do_cgroup(cgroup, ppid);

		do_user(pw_uid, pw_gid);

		do_mount(dest);

		/* if (clone_flags & CLONE_NEWUSER){ */
                /*   if (uid_map_set){ */
                /*     map_users_to_users(getpid(),uid_map, uid_len, gid_map, gid_len); */
                /*   }else{ */
                /*     map_user_to_user(uid, gid, pw_uid, pw_gid); */
                /*   } */
                /* } */

		if (dest != NULL) {
                  do_chroot(dest);
		}

		if (clone_flags & CLONE_NEWNET)
			setup_loopback();

		umask(0022);

		/* TODO: drop capabilities */


		if (change != NULL) {
			rc = chdir(change);
			if (rc < 0) sysf_printf("chdir()");
		}

		if (dest != NULL) {
			char *term = getenv("TERM");

			if (keepenv == 0)
				clearenv();

			setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
			/* setenv("USER", user, 1); */
			/* setenv("LOGNAME", user, 1); */
			setenv("TERM", term, 1);
		}

		if (env != NULL) {
			size_t i, c;

			_free_ char **vars = NULL;

			_free_ char *tmp = strdup(env);
			if (tmp == NULL) fail_printf("OOM");

			c = split_str(tmp, &vars, ",");

			for (i = 0; i < c; i++) {
				rc = putenv(strdup(vars[i]));
				if (rc != 0) sysf_printf("putenv()");
			}
		}

		setenv("container", "pflask", 1);

		if (argc > optind)
			rc = execvpe(argv[optind], argv + optind, environ);
		else
			rc = execle("/bin/bash", "-bash", NULL, environ);

		if (rc < 0) sysf_printf("exec()");
	}

        printf("pid: %i\n", pid);

        if (clone_flags & CLONE_NEWUSER){
          if (uid_map_set){
            map_users_to_users(pid,uid_map, uid_len, gid_map, gid_len);
          }else{
            map_user_to_user(uid, gid, pw_uid, pw_gid);
          }
        }

	do_netif(pid);

        close(pipe_sync[0]);
        write(pipe_sync[1],"a",1);
        close(pipe_sync[1]);

process_fd:
	if (detach == 1)
		serve_pty(master_fd);
	else
		process_pty(master_fd);

	if (pid == -1)
		return 0;

	kill(pid, SIGKILL);

	rc = waitid(P_PID, pid, &status, WEXITED);
	if (rc < 0) sysf_printf("waitid()");

	switch (status.si_code) {
		case CLD_EXITED:
			if (status.si_status != 0)
				err_printf("Child failed with code '%d'",
				           status.si_status);
			else
				ok_printf("Child exited");
			break;

		case CLD_KILLED:
			err_printf("Child was terminated");
			break;

		default:
			err_printf("Child failed");
			break;
	}

	undo_cgroup(cgroup, ppid);

	return status.si_status;
}

static void do_daemonize(void) {
	int rc;

	openlog("pflask", LOG_NDELAY | LOG_PID, LOG_DAEMON);
	use_syslog = 1;

	rc = daemon(0, 0);
	if (rc < 0) sysf_printf("daemon()");
}

static void do_chroot(char *dest) {
	int rc;

	rc = chdir(dest);
	if (rc < 0) sysf_printf("chdir()");

	rc = chroot(".");
	if (rc < 0) sysf_printf("chroot()");

	rc = chdir("/");
	if (rc < 0) sysf_printf("chdir(/)");
}

#define STACK_SIZE (1024 * 1024)

static pid_t do_clone(void) {
	pid_t pid;

	pid = syscall(__NR_clone, clone_flags, NULL);

	if (pid < 0) sysf_printf("clone()");

	return pid;
}

static inline void help(void) {
	#define CMD_HELP(CMDL, CMDS, MSG) printf("  %s, %-15s \t%s.\n", \
                                                 COLOR_YELLOW CMDS, \
                                                 CMDL COLOR_OFF, MSG);

	printf(COLOR_RED "Usage: " COLOR_OFF);
	printf(COLOR_GREEN "pflask " COLOR_OFF);
	puts("[OPTIONS] [COMMAND [ARGS...]]\n");

	puts(COLOR_RED " Options:" COLOR_OFF);

	CMD_HELP("--mount", "-m",
		"Create a new mount point inside the container");
	CMD_HELP("--netif", "-n",
		"Create a new network namespace and optionally move a network \
interface inside it");

	CMD_HELP("--user",  "-u",
		"Run the command as the specified user inside the container");
	CMD_HELP("--map",   "-o",
		"Run the command with the given user and group map \
\"firstuid,len,firstgid,len\"");

	CMD_HELP("--chroot",  "-r",
		"Use the specified directory as root inside the container");
	CMD_HELP("--chdir", "-c",
		"Change to the specified directory inside the container");

	CMD_HELP("--detach", "-d",
		"Detach from terminal");
	CMD_HELP("--attach", "-a",
		"Attach to the specified detached process");

	CMD_HELP("--setenv", "-s",
		"Set additional environment variables");

	puts("");

	CMD_HELP("--no-userns",  "-U", "Disable user namespace support");
	CMD_HELP("--no-mountns", "-M", "Disable mount namespace support");
	CMD_HELP("--no-netns",   "-N", "Disable net namespace support");
	CMD_HELP("--no-ipcns",   "-I", "Disable IPC namespace support");
	CMD_HELP("--no-utsns",   "-H", "Disable UTS namespace support");
	CMD_HELP("--no-pidns",   "-P", "Disable PID namespace support");

	puts("");
}
