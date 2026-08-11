#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int numeric_name(const char *name)
{
    if (!*name)
        return 0;
    for (; *name; name++)
        if (!isdigit((unsigned char)*name))
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    DIR *proc;
    struct dirent *entry;
    uid_t uid;
    unsigned int visible = 0;
    unsigned int markers = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s UID\n", argv[0]);
        return 2;
    }
    uid = (uid_t)strtoul(argv[1], NULL, 10);
    if (setgroups(0, NULL) || setgid(uid) || setuid(uid)) {
        perror("drop credentials");
        return 2;
    }

    proc = opendir("/proc");
    if (!proc) {
        perror("opendir /proc");
        return 2;
    }
    while ((entry = readdir(proc))) {
        char path[512];
        char comm[256];
        FILE *file;

        if (!numeric_name(entry->d_name))
            continue;
        visible++;
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        file = fopen(path, "re");
        if (!file)
            continue;
        if (fgets(comm, sizeof(comm), file) &&
            (strcasestr(comm, "zygisk") || strcasestr(comm, "magisk") ||
             !strncasecmp(comm, "zn-", 3))) {
            printf("LEAK pid=%s comm=%s", entry->d_name, comm);
            markers++;
        }
        fclose(file);
    }
    closedir(proc);
    printf("uid=%u visible_pids=%u marker_processes=%u\n", uid, visible, markers);
    return markers ? 1 : 0;
}
