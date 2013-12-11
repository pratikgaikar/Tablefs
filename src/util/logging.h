/*
 * Logging.h
 *
 *  Created on: Jul 24, 2011
 *      Author: kair
 */

#ifndef LOGGING_H_
#define LOGGING_H_

#include <string>
#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>

namespace tablefs {

class Logging {
public:
  Logging() : logging_filename("/tmp/tmp.log") {}

  Logging(const char *path) : logging_filename(path) {}

  Logging(const std::string &path) : logging_filename(path) {}

  void Open();

  void LogMsg(const char *format, ...);

  void LogStat(const char* path, const struct stat* statbuf);

  virtual ~Logging();

  static Logging* Default();

  void SetDefault(Logging* log);

private:
  std::string logging_filename;
  FILE *logfile;
};

}

#endif /* LOGGING_H_ */
