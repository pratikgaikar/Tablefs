/*
 * Logging.cpp
 *
 *  Created on: Jul 24, 2011
 *      Author: kair
 */

#include "logging.h"
#include <sys/stat.h>

namespace tablefs {

Logging::~Logging() {
  if (logfile != NULL) {
    fflush(logfile);
    fclose(logfile);
  }
}

void Logging::Open() {
  if (logging_filename.size() > 0) {
    logfile = fopen(logging_filename.data(), "w");
  } else {
    logfile = NULL;
  }
}

void Logging::LogMsg(const char *format, ...)
{
  if (logfile != NULL) {
    va_list ap;
    va_start(ap, format);

    vfprintf(logfile, format, ap);
    fflush(logfile);
  }
}

void Logging::LogStat(const char *path, const struct stat *statbuf)
{
  if (logfile != NULL) {
    fprintf(logfile, "Stat of [%s]:\n", path);
    fprintf(logfile, "  inode[%d] mode[%d], uid[%d], gid[%d], size[%d]\n",
            statbuf->st_ino,
            statbuf->st_mode, statbuf->st_uid,
            statbuf->st_gid, statbuf->st_size);
    fprintf(logfile, "  atime[%d] mtime[%d] ctime[%d]\n",
            statbuf->st_atime, statbuf->st_mtime,
            statbuf->st_ctime);
    fflush(logfile);
  }
}


static Logging* log_ = NULL;

Logging* Logging::Default() {
  return log_;
}

void Logging::SetDefault(Logging* log) {
  log_ = log;
}

} 
