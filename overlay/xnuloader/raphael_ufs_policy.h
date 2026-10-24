/* Experimental raphael UEFI-backed UFS bring-up policy. No MMIO addresses assumed. */
#ifndef RAPHAEL_UFS_POLICY_H
#define RAPHAEL_UFS_POLICY_H

/* Exact token matching; reject values such as raphael_ufs=10. */
static inline int raphael_ufs_enabled(const char *args)
{
    const char token[] = "raphael_ufs=1";
    if (!args) return 0;
    while (*args) {
        unsigned i = 0;
        while (*args == ' ' || *args == '\t') ++args;
        while (token[i] && args[i] == token[i]) ++i;
        if (!token[i] && (!args[i] || args[i] == ' ' || args[i] == '\t')) return 1;
        while (*args && *args != ' ' && *args != '\t') ++args;
    }
    return 0;
}

/* Apply after upstream argument parsing, before any storage enumeration. */
static inline void raphael_ufs_loader_policy(int *ufs, int *write, int *native, int *probe)
{
    *ufs = 2; /* Enumerate Block I/O and publish upstream mailbox. */
    *write = 0; /* Do not inherit redwood's partition identity or write offsets. */
    *native = 0; /* No redwood controller MMIO or descriptor takeover. */
    *probe = 0; /* No device-specific async benchmark. */
}
#endif
