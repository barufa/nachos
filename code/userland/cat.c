#include "syscall.h"
#define BSIZE 30

int
main(int arg, char * argv[])
{
    char bf[BSIZE];
    int i = -1;

    do {
        Read(&bf[++i], 1, CONSOLE_INPUT);
    } while (bf[i] != '\n' && bf[i] != '\0');
    Write(bf, i, CONSOLE_OUTPUT);
    Write("\n", 1, CONSOLE_OUTPUT);
    Exit(i);
}
