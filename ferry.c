/* Copyright (C) 2026 kadinho

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "ferry.h"
#include "io.h"
#include "log.h"
#include "notify.h"
#include "sha256.h"


/**
 * Stack size for copy threads; the tree walk recurses once per directory.
 **/
#define FERRY_STACK_SIZE 0x100000


static ferry_job_t     g_jobs[FERRY_MAX_JOBS];
static int             g_next_id = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;


const char*
ferry_state_name(ferry_state_t state) {
  switch(state) {
  case FERRY_SCANNING:  return "scanning";
  case FERRY_COPYING:   return "copying";
  case FERRY_REMOVING:  return "removing";
  case FERRY_DONE:      return "done";
  case FERRY_FAILED:    return "failed";
  case FERRY_CANCELLED: return "cancelled";
  default:              return "free";
  }
}


const char*
ferry_kind_name(ferry_kind_t kind) {
  return kind == FERRY_REMOVE ? "remove" : "copy";
}


static int
ferry_is_running(ferry_state_t state) {
  return state == FERRY_SCANNING || state == FERRY_COPYING ||
         state == FERRY_REMOVING;
}


/**
 * Join a directory and an entry name into a newly allocated path.
 **/
static char*
ferry_join(const char *dir, const char *name) {
  size_t len = strlen(dir) + strlen(name) + 2;
  char *path;

  if(!(path=malloc(len))) {
    return NULL;
  }
  snprintf(path, len, "%s/%s", dir, name);

  return path;
}


static int
ferry_is_dot(const char *name) {
  return !strcmp(name, ".") || !strcmp(name, "..");
}


static void
ferry_set_error(ferry_job_t *job, const char *what, const char *path) {
  pthread_mutex_lock(&g_lock);
  snprintf(job->error, sizeof(job->error), "%s %s: %s", what, path,
	   strerror(errno));
  pthread_mutex_unlock(&g_lock);
}


static void
ferry_set_current(ferry_job_t *job, const char *path) {
  pthread_mutex_lock(&g_lock);
  snprintf(job->current, sizeof(job->current), "%s", path);
  pthread_mutex_unlock(&g_lock);
}


/**
 * Count the source tree and how much of it is missing at the destination.
 **/
static int
ferry_scan(ferry_job_t *job, const char *src, const char *dst) {
  struct dirent *ent;
  struct stat st;
  struct stat dst_st;
  char *s, *d;
  DIR *dir;
  int err = 0;

  if(job->cancel) {
    errno = ECANCELED;
    return -1;
  }

  if(lstat(src, &st)) {
    ferry_set_error(job, "lstat", src);
    return -1;
  }

  if(S_ISREG(st.st_mode)) {
    job->files_total++;
    job->bytes_total += st.st_size;
    if(lstat(dst, &dst_st) || !S_ISREG(dst_st.st_mode) ||
       dst_st.st_size != st.st_size) {
      job->bytes_needed += st.st_size;
    }
    return 0;
  }

  if(!S_ISDIR(st.st_mode)) {
    return 0;
  }

  if(!(dir=opendir(src))) {
    ferry_set_error(job, "opendir", src);
    return -1;
  }

  while(!err && (ent=readdir(dir))) {
    if(ferry_is_dot(ent->d_name)) {
      continue;
    }
    s = ferry_join(src, ent->d_name);
    d = ferry_join(dst, ent->d_name);
    if(!s || !d) {
      err = -1;
    } else {
      err = ferry_scan(job, s, d);
    }
    free(s);
    free(d);
  }

  closedir(dir);

  return err;
}


/**
 * Copy a single regular file, skipping it when DST already has the same size.
 **/
static int
ferry_copy_file(ferry_job_t *job, const char *src, const char *dst,
		const struct stat *st, void *buf) {
  struct stat dst_st;
  off_t copied = 0;
  ssize_t n;
  int fd_in;
  int fd_out;

  if(!lstat(dst, &dst_st) && S_ISREG(dst_st.st_mode) &&
     dst_st.st_size == st->st_size) {
    job->files_done++;
    job->files_skipped++;
    job->bytes_done += st->st_size;
    return 0;
  }

  ferry_set_current(job, src);

  if((fd_in=open(src, O_RDONLY, 0)) < 0) {
    ferry_set_error(job, "open", src);
    return -1;
  }

  if((fd_out=open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
    ferry_set_error(job, "open", dst);
    close(fd_in);
    return -1;
  }

  while(copied < st->st_size) {
    if(job->cancel) {
      errno = ECANCELED;
      close(fd_in);
      close(fd_out);
      return -1;
    }

    if((n=read(fd_in, buf, IO_COPY_BUFSIZE)) < 0) {
      if(errno == EINTR) {
	continue;
      }
      ferry_set_error(job, "read", src);
      close(fd_in);
      close(fd_out);
      return -1;
    }

    if(n == 0) {
      errno = EIO;
      ferry_set_error(job, "short read", src);
      close(fd_in);
      close(fd_out);
      return -1;
    }

    if(io_nwrite(fd_out, buf, n)) {
      ferry_set_error(job, "write", dst);
      close(fd_in);
      close(fd_out);
      return -1;
    }

    copied += n;
    job->bytes_done += n;
  }

  close(fd_in);

  if(fsync(fd_out) || fstat(fd_out, &dst_st)) {
    ferry_set_error(job, "fsync", dst);
    close(fd_out);
    return -1;
  }
  close(fd_out);

  if(dst_st.st_size != st->st_size) {
    errno = EIO;
    ferry_set_error(job, "size mismatch", dst);
    return -1;
  }

  chmod(dst, st->st_mode & 07777);
  job->files_done++;

  return 0;
}


/**
 * Recursively copy SRC to DST.
 **/
static int
ferry_copy_tree(ferry_job_t *job, const char *src, const char *dst,
		void *buf) {
  char target[PATH_MAX];
  struct dirent *ent;
  struct stat st;
  char *s, *d;
  ssize_t len;
  DIR *dir;
  int err = 0;

  if(job->cancel) {
    errno = ECANCELED;
    return -1;
  }

  if(lstat(src, &st)) {
    ferry_set_error(job, "lstat", src);
    return -1;
  }

  if(S_ISREG(st.st_mode)) {
    return ferry_copy_file(job, src, dst, &st, buf);
  }

  if(S_ISLNK(st.st_mode)) {
    if((len=readlink(src, target, sizeof(target)-1)) < 0) {
      ferry_set_error(job, "readlink", src);
      return -1;
    }
    target[len] = 0;
    if(symlink(target, dst) && errno != EEXIST) {
      ferry_set_error(job, "symlink", dst);
      return -1;
    }
    return 0;
  }

  if(!S_ISDIR(st.st_mode)) {
    return 0;
  }

  if(mkdir(dst, 0777) && errno != EEXIST) {
    ferry_set_error(job, "mkdir", dst);
    return -1;
  }

  if(!(dir=opendir(src))) {
    ferry_set_error(job, "opendir", src);
    return -1;
  }

  while(!err && (ent=readdir(dir))) {
    if(ferry_is_dot(ent->d_name)) {
      continue;
    }
    s = ferry_join(src, ent->d_name);
    d = ferry_join(dst, ent->d_name);
    if(!s || !d) {
      err = -1;
    } else {
      err = ferry_copy_tree(job, s, d, buf);
    }
    free(s);
    free(d);
  }

  closedir(dir);
  chmod(dst, st.st_mode & 07777);

  return err;
}


static void
ferry_finish(ferry_job_t *job, ferry_state_t state) {
  pthread_mutex_lock(&g_lock);
  job->state = state;
  job->finished = time(NULL);
  job->current[0] = 0;
  pthread_mutex_unlock(&g_lock);

  if(state == FERRY_DONE) {
    notify("ferry #%d %s done: %llu files, %llu MiB\n%s",
	   job->id, ferry_kind_name(job->kind),
	   (unsigned long long)job->files_done,
	   (unsigned long long)(job->bytes_done >> 20),
	   job->kind == FERRY_REMOVE ? job->src : job->dst);
  } else {
    notify("ferry #%d %s\n%s", job->id, ferry_state_name(state),
	   job->error[0] ? job->error : job->src);
  }
  FTP_LOG_PRINTF("ferry #%d %s: %s -> %s %s\n", job->id,
		 ferry_state_name(state), job->src, job->dst, job->error);
}


static void*
ferry_copy_thread(void *args) {
  ferry_job_t *job = args;
  struct statvfs vfs;
  char parent[PATH_MAX];
  uint64_t avail;
  void *buf;

  if(ferry_scan(job, job->src, job->dst)) {
    ferry_finish(job, job->cancel ? FERRY_CANCELLED : FERRY_FAILED);
    return NULL;
  }

  snprintf(parent, sizeof(parent), "%s", job->dst);
  if(!statvfs(dirname(parent), &vfs)) {
    avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    if(avail < job->bytes_needed) {
      snprintf(job->error, sizeof(job->error),
	       "not enough space: need %llu MiB, have %llu MiB",
	       (unsigned long long)(job->bytes_needed >> 20),
	       (unsigned long long)(avail >> 20));
      ferry_finish(job, FERRY_FAILED);
      return NULL;
    }
  }

  if(!(buf=malloc(IO_COPY_BUFSIZE))) {
    ferry_set_error(job, "malloc", "buffer");
    ferry_finish(job, FERRY_FAILED);
    return NULL;
  }

  pthread_mutex_lock(&g_lock);
  job->state = FERRY_COPYING;
  pthread_mutex_unlock(&g_lock);

  if(ferry_copy_tree(job, job->src, job->dst, buf)) {
    ferry_finish(job, job->cancel ? FERRY_CANCELLED : FERRY_FAILED);
  } else {
    ferry_finish(job, FERRY_DONE);
  }

  free(buf);

  return NULL;
}


/**
 * Find a free job slot, recycling the oldest finished job when needed.
 **/
static ferry_job_t*
ferry_alloc_job(void) {
  ferry_job_t *oldest = NULL;

  for(int i=0; i<FERRY_MAX_JOBS; i++) {
    if(g_jobs[i].state == FERRY_FREE) {
      return &g_jobs[i];
    }
    if(ferry_is_running(g_jobs[i].state)) {
      continue;
    }
    if(!oldest || g_jobs[i].finished < oldest->finished) {
      oldest = &g_jobs[i];
    }
  }

  return oldest;
}


/**
 * Allocate a job and run FN on it in a detached thread.
 **/
static int
ferry_job_start(ferry_kind_t kind, const char *src, const char *dst,
		void* (*fn)(void*)) {
  pthread_attr_t attr;
  ferry_job_t *job;
  pthread_t trd;
  int id;

  pthread_mutex_lock(&g_lock);
  if(!(job=ferry_alloc_job())) {
    pthread_mutex_unlock(&g_lock);
    errno = EBUSY;
    return -1;
  }

  memset(job, 0, sizeof(*job));
  job->id = id = g_next_id++;
  job->kind = kind;
  job->state = FERRY_SCANNING;
  job->started = time(NULL);
  snprintf(job->src, sizeof(job->src), "%s", src);
  snprintf(job->dst, sizeof(job->dst), "%s", dst);
  pthread_mutex_unlock(&g_lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, FERRY_STACK_SIZE);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if(pthread_create(&trd, &attr, fn, job)) {
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&g_lock);
    job->state = FERRY_FREE;
    pthread_mutex_unlock(&g_lock);
    errno = EAGAIN;
    return -1;
  }
  pthread_attr_destroy(&attr);

  return id;
}


int
ferry_copy_start(const char *src, const char *dst) {
  char src_real[PATH_MAX];
  char dst_parent[PATH_MAX];
  char dst_real[PATH_MAX];
  char dst_base[PATH_MAX];
  size_t len;

  if(!realpath(src, src_real)) {
    return -1;
  }

  // the destination may not exist yet, so resolve its parent instead
  snprintf(dst_parent, sizeof(dst_parent), "%s", dst);
  snprintf(dst_base, sizeof(dst_base), "%s", dst);
  if(!realpath(dirname(dst_parent), dst_real)) {
    return -1;
  }
  len = strlen(dst_real);
  snprintf(dst_real + len, sizeof(dst_real) - len, "/%s", basename(dst_base));

  // refuse to copy a tree into itself
  len = strlen(src_real);
  if(!strncmp(src_real, dst_real, len) &&
     (dst_real[len] == '/' || dst_real[len] == 0)) {
    errno = EINVAL;
    return -1;
  }

  return ferry_job_start(FERRY_COPY, src_real, dst_real, ferry_copy_thread);
}


/**
 * Recursively remove PATH, updating the job's progress as files go.
 **/
static int
ferry_remove_tree(ferry_job_t *job, const char *path) {
  struct dirent *ent;
  struct stat st;
  DIR *dir;
  char *p;
  int err = 0;

  if(job->cancel) {
    errno = ECANCELED;
    return -1;
  }

  if(lstat(path, &st)) {
    ferry_set_error(job, "lstat", path);
    return -1;
  }

  if(!S_ISDIR(st.st_mode)) {
    ferry_set_current(job, path);
    if(unlink(path)) {
      ferry_set_error(job, "unlink", path);
      return -1;
    }
    if(S_ISREG(st.st_mode)) {
      job->files_done++;
      job->bytes_done += st.st_size;
    }
    return 0;
  }

  if(!(dir=opendir(path))) {
    ferry_set_error(job, "opendir", path);
    return -1;
  }

  while(!err && (ent=readdir(dir))) {
    if(ferry_is_dot(ent->d_name)) {
      continue;
    }
    if(!(p=ferry_join(path, ent->d_name))) {
      err = -1;
    } else {
      err = ferry_remove_tree(job, p);
      free(p);
    }
  }

  closedir(dir);
  if(err) {
    return err;
  }

  if(rmdir(path)) {
    ferry_set_error(job, "rmdir", path);
    return -1;
  }

  return 0;
}


static void*
ferry_remove_thread(void *args) {
  ferry_job_t *job = args;
  uint64_t dirs = 0;

  if(ferry_du(job->src, &job->bytes_total, &job->files_total, &dirs)) {
    ferry_set_error(job, "scan", job->src);
    ferry_finish(job, FERRY_FAILED);
    return NULL;
  }

  pthread_mutex_lock(&g_lock);
  job->state = FERRY_REMOVING;
  pthread_mutex_unlock(&g_lock);

  if(ferry_remove_tree(job, job->src)) {
    ferry_finish(job, job->cancel ? FERRY_CANCELLED : FERRY_FAILED);
  } else {
    ferry_finish(job, FERRY_DONE);
  }

  return NULL;
}


int
ferry_remove_start(const char *path) {
  struct stat st;

  if(lstat(path, &st)) {
    return -1;
  }

  return ferry_job_start(FERRY_REMOVE, path, "", ferry_remove_thread);
}


static ferry_job_t*
ferry_find_job(int id) {
  for(int i=0; i<FERRY_MAX_JOBS; i++) {
    if(g_jobs[i].state != FERRY_FREE && g_jobs[i].id == id) {
      return &g_jobs[i];
    }
  }

  return NULL;
}


int
ferry_job_get(int id, ferry_job_t *job) {
  ferry_job_t *j;

  pthread_mutex_lock(&g_lock);
  if((j=ferry_find_job(id))) {
    memcpy(job, j, sizeof(*job));
  }
  pthread_mutex_unlock(&g_lock);

  return j ? 0 : -1;
}


int
ferry_job_list(ferry_job_t *jobs) {
  ferry_job_t tmp;
  int count = 0;

  pthread_mutex_lock(&g_lock);
  for(int i=0; i<FERRY_MAX_JOBS; i++) {
    if(g_jobs[i].state != FERRY_FREE) {
      memcpy(&jobs[count++], &g_jobs[i], sizeof(ferry_job_t));
    }
  }
  pthread_mutex_unlock(&g_lock);

  // slots are recycled, so order by id
  for(int i=1; i<count; i++) {
    for(int j=i; j>0 && jobs[j-1].id > jobs[j].id; j--) {
      memcpy(&tmp, &jobs[j], sizeof(tmp));
      memcpy(&jobs[j], &jobs[j-1], sizeof(tmp));
      memcpy(&jobs[j-1], &tmp, sizeof(tmp));
    }
  }

  return count;
}


int
ferry_job_cancel(int id) {
  ferry_job_t *j;

  pthread_mutex_lock(&g_lock);
  if((j=ferry_find_job(id))) {
    j->cancel = 1;
  }
  pthread_mutex_unlock(&g_lock);

  return j ? 0 : -1;
}


int
ferry_du(const char *path, uint64_t *bytes, uint64_t *files, uint64_t *dirs) {
  struct dirent *ent;
  struct stat st;
  DIR *dir;
  char *p;
  int err = 0;

  if(lstat(path, &st)) {
    return -1;
  }

  if(S_ISREG(st.st_mode)) {
    *bytes += st.st_size;
    *files += 1;
    return 0;
  }

  if(!S_ISDIR(st.st_mode)) {
    return 0;
  }

  *dirs += 1;
  if(!(dir=opendir(path))) {
    return -1;
  }

  while(!err && (ent=readdir(dir))) {
    if(ferry_is_dot(ent->d_name)) {
      continue;
    }
    if(!(p=ferry_join(path, ent->d_name))) {
      err = -1;
    } else {
      err = ferry_du(p, bytes, files, dirs);
      free(p);
    }
  }

  closedir(dir);

  return err;
}


int
ferry_sha256(const char *path, char *hex) {
  uint8_t digest[SHA256_BLOCK_SIZE];
  SHA256_CTX ctx;
  void *buf;
  ssize_t n;
  int fd;

  if((fd=open(path, O_RDONLY, 0)) < 0) {
    return -1;
  }

  if(!(buf=malloc(IO_COPY_BUFSIZE))) {
    close(fd);
    return -1;
  }

  sha256_init(&ctx);
  while((n=read(fd, buf, IO_COPY_BUFSIZE)) != 0) {
    if(n < 0) {
      if(errno == EINTR) {
	continue;
      }
      free(buf);
      close(fd);
      return -1;
    }
    sha256_update(&ctx, buf, n);
  }
  sha256_final(&ctx, digest);

  free(buf);
  close(fd);

  for(int i=0; i<SHA256_BLOCK_SIZE; i++) {
    sprintf(hex + i*2, "%02x", digest[i]);
  }

  return 0;
}


/*
  Local Variables:
  c-file-style: "gnu"
  End:
*/
