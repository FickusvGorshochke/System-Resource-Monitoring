#include <stdio.h>
#include <unistd.h>
#include <sys/neutrino.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Использование: %s <программа> [аргументы...]\n", argv[0]);
        return 1;
    }

    int d[3] = { 1, 1, 1 };
    if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, d) == -1)
        perror("ThreadCtl RUNMASK_INHERIT");

    execvp(argv[1], &argv[1]);
    perror("execvp");
    return 1;
}
