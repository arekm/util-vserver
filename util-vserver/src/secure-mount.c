// $Id$    --*- c++ -*--

// Copyright (C) 2003 Enrico Scholz <enrico.scholz@informatik.tu-chemnitz.de>
//  
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; version 2 of the License.
//  
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//  
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.


  // secure-mount <general mount(8) options> [--secure] [--chroot <dir>]
  //              [--mtab <mtabfile>] [--fstab <fstabfile>]
  //
  // Executes mount-operations in the given chroot-dir: it assumes sources in
  // the current root-dir while destinations are expected in the chroot
  // environment.  When '--secure' is given, the destination must not contain
  // symlinks.


#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "util.h"
#include "pathconfig.h"

#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <linux/fs.h>
#include <assert.h>
#include <ctype.h>
#include <sys/wait.h>
#include <libgen.h>
#include <signal.h>

#define MNTPOINT	"/etc"

struct MountInfo {
    char const *	src;
    char const *	dst;
    char const *	type;
    unsigned long	flag;
    unsigned long	xflag;
    char *		data;
};

struct Options {
    char const *	mtab;
    char const *	fstab;
    char const *	rootdir;
    bool		ignore_mtab;
    bool		mount_all;
    bool		is_secure;

    int			cur_rootdir_fd;
};

#define OPTION_BIND	1024
#define OPTION_MOVE	1025
#define OPTION_MTAB	1026
#define OPTION_FSTAB	1027
#define OPTION_CHROOT	1028
#define OPTION_SECURE	1029
#define OPTION_RBIND	1030

#define XFLAG_NOAUTO	0x01

static struct option const
CMDLINE_OPTIONS[] = {
  { "help",    no_argument,       0, 'h' },
  { "version", no_argument,       0, 'v' },
  { "bind",    no_argument,       0, OPTION_BIND },
  { "move",    no_argument,       0, OPTION_MOVE },
  { "mtab",    required_argument, 0, OPTION_MTAB },
  { "fstab",   required_argument, 0, OPTION_FSTAB },
  { "chroot",  required_argument, 0, OPTION_CHROOT },
  { "secure",  no_argument,       0, OPTION_SECURE },
  { "rbind",   no_argument,       0, OPTION_RBIND },
  { 0, 0, 0, 0 }
};

#ifndef MS_REC
#  define MS_REC	0x4000
#endif

static struct FstabOption {
    char const * const	opt;
    unsigned long const		flag;
    unsigned long const		mask;
    unsigned long const 	xflag;
    bool const			is_dflt;
} const FSTAB_OPTIONS[] = {
  { "defaults",   0,             (MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|
				  MS_SYNCHRONOUS), 0, false },
  { "rbind",      MS_BIND|MS_REC, MS_BIND|MS_REC,  0, false },
  { "bind",       MS_BIND,        MS_BIND,         0, false },
  { "move",       MS_MOVE,        MS_MOVE,         0, false },
  { "async",      0,              MS_SYNCHRONOUS,  0, false },
  { "sync",       MS_SYNCHRONOUS, MS_SYNCHRONOUS,  0, false },
  { "atime",      0,              MS_NOATIME,      0, false },
  { "noatime",    MS_NOATIME,     MS_NOATIME,      0, false },
  { "dev",        0,              MS_NODEV,        0, false },
  { "nodev",      MS_NODEV,       MS_NODEV,        0, false },
  { "exec",       0,              MS_NOEXEC,       0, false },
  { "noexec",     MS_NOEXEC,      MS_NOEXEC,       0, false },
  { "suid",       0,              MS_NOSUID,       0, false },
  { "nosuid",     MS_NOSUID,      MS_NOSUID,       0, false },
  { "ro",         MS_RDONLY,      MS_RDONLY,       0, false },
  { "rw",         0,              MS_RDONLY,       0, false },
  
  { "remount",    MS_REMOUNT,     MS_REMOUNT,      0, false },
  { "users",      MS_NOEXEC|MS_NOSUID|MS_NODEV,
                  MS_NOEXEC|MS_NOSUID|MS_NODEV,    0, false },
  { "mandlock",   MS_MANDLOCK,    MS_MANDLOCK,     0, false },
  { "nodiratime", MS_NODIRATIME,  MS_NODIRATIME,   0, false },
#ifdef MS_DIRSYNC  
  { "dirsync",    MS_DIRSYNC,     MS_DIRSYNC,      0, false },
#endif
  { "_netdev",    0,              0,               0, false },
  { "auto",       0,              0,               0, false },
  { "noauto",     0,              0,               XFLAG_NOAUTO, false },
  { "user",       0,              0,               0, false },
  { "nouser",     0,              0,               0, false },
};

static void
showHelp(int fd, char const *cmd, int res)
{
  VSERVER_DECLARE_CMD(cmd);
  
  WRITE_MSG(fd, "Usage:  ");
  WRITE_STR(fd, cmd);
  WRITE_MSG(fd,
	    " [--help] [--version] [--bind] [--move] [--rbind] [-t <type>] [-n]\n"
	    "            [--mtab <filename>] [--fstab <filename>] [--chroot <dirname>] \n"
	    "            [--secure] -a|([-o <options>] [--] <src> <dst>)\n\n"
	    "Executes mount-operations in the given chroot-dir: it assumes sources in the\n"
	    "current root-dir while destinations are expected in the chroot environment.\n"
	    "When '--secure' is given, the destination must not contain symlinks.\n\n"
	    "For non-trivial mount-operations it uses the external 'mount' program which\n"
	    "can be overridden by the $MOUNT environment variable.\n\n"
	    "Please report bugs to " PACKAGE_BUGREPORT "\n");

  exit(res);
}

static void
showVersion()
{
  WRITE_MSG(1,
	    "secure-mount " VERSION " -- secure mounting of directories\n"
	    "This program is part of " PACKAGE_STRING "\n\n"
	    "Copyright (C) 2003 Enrico Scholz\n"
	    VERSION_COPYRIGHT_DISCLAIMER);
  exit(0);
}

inline static bool
isSameObject(struct stat const *lhs,
	     struct stat const *rhs)
{
  return (lhs->st_dev==rhs->st_dev &&
	  lhs->st_ino==rhs->st_ino);
}

static int
chdirSecure(char const *dir)
{
  char		tmp[strlen(dir)+1], *ptr;
  char const	*cur;

  strcpy(tmp, dir);
  cur = strtok_r(tmp, "/", &ptr);
  while (cur) {
    struct stat		pre_stat, post_stat;

    if (lstat(cur, &pre_stat)==-1) return -1;
    
    if (!S_ISDIR(pre_stat.st_mode)) {
      errno = ENOENT;
      return -1;
    }
    if (S_ISLNK(pre_stat.st_mode)) {
      errno = EINVAL;
      return -1;
    }

    if (chdir(cur)==-1)            return -1;
    if (stat(".", &post_stat)==-1) return -1;

    if (!isSameObject(&pre_stat, &post_stat)) {
      char	dir[PATH_MAX];
      
      WRITE_MSG(2, "Possible symlink race ATTACK at '");
      WRITE_STR(2, getcwd(dir, sizeof(dir)));
      WRITE_MSG(2, "'\n");

      errno = EINVAL;
      return -1;
    }

    cur = strtok_r(0, "/", &ptr);
  }

  return 0;
}

static int
verifyPosition(char const *mntpoint, char const *dir1, char const *dir2)
{
  struct stat		pre_stat, post_stat;

  if (stat(mntpoint, &pre_stat)==-1)       return -1;
  if (chroot(dir1)==-1 || chdir(dir2)==-1) return -1;
  if (stat(".", &post_stat)==-1)           return -1;

  if (!isSameObject(&pre_stat, &post_stat)) {
    char	dir[PATH_MAX];
      
    WRITE_MSG(2, "Possible symlink race ATTACK at '");
    WRITE_STR(2, getcwd(dir, sizeof(dir)));
    WRITE_MSG(2, "' within '");
    WRITE_STR(2, dir1);
    WRITE_STR(2, "'\n");

    errno = EINVAL;
    return -1;
  }

  return 0;
}

static int
fchroot(int fd)
{
  if (fchdir(fd)==-1 || chroot(".")==-1) return -1;
  return 0;
}

static int
writeX(int fd, void const *buf, size_t len)
{
  if ((size_t)(write(fd, buf, len))!=len) return -1;
  return 0;
}

static int
writeStrX(int fd, char const *str)
{
  return writeX(fd, str, strlen(str));
}

static inline char const *
getType(struct MountInfo const *mnt)
{
  if (mnt->type==0)                         return "none";
  else if (strncmp(mnt->type, "ext", 3)==0) return "ufs";
  else                                      return mnt->type;
}

static int
updateMtab(struct MountInfo const *mnt, struct Options const *opt)
{
  int		res = -1;
  int		fd;
  assert(opt->mtab!=0);

  if (opt->rootdir!=0 &&
      chroot(opt->rootdir)==-1) {
      perror("secure-mount: chroot()");
      return -1;
  }

  fd=open(opt->mtab, O_CREAT|O_APPEND|O_WRONLY, 0644);
  
  if (fd==-1) perror("secure-mount: open(<mtab>)");
  
  if (fchroot(opt->cur_rootdir_fd)==-1) {
    perror("secure-mount: fchroot()");
    goto err1;
  }

  if (fd==-1) goto err0;

  if (flock(fd, LOCK_EX)==-1) {
    perror("secure-mount: flock()");
    goto err1;
  }


  if (writeStrX(fd, mnt->src)==-1 ||
      writeStrX(fd, " ")==-1 ||
      writeStrX(fd, mnt->dst)==-1 ||
      writeStrX(fd, " ")==-1 ||
      writeStrX(fd, getType(mnt))==-1 ||
      writeStrX(fd, " ")==-1 ||
      writeStrX(fd, mnt->data ? mnt->data : "defaults")==-1 ||
      writeStrX(fd, " 0 0\n")==-1) {
    perror("secure-mount: write()");
    goto err1;
  }

  res = 0;

  err1:	close(fd);
  err0:	return res;
}

static bool
callExternalMount(struct MountInfo const *mnt)
{
  char const * 	argv[10];
  size_t	idx = 0;
  pid_t		pid;
  int		status;
  char const *	mount_prog = getenv("MOUNT");

  if (mount_prog==0) mount_prog = MOUNT_PROG;

  argv[idx++] = mount_prog;
  argv[idx++] = "-n";
  if      (mnt->flag & MS_BIND) argv[idx++] = "--bind";
  else if (mnt->flag & MS_MOVE) argv[idx++] = "--move";

  if (mnt->data &&
      strcmp(mnt->data, "defaults")!=0) {
    argv[idx++] = "-o";
    argv[idx++] = mnt->data;
  }

  if (mnt->type) {
    argv[idx++] = "-t";
    argv[idx++] = mnt->type;
  }

  argv[idx++] = mnt->src;
  argv[idx++] = ".";
  argv[idx]   = 0;

  pid = fork();
  if (pid==-1) {
    perror("secure-mount: fork()");
    return false;
  }

  if (pid==0) {
    execv(mount_prog, const_cast(char **)(argv));
    PERROR_Q("secure-mount: execv", mount_prog);
    exit(1);
  }

  if (wait4(pid, &status, 0, 0)==-1) {
    perror("secure-mount: wait4()");
    return false;
  }

  return (WIFEXITED(status)) && (WEXITSTATUS(status)==0);
}

static bool
mountSingle(struct MountInfo const *mnt, struct Options const *opt)
{
  char const	*dir = mnt->dst;

  assert(mnt->dst!=0);
  
  if (opt->rootdir!=0) {
    if (chdir(opt->rootdir)==-1) {
      PERROR_Q("secure-mount: chdir", opt->rootdir);
      return false;
    }

    while (*dir=='/') ++dir;
  }

  if (opt->is_secure) {
    if (chdirSecure(dir)==-1) {
      PERROR_Q("secure-mount: chdirSecure", dir);
      return false;
    }
  }
  else {
    if (*dir!='\0' &&
	chdir(dir)==-1) {
      PERROR_Q("secure-mount: chdir", dir);
      return false;
    }
  }

  if (mnt->flag & (MS_BIND|MS_MOVE)) {
    if (mount(mnt->src, ".",
	      mnt->type ? mnt->type : "",
	      mnt->flag,  mnt->data)==-1) {
      perror("secure-mount: mount()");
      return false;
    }
  }
  else {
    if (!callExternalMount(mnt)) return false;
  }

    // Check if directories were moved between the chdirSecure() and mount(2)
  if ((mnt->flag&MS_BIND) && opt->rootdir!=0 &&
      (verifyPosition(mnt->src, opt->rootdir, mnt->dst)==-1 ||
       fchroot(opt->cur_rootdir_fd)==-1)) {
    perror("secure-mount: verifyPosition/fchroot");
      // TODO: what is with unmounting?
    return false;
  }

  if (!opt->ignore_mtab &&
      updateMtab(mnt, opt)==-1) {
    WRITE_MSG(2, "Failed to update mtab-file\n");
      // no error
  }
  
  return true;
}

static struct FstabOption const *
searchOption(char const *opt, size_t len)
{
  struct FstabOption const *		i;
  for (i=FSTAB_OPTIONS+0; i<FSTAB_OPTIONS+DIM_OF(FSTAB_OPTIONS); ++i)
    if (strncmp(i->opt, opt, len)==0) return i;

  return 0;
}

static bool
transformOptionList(struct MountInfo *info)
{
  char const *			ptr = info->data;

  do {
    char const *		pos = strchr(ptr, ',');
    struct FstabOption const *	opt;
    
    if (pos==0) pos = ptr+strlen(ptr);
    opt = searchOption(ptr, pos-ptr);

    if (opt!=0) {
      info->flag  &= ~opt->mask;
      info->flag  |=  opt->flag;
      info->xflag |=  opt->xflag;
    }

    if (*pos!='\0')
      ptr = pos+1;
    else
      ptr = pos;

  } while (*ptr!='\0');

  return true;
}

#define MOVE_TO_NEXT_FIELD(PTR,ALLOW_EOL)		\
  while (!isspace(*PTR) && *PTR!='\0') ++PTR;		\
  if (!(ALLOW_EOL) && *PTR=='\0') return prFAIL;	\
  *PTR++ = '\0';					\
  while (isspace(*PTR)) ++PTR

static enum {prDOIT, prFAIL, prIGNORE}
parseFstabLine(struct MountInfo	*info, char *buf)
{
  while (isspace(*buf)) ++buf;
  if (*buf=='#' || *buf=='\0')  return prIGNORE;

  info->src  = buf;
  MOVE_TO_NEXT_FIELD(buf, false);
  info->dst  = buf;
  MOVE_TO_NEXT_FIELD(buf, false);
  info->type = buf;
  MOVE_TO_NEXT_FIELD(buf, false);
  info->data = buf;
  MOVE_TO_NEXT_FIELD(buf, true);

  if (strcmp(info->type, "swap")==0) return prIGNORE;
  if (strcmp(info->type, "none")==0) info->type = 0;

  info->flag   = 0;
  info->xflag  = 0;
  if (!transformOptionList(info)) return prFAIL;
  if (info->xflag & XFLAG_NOAUTO) return prIGNORE;

  return prDOIT;
}

#undef MOVE_TO_NEXT_FIELD

static bool
mountFstab(struct Options const *opt)
{
  bool		res = false;
  int		fd;
  off_t		len;

  assert(opt->fstab!=0);
  fd = open(opt->fstab, O_RDONLY);
  if (fd==-1) {
    perror("secure-mount: open(<fstab>)");
    goto err0;
  }

  len = lseek(fd, 0, SEEK_END);
  if (len==-1 ||
      lseek(fd, 0, SEEK_SET)==-1) {
    perror("secure-mount: lseek(<fstab>)");
    goto err1;
  }

  {
    char	buf[len+2];
    char	*ptr, *ptrptr;

    if (read(fd, buf, len+1)!=len) {
      perror("secure-mount: read()");
      goto err1;
    }
    buf[len]   = '#';	// workaround for broken dietlibc strtok_r()
			// implementation
    buf[len+1] = '\0';

    ptr = strtok_r(buf, "\n", &ptrptr);
    while (ptr) {
      struct MountInfo	mnt;
      char *		new_ptr = strtok_r(0, "\n", &ptrptr);

      switch (parseFstabLine(&mnt, ptr)) {
	case prFAIL	:
	  WRITE_MSG(2, "Failed to parse fstab-line beginning with '");
	  WRITE_STR(2, ptr);
	  WRITE_MSG(2, "'\n");
	  goto err1;

	case prIGNORE	:  break;
	case prDOIT	:
	  chdir("/");
	  if (!mountSingle(&mnt, opt)) {
	    WRITE_MSG(2, "Failed to mount fstab-line beginning with '");
	    WRITE_STR(2, ptr);
	    WRITE_MSG(2, "'\n");
	  }
	  break;
	default		:
	  assert(false);
      }

      ptr = new_ptr;
    }
  }

  res = true;

  err1: close(fd);
  err0: return res;
}

int main(int argc, char *argv[])
{
  struct MountInfo	mnt = {
    .src         = 0,
    .dst         = 0,
    .type        = 0,
    .flag        = 0,
    .xflag	 = 0,
    .data        = 0,
  };

  struct Options	opt = {
    .mtab           = "/etc/mtab",
    .fstab          = "/etc/fstab",
    .rootdir        = 0,
    .ignore_mtab    = false,
    .mount_all      = false,
    .is_secure      = false,
    .cur_rootdir_fd = -1
  };

  opt.cur_rootdir_fd = open("/", O_RDONLY|O_DIRECTORY);

  if (opt.cur_rootdir_fd==-1) {
    perror("secure-mount: open(\"/\")");
    return EXIT_FAILURE;
  }

  signal(SIGCHLD, SIG_DFL);

  while (1) {
    int		c = getopt_long(argc, argv, "ht:nao:", CMDLINE_OPTIONS, 0);
    if (c==-1) break;
    
    switch (c) {
      case 'h'		:  showHelp(1, argv[0], 0);
      case 'v'		:  showVersion();
      case 't'		:  mnt.type = optarg;         break;
      case 'n'		:  opt.ignore_mtab = true;    break;
      case 'a'		:  opt.mount_all   = true;    break;
      case 'o'		:  mnt.data        = optarg;  break;
      case OPTION_RBIND	:  mnt.flag       |= MS_REC;  /*@fallthrough@*/
      case OPTION_BIND	:  mnt.flag       |= MS_BIND; break;
      case OPTION_MOVE	:  mnt.flag       |= MS_MOVE; break;
      case OPTION_MTAB	:  opt.mtab        = optarg;  break;
      case OPTION_FSTAB	:  opt.fstab       = optarg;  break;
      case OPTION_CHROOT:  opt.rootdir     = optarg;  break;
      case OPTION_SECURE:  opt.is_secure   = true;    break;
      default		:
	WRITE_MSG(2, "Try '");
	WRITE_STR(2, argv[0]);
	WRITE_MSG(2, " --help\" for more information.\n");
	return EXIT_FAILURE;
	break;
    }
  }

  if (opt.mount_all && optind<argc) {
    WRITE_MSG(2, "Can not specify <src> and '-a' at the same time\n");
    return EXIT_FAILURE;
  }

  if (opt.mount_all) {
    if (!mountFstab(&opt)) return EXIT_FAILURE;
    else                   return EXIT_SUCCESS;
  }

  if (optind+2!=argc) {
    WRITE_MSG(2, "Invalid <src> <dst> pair specified\n");
    return EXIT_FAILURE;
  }

  if (mnt.data) {
    mnt.data = strdup(mnt.data);
    if (!transformOptionList(&mnt)) {
      WRITE_MSG(2, "Invalid options specified\n");
      return EXIT_FAILURE;
    }
  }
    
  mnt.src  = argv[optind++];
  mnt.dst  = argv[optind++];

  if (!mountSingle(&mnt, &opt)) return EXIT_FAILURE;
    
  return EXIT_SUCCESS;
}