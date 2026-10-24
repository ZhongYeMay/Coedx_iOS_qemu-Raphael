#include <assert.h>
#include <stdio.h>
#include "raphael_ufs_policy.h"
int main(void)
{
    assert(!raphael_ufs_enabled(NULL));
    assert(!raphael_ufs_enabled(""));
    assert(!raphael_ufs_enabled("xraphael_ufs=1"));
    assert(!raphael_ufs_enabled("raphael_ufs=10"));
    assert(!raphael_ufs_enabled("raphael_ufs=0"));
    assert(raphael_ufs_enabled("raphael_ufs=1"));
    assert(raphael_ufs_enabled("ufs_write=1\traphael_ufs=1 cpus=8"));
    int ufs=0, write=1, native=1, probe=1;
    raphael_ufs_loader_policy(&ufs, &write, &native, &probe);
    assert(ufs==2 && write==0 && native==0 && probe==0);
    puts("PASS: exact tokens and conflicting write/native options suppressed");
    return 0;
}
