/*
 * Properties.h
 *
 *  Created on: Jul 19, 2011
 *      Author: kair
 */

#ifndef PROPERTIES_H_
#define PROPERTIES_H_

#include <string>
#include <map>

namespace tablefs {

class Properties {
public:
  explicit Properties();
  virtual ~Properties();

  Properties & operator=(const Properties &oldp);

  std::string getProperty(const std::string &key);

  std::string getProperty(const std::string &key, std::string defaultValue);

  int getPropertyInt(const std::string &key, int defaultValue=0);

  double getPropertyDouble(const std::string &key, double defaultValue=0);

  bool getPropertyBool(const std::string &key, bool defaultValue=false);

  void setProperty(const std::string &key, std::string value);

  void setPropertyInt(const std::string &key, int value);

  void load(const std::string &filename);

  void store(const std::string &filename);

  void parseOpts(int argc, char *argv[]);

  void Report(FILE* logf);

private:
  std::map<std::string, std::string> plist;
};

}

#endif /* PROPERTIES_H_ */
