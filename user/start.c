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
    printu("START\n\n");
    char cmd[32];
    while (1)
    {
        scanfu("%s", cmd);
        if (cmd[0] == 't' && cmd[1] == 'e' && cmd[2] == 's' && cmd[3] == 't')
        {
            char *p = cmd;
            p = cmd + 4;
            if (strcmp(p, "3") == 0)
            {
                execu("/bin/app_errorline", "");
            }
            // else if (strcmp(p, "1") == 0)
            // {
            //     int pid0 = fork();
            //     if (pid0 == 0)
            //     {
            //         exec("/bin/app0", "");
            //         exit(-1);
            //     }
            //     int pid1 = fork();
            //     if (pid1 == 0)
            //     {
            //         exec("/bin/app1", "");
            //         exit(-1);
            //     }
            //     wait(pid0);
            //     wait(pid1);
            // }
            else if (strcmp(p, "4") == 0)
            {
                execu("/bin/app_sum_sequence", "");
            }
            else if (strcmp(p, "2") == 0)
            {
                int pid0 = fork();
                if (pid0 == 0)
                {
                    exec("/bin/app_alloc0", "");
                    exit(-1);
                }
                int pid1 = fork();
                if (pid1 == 0)
                {
                    exec("/bin/app_alloc1", "");
                    exit(-1);
                }
                wait(pid0);
                wait(pid1);
            }

            else if (strcmp(p, "0") == 0)
            {
                execu("/bin/app_shell", "");
            }
        }
        else if (strcmp(cmd, "pwd") == 0)
        {
            char path[30];
            read_cwd(path);
            printu("cwd:%s\n", path);
        }
        else if (strcmp(cmd, "cd") == 0)
        {
            char p[32];
            scanfu("%s", p);
            change_cwd(p);
        }
        else if (strcmp(cmd, "cat") == 0)
        {
            char p[32];
            scanfu("%s", p);
            execu("/bin/app_cat", p);
        }
        else if (strcmp(cmd, "echo") == 0)
        {
            char p[32];
            scanfu("%s", p);
            execu("/bin/app_echo", p);
        }
        else if (strcmp(cmd, "ls") == 0)
        {
            char p[32];
            scanfu("%s", p);
            execu("/bin/app_ls", p);
        }
        else if (strcmp(cmd, "mkdir") == 0)
        {
            char p[32];
            scanfu("%s", p);
            execu("/bin/app_mkdir", p);
        }
        else if (strcmp(cmd, "touch") == 0)
        {
            char p[32];
            scanfu("%s", p);
            execu("/bin/app_touch", p);
        }
        else if (strcmp(cmd, "q") == 0)
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