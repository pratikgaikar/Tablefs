/*
 * Properties.cpp
 *
 *  Created on: Jul 19, 2011
 *      Author: kair
 */

#include "util/properties.h"
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace tablefs {

Properties::Properties() {
}

Properties::~Properties() {
}

void Properties::setProperty(const std::string &key, std::string value) {
  plist[key] = value;
}

void Properties::setPropertyInt(const std::string &key, int value) {
  char buf[32];
  sprintf(buf, "%d", value);
  plist[key] = std::string(buf);
}

std::string Properties::getProperty(const std::string &key) {
  std::map<std::string, std::string>::iterator it = plist.find(key);
  if (it != plist.end()) {
    return it->second;
  } else {
    return std::string("/");
  }
}

std::string Properties::getProperty(const std::string &key,
                                    std::string defaultVal) {
  const std::map<std::string, std::string>::iterator it = plist.find(key);
  if (it != plist.end()) {
    return it->second;
  } else {
    return defaultVal;
  }
}

int Properties::getPropertyInt(const std::string &key, int defaultVal) {
  const std::map<std::string, std::string>::iterator it = plist.find(key);
  if (it != plist.end()) {
    return atoi(it->second.data());
  } else {
    return defaultVal;
  }
}

double Properties::getPropertyDouble(const std::string &key,
                                     double defaultVal) {
  std::map<std::string, std::string>::iterator it = plist.find(key);
  if (it != plist.end()) {
    return atof(it->second.data());
  } else {
    return defaultVal;
  }
}

bool Properties::getPropertyBool(const std::string &key,
                                 bool defaultVal) {
  std::map<std::string, std::string>::iterator it = plist.find(key);
  if (it != plist.end()) {
    if (it->second.compare("true") == 0) {
      return true;
    } else {
      return false;
    }
  } else {
    return defaultVal;
  }
}

void Properties::load(const std::string &filename) {
  std::ifstream is;
  char line[1024];
  is.open(filename);
  while (is.good()) {
    is.getline(line, 1024);
    char* pch = strchr(line, '=');
    if (pch != NULL) {
      std::string key = std::string(line, pch-line);
      std::string val = std::string(pch+1, strlen(pch+1));
      printf("%s %s\n", key.c_str(), val.c_str());
      plist[key] = val;
    }
  }
  is.close();
}

void Properties::store(const std::string &filename) {
  std::ofstream os;
  os.open(filename.data());
  if (os.good()) {
    std::map<std::string, std::string>::iterator it;
    for (it = plist.begin(); it != plist.end(); it++) {
      os << it->first << '=' << it->second << std::endl;
    }
  }
  os.close();
}

void Properties::parseOpts(int argc, char *argv[]) {
   for (int i = 1; i < argc; ++i)
    if (argv[i][0] == '-') {
      setProperty(std::string(argv[i]+1, strlen(argv[i]+1)),
                  std::string(argv[i+1], strlen(argv[i+1])));
      printf("%s %s\n", argv[i]+1, argv[i+1]);
      ++i;
    }
}

void Properties::Report(FILE* logf) {
  std::map<std::string, std::string>::iterator it;
  for (it = plist.begin(); it != plist.end(); it++) {
    fprintf(logf, "%s %s\n", it->first.c_str(), it->second.c_str());
  }
}

Properties& Properties::operator=(const Properties &oldp) {
  plist = oldp.plist;
  return *this;
}

}
