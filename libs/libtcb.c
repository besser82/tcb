#define _GNU_SOURCE
#include <errno.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <grp.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

#include "tcb.h"

#define TIMEOUT				1
#define TRIES				15

#define LOCK_SUFFIX			".lock"

static void setup_timer(void);
static void unsetup_timer(void);

static int lockfd = -1;

static int set_close_on_exec(int fd)
{
	int flags = fcntl(fd, F_GETFD, 0);
	if (flags == -1)
		return -1;
	flags |= FD_CLOEXEC;
	return fcntl(fd, F_SETFD, flags);
}

static int do_lock(int fd)
{
	struct flock fl;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	return fcntl(fd, F_SETLKW, &fl);
}

static void setup_timer()
{
	struct itimerval v = {
		{TIMEOUT, 0},
		{TIMEOUT, 0}
	};
	setitimer(ITIMER_REAL, &v, 0);
}

static void unsetup_timer()
{
	struct itimerval v = {
		{0, 0},
		{0, 0}
	};
	setitimer(ITIMER_REAL, &v, 0);
}

static void alarm_catch(int sig)
{
	/* does nothing, but fcntl F_SETLKW will fail with EINTR */
}

int lckpwdf_tcb(const char *file)
{
	struct sigaction act, oldact;
	sigset_t set, oldset;
	char *lockfile;
	int i;

	if (lockfd != -1)
		return -1;

	asprintf(&lockfile, "%s%s", file, LOCK_SUFFIX);
	if (!lockfile)
		return -1;
	lockfd = open(lockfile,
	    O_CREAT | O_WRONLY | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW, 0600);
	free(lockfile);
	if (lockfd == -1)
		return -1;

	if (set_close_on_exec(lockfd) == -1)
		goto cleanup_fd;

	memset(&act, 0, sizeof(act));
	act.sa_handler = alarm_catch;
	act.sa_flags = 0; /* no SA_RESTART */
	sigfillset(&act.sa_mask);
	if (sigaction(SIGALRM, &act, &oldact) == -1)
		goto cleanup_fd;

	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	if (sigprocmask(SIG_UNBLOCK, &set, &oldset) == -1)
		goto cleanup_sig;

	setup_timer();

	for (i = 0; i < TRIES; i++) {
		if (do_lock(lockfd) == 0) {
			unsetup_timer();
			sigprocmask(SIG_SETMASK, &oldset, NULL);
			sigaction(SIGALRM, &oldact, NULL);
			return 0;
		}
		if (errno != EINTR)
			break;
	}

	unsetup_timer();
	sigprocmask(SIG_SETMASK, &oldset, NULL);

cleanup_sig:
	sigaction(SIGALRM, &oldact, NULL);

cleanup_fd:
	close(lockfd);
	lockfd = -1;
	return -1;
}

int ulckpwdf_tcb(void)
{
	if (lockfd == -1)
		return -1;

	if (close(lockfd) == -1) {
		lockfd = -1;
		return -1;
	}
	lockfd = -1;

	return 0;
}

static struct tcb_privs glob_privs = { {0}, 0, -1, -1, 0 };

#define PRIV_MAGIC			0x1004000a
#define PRIV_MAGIC_NONROOT		0xdead000a

int tcb_drop_priv_r(const char *name, struct tcb_privs *p)
{
	int res;
	struct stat st;
	gid_t shadow_gid = -1;
	char *dir;

	if (p->is_dropped)
		return -1;

/* if not root, we can do nothing and in fact we do not care */
	if (geteuid()) {
		p->is_dropped = PRIV_MAGIC_NONROOT;
		return 0;
	}

	if (stat(TCB_DIR, &st))
		return -1;

	shadow_gid = st.st_gid;

	asprintf(&dir, "%s/%s", TCB_DIR, name);
	if (!dir)
		return -1;
	if (stat(dir, &st)) {
		free(dir);
		return -1;
	}
	free(dir);

	res = getgroups(sizeof(p->grpbuf), p->grpbuf);
	if (res == -1 || res > sizeof(p->grpbuf))
		return -1;

	p->saved_groups = res;

	if (setgroups(0, NULL) == -1)
		return -1;
	p->old_egid = getegid();
	if (setregid(-1, shadow_gid) == -1)
		return -1;
	p->old_euid = geteuid();
	if (setreuid(-1, st.st_uid) == -1)
		return -1;

	p->is_dropped = PRIV_MAGIC;
	return 0;
}

int tcb_gain_priv_r(struct tcb_privs *p)
{
	switch (p->is_dropped) {
	case PRIV_MAGIC_NONROOT:
		p->is_dropped = 0;
		return 0;

	case PRIV_MAGIC:
		break;

	default:
		return -1;
	}

	if (setreuid(-1, p->old_euid) == -1)
		return -1;
	if (setregid(-1, p->old_egid) == -1)
		return -1;
	if (setgroups(p->saved_groups, p->grpbuf) == -1)
		return -1;

	p->is_dropped = 0;
	return 0;
}

int tcb_drop_priv(const char *name)
{
	return tcb_drop_priv_r(name, &glob_privs);
}

int tcb_gain_priv()
{
	return tcb_gain_priv_r(&glob_privs);
}

int tcb_is_suspect(int fd)
{
	struct stat st;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
	    (st.st_size > st.st_blocks * st.st_blksize &&
	    st.st_blksize >= 512))
		return -1;

	return 0;
}