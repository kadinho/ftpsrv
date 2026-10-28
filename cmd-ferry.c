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

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#include "cmd.h"
#include "ferry.h"


/**
 * Minimum number of path components RMTREE accepts, so that e.g. /data or
 * /mnt/ext1 can never be wiped by a typo.
 **/
#define FERRY_RMTREE_MIN_DEPTH 3


/**
 * Write one status line for a job.
 **/
static int
ftp_ferry_print_job(ftp_env_t *env, const ferry_job_t *job, int last) {
  time_t end = job->finished ? job->finished : time(NULL);
  unsigned long long pct = 100;

  if(job->bytes_total) {
    pct = (unsigned long long)(job->bytes_done * 100 / job->bytes_total);
  }

  return ftp_active_printf(env,
			   "211%cjob=%d;state=%s;pct=%llu;bytes=%llu/%llu;"
			   "needed=%llu;files=%llu/%llu;skipped=%llu;secs=%lld;"
			   "src=%s;dst=%s;current=%s;error=%s\r\n",
			   last ? ' ' : '-', job->id,
			   ferry_state_name(job->state), pct,
			   (unsigned long long)job->bytes_done,
			   (unsigned long long)job->bytes_total,
			   (unsigned long long)job->bytes_needed,
			   (unsigned long long)job->files_done,
			   (unsigned long long)job->files_total,
			   (unsigned long long)job->files_skipped,
			   (long long)(end - job->started),
			   job->src, job->dst, job->current, job->error);
}


/**
 * Specify the source path of a server-side copy started by CPTO.
 **/
int
ftp_cmd_CPFR(ftp_env_t *env, const char* arg) {
  struct stat st;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: CPFR <PATH>\r\n");
  }

  ftp_abspath(env, env->copy_path, arg);
  if(lstat(env->copy_path, &st)) {
    env->copy_path[0] = 0;
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "350 Awaiting copy destination\r\n");
}


/**
 * Start a background copy of the path given by CPFR.
 **/
int
ftp_cmd_CPTO(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  int id;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: CPTO <PATH>\r\n");
  }

  if(!env->copy_path[0]) {
    return ftp_active_printf(env, "503 Use CPFR first\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  id = ferry_copy_start(env->copy_path, pathbuf);
  env->copy_path[0] = 0;

  if(id < 0) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "250 Copy job %d started\r\n", id);
}


/**
 * Report the progress of one job, or of all jobs when no id is given.
 **/
int
ftp_cmd_JOBS(ftp_env_t *env, const char* arg) {
  ferry_job_t jobs[FERRY_MAX_JOBS];
  int count;

  if(arg[0]) {
    if(ferry_job_get(atoi(arg), &jobs[0])) {
      return ftp_active_printf(env, "550 No such job\r\n");
    }
    return ftp_ferry_print_job(env, &jobs[0], 1);
  }

  if(!(count=ferry_job_list(jobs))) {
    return ftp_active_printf(env, "211 No jobs\r\n");
  }

  for(int i=0; i<count; i++) {
    if(ftp_ferry_print_job(env, &jobs[i], i == count-1)) {
      return -1;
    }
  }

  return 0;
}


/**
 * Ask a running job to stop.
 **/
int
ftp_cmd_CANCEL(ftp_env_t *env, const char* arg) {
  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: CANCEL <JOB>\r\n");
  }

  if(ferry_job_cancel(atoi(arg))) {
    return ftp_active_printf(env, "550 No such job\r\n");
  }

  return ftp_active_printf(env, "200 Cancel requested\r\n");
}


/**
 * Report free and total space of the file system holding a path.
 **/
int
ftp_cmd_DF(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  struct statvfs vfs;

  ftp_abspath(env, pathbuf, arg[0] ? arg : env->cwd);
  if(statvfs(pathbuf, &vfs)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "213 avail=%llu;free=%llu;total=%llu\r\n",
			   (unsigned long long)vfs.f_bavail * vfs.f_frsize,
			   (unsigned long long)vfs.f_bfree * vfs.f_frsize,
			   (unsigned long long)vfs.f_blocks * vfs.f_frsize);
}


/**
 * Report the recursive size of a path.
 **/
int
ftp_cmd_DU(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  uint64_t bytes = 0;
  uint64_t files = 0;
  uint64_t dirs = 0;

  ftp_abspath(env, pathbuf, arg[0] ? arg : env->cwd);
  if(ferry_du(pathbuf, &bytes, &files, &dirs)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "213 bytes=%llu;files=%llu;dirs=%llu\r\n",
			   (unsigned long long)bytes,
			   (unsigned long long)files,
			   (unsigned long long)dirs);
}


/**
 * Recursively delete a path.
 **/
int
ftp_cmd_RMTREE(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  uint64_t files = 0;
  uint64_t dirs = 0;
  int depth = 0;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: RMTREE <PATH>\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  if(strstr(pathbuf, "/..") || strstr(pathbuf, "/./")) {
    return ftp_active_printf(env, "553 Path must be canonical\r\n");
  }

  for(char *p=pathbuf; *p; p++) {
    if(*p == '/' && p[1] && p[1] != '/') {
      depth++;
    }
  }
  if(depth < FERRY_RMTREE_MIN_DEPTH) {
    return ftp_active_printf(env, "553 Refusing to remove a path shallower "
			     "than %d levels\r\n", FERRY_RMTREE_MIN_DEPTH);
  }

  if(ferry_rmtree(pathbuf, &files, &dirs)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "250 Removed %llu files, %llu dirs\r\n",
			   (unsigned long long)files,
			   (unsigned long long)dirs);
}


/**
 * Compute the SHA-256 digest of a file.
 **/
int
ftp_cmd_HASH(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  char hex[SHA256_HEX_SIZE];

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: HASH <FILENAME>\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  if(ferry_sha256(pathbuf, hex)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "213 SHA-256 %s\r\n", hex);
}


/*
  Local Variables:
  c-file-style: "gnu"
  End:
*/
