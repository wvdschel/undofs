#include "config.h"
#include "undofs_fops.h"
#include "undofs_util.h"

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <fuse.h>

int main(int argc, char *argv[])
{
    int fuse_stat;
    void *priv_data;

    if(argc < 3)
    {
        fprintf(stderr, "Usage: undofs [fuse options] <source root> <mountpoint>\n");
        exit(1);
    }

    priv_data = create_private_data(realpath(argv[argc-2], NULL));

    // Remove the source root from the arguments.
    argv[argc-2] = argv[argc-1];
    argv[argc-1] = NULL;
    argc--;

    fprintf(stderr, "Calling fuse_main.\n");
    fuse_stat = fuse_main(argc, argv, undofs_operations(), priv_data);
    fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
    
    return fuse_stat;
}
