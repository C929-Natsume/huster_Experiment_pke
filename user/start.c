#include "user/user_lib.h"
#include "util/string.h"

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

int main()
{
    char cmd[24];
    char arg[20];
    printu("\n============================START============================\n\n");

    while (1)
    {
        printu("$ ");
        scanfu("%s", cmd);

        if (cmd[0] == 'q' && cmd[1] == 0)
        {
            printu("Exit.\n");
            break;
        }

        if (!cmd[0])
        {
            continue;
        };

        if (cmd[0] == 't' && cmd[1] == 'e' && cmd[2] == 's' && cmd[3] == 't')
        {
            char *p = cmd + 4;
            if (p[0] == '2' && p[1] == 0)
                execu("/bin/app_errorline", "");
            else if (p[0] == '3' && p[1] == 0)
                execu("/bin/app_sum_sequence", "");
            else if (p[0] == '1' && p[1] == 0)
            {
                int pid0 = fork();
                if (pid0 == 0)
                    exec("/bin/app_alloc0", "");
                else
                {
                    int pid1 = fork();
                    if (pid1 == 0)
                        exec("/bin/app_alloc1", "");
                    else
                    {
                        wait(pid0);
                        wait(pid1);
                    }
                }
            }
            else if (p[0] == '4' && p[1] == 0)
            {
                int pid0 = fork();
                if (pid0 == 0)
                    exec("/bin/app0", "");
                else
                {
                    int pid1 = fork();
                    if (pid1 == 0)
                        exec("/bin/app1", "");
                    else
                    {
                        wait(pid0);
                        wait(pid1);
                    }
                }
            }
            else if (p[0] == '0' && p[1] == 0)
                execu("/bin/app_shell", "");
            else
                printu("Error command: %s\n", cmd);
            continue;
        }

        // pwd
        if (cmd[0] == 'p' && cmd[1] == 'w' && cmd[2] == 'd' && cmd[3] == 0)
        {
            char path[30];
            read_cwd(path);
            printu("cwd:%s\n", path);
            continue;
        }

        // cd
        if (cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == 0)
        {
            printu(">");
            scanfu("%s", arg);
            change_cwd(arg);
            continue;
        }

        // cat
        if (cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 't' && cmd[3] == 0)
        {
            printu(">");
            scanfu("%s", arg);
            execu("/bin/app_cat", arg);
            continue;
        }

        // echo
        if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == 0)
        {
            printu(">");
            scanfu("%s", arg);
            execu("/bin/app_echo", arg);
            continue;
        }

        // ls
        if (cmd[0] == 'l' && cmd[1] == 's' && cmd[2] == 0)
        {
            printu(">");
            scanfu("%s", arg);
            execu("/bin/app_ls", arg);
            continue;
        }

        // mkdir
        if (cmd[0] == 'm' && cmd[1] == 'k' && cmd[2] == 'd' && cmd[3] == 'i' && cmd[4] == 'r' && cmd[5] == 0)
        {
            printu(">");
            scanfu("%s", arg);
            execu("/bin/app_mkdir", arg);
            continue;
        }

        // touch
        if (cmd[0] == 't' && cmd[1] == 'o' && cmd[2] == 'u' && cmd[3] == 'c' && cmd[4] == 'h' && cmd[5] == 0)
        {
            printu(">");
            scanfu("%s", arg);
            execu("/bin/app_touch", arg);
            continue;
        }

        printu("Error command: %s\n", cmd);
    }

    exit(0);
    return 0;
}