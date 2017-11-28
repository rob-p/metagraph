#ifndef __UNIX_TOOLS_HPP__
#define __UNIX_TOOLS_HPP__

/**
 * This header file collects some useful functions to find out about
 * system resources and stuff
 */

#include <stdio.h>


/** This returns the currently used memory by the process.
 *
 * The code was copied and has been modified from:
 * http://nadeausoftware.com/articles/2012/07/c_c_tip_how_get_process_resident_set_size_physical_memory_use
 */
size_t get_curr_mem2() {
    FILE *fp = NULL;

    if ( (fp = fopen( "/proc/self/statm", "r")) == NULL ) {
        return 0;      /* Can't open? */
    }
    long rss = 0L;
    if ( fscanf(fp, "%*s%ld", &rss) != 1 ) {
        fclose(fp);
        return 0;      /* Can't read? */
    }
    fclose(fp);

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 0 || rss < 0)
        return 0;

    return rss * page_size;
}


void get_RAM() {
    //output total RAM usage
    FILE *sfile = fopen("/proc/self/status", "r");
    char line[128];
    while (fgets(line, 128, sfile) != NULL) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            printf("%s", line);
            break;
        }
    }
    fclose(sfile);
}

#endif // __UNIX_TOOLS_HPP__
