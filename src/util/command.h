#ifndef _COMMAND_H_
#define _COMMAND_H_

namespace command {

extern int MountDevice(const char* partition, const char *mountpoint);

extern int UmountDevice(const char* partition, const char *mountpoint);

extern int DropBufferCache();

}

#endif
