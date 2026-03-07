#include "user/user_lib.h"
#include "util/string.h"
#include "util/snprintf.h"
#include "util/types.h"

void execu(char *path, char *arg)
{
    int pid = fork();
    if (pid != 0)
    {
        wait(pid);
        return;
    }
    exec(path, arg);
}

int main(int argc, char *argv[])
{
    printu("PKE-START\n\n");
    char *cmd = (char *)better_malloc(100);
    while (1)
    {
        scanfu("%s", cmd);
        if (strcmp(cmd, "trace") == 0)
        {
            execu("/bin/app_print_backtrace", "");
        }
        else if (strcmp(cmd, "error") == 0)
        {
            execu("/bin/app_errorline", "");
        }
        else if (strcmp(cmd, "2core") == 0)
        {
            // int pid = fork();
            // if (pid != 0)
            // {
            //     execu("/bin/app0", "");
            // }
            // else
            // {
            //     execu("/bin/app1", "");
            // }
        }
        else if (strcmp(cmd, "error_sum") == 0)
        {
            execu("/bin/app_sum_sequence", "");
        }
        else if (strcmp(cmd, "alloc") == 0)
        {
            execu("/bin/app_singlepageheap", "");
        }
        else if (strcmp(cmd, "2alloc") == 0)
        {
            // int pid = fork();
            // if (pid != 0)
            // {
            //     execu("/bin/app_alloc0", "");
            // }
            // else
            // {
            //     execu("/bin/app_alloc1", "");
            // }
        }
        else if (strcmp(cmd, "wait") == 0)
        {
            execu("/bin/app_wait", "");
        }
        else if (strcmp(cmd, "sem") == 0)
        {
            execu("/bin/app_semaphore", "");
        }
        else if (strcmp(cmd, "cow") == 0)
        {
            execu("/bin/app_cow", "");
        }
        else if (strcmp(cmd, "path") == 0)
        {
            execu("/bin/app_relativepath", "");
        }
        else if (strcmp(cmd, "exec") == 0)
        {
            execu("/bin/app_exec", "");
        }
        else if (strcmp(cmd, "shell_test") == 0)
        {
            execu("/bin/app_shell", "");
        }
        else if (strcmp(cmd, "exit") == 0)
        {
            printu("Exit.\n");
            break;
        }
        else
        {
            printu("Error command: %s\n", cmd);
        }
    }
    exit(0);
    return 0;
}