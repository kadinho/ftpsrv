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

#pragma once

#include <limits.h>
#include <stdint.h>
#include <time.h>


/**
 * Maximum number of copy jobs remembered by the server.
 **/
#define FERRY_MAX_JOBS 8


/**
 * Size of a hex-encoded SHA-256 digest, including the terminator.
 **/
#define SHA256_HEX_SIZE 65


/**
 * Life cycle of a copy job.
 **/
typedef enum ferry_state {
  FERRY_FREE = 0,
  FERRY_SCANNING,
  FERRY_COPYING,
  FERRY_DONE,
  FERRY_FAILED,
  FERRY_CANCELLED,
} ferry_state_t;


/**
 * Snapshot of a copy job, safe to read without holding any lock.
 **/
typedef struct ferry_job {
  int           id;
  ferry_state_t state;
  int           cancel;

  char src[PATH_MAX];
  char dst[PATH_MAX];
  char current[PATH_MAX];
  char error[256];

  uint64_t bytes_total;  // bytes in the source tree
  uint64_t bytes_needed; // bytes not yet present at the destination
  uint64_t bytes_done;   // bytes copied or skipped (already present)
  uint64_t files_total;
  uint64_t files_done;
  uint64_t files_skipped;

  time_t started;
  time_t finished;
} ferry_job_t;


/**
 * Start a background copy of SRC (file or directory tree) to DST.
 * Files already present at DST with the same size are skipped, so restarting
 * an interrupted job resumes it. Returns the job id, or -1 with errno set.
 **/
int ferry_copy_start(const char *src, const char *dst);


/**
 * Copy the state of job ID into JOB. Returns 0 on success, -1 if unknown.
 **/
int ferry_job_get(int id, ferry_job_t *job);


/**
 * Copy all known jobs into JOBS (FERRY_MAX_JOBS entries), oldest first.
 * Returns the number of jobs copied.
 **/
int ferry_job_list(ferry_job_t *jobs);


/**
 * Request cancellation of job ID. Returns 0 on success, -1 if unknown.
 **/
int ferry_job_cancel(int id);


/**
 * Human-readable name of a job state.
 **/
const char* ferry_state_name(ferry_state_t state);


/**
 * Sum the size of all regular files below PATH.
 **/
int ferry_du(const char *path, uint64_t *bytes, uint64_t *files,
	     uint64_t *dirs);


/**
 * Recursively remove PATH without following symlinks.
 **/
int ferry_rmtree(const char *path, uint64_t *files, uint64_t *dirs);


/**
 * Compute the SHA-256 digest of a file as a hex string (65 bytes).
 **/
int ferry_sha256(const char *path, char *hex);
