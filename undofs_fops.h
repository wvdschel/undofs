#ifndef __UNDOFS_FOPS_H_
#define __UNDOFS_FOPS_H_
#include "config.h"

#include <fuse.h>

/**
 * @return pointer to a fuse_operations structure with all the undofs operations filled in.
 */
struct fuse_operations *undofs_operations();

#endif
