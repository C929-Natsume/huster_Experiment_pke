// #include "user_lib.h"
// #include "string.h"
// #include "util/types.h"

// #define MAXLINE 256
// #define MAXARG 64

// /* 从 fd 读一行到 line，返回读到的长度，0 表示 EOF/结束 */
// static int read_line(int fd, char *line, int max)
// {
//     int i = 0;
//     while (i < max - 1)
//     {
//         int n = read_u(fd, line + i, 1);
//         if (n <= 0)
//             break;
//         if (line[i] == '\n')
//         {
//             line[i] = '\0';
//             return i;
//         }
//         i++;
//     }
//     line[i] = '\0';
//     return i;
// }

// /* 把一行拆成 command + 可选参数，解析结果放在 cmd 和 arg 里 */
// static void parse_cmd_arg(char *line, char **cmd, char **arg)
// {
//     *cmd = line;
//     *arg = NULL;
//     while (*line && *line != ' ')
//         line++;
//     if (*line)
//     {
//         *line = '\0';
//         line++;
//         if (*line)
//             *arg = line;
//     }
// }

// static void run_cmd(const char *cmd, const char *arg)
// {
//     if (!cmd || !*cmd)
//         return;
//     if (strcmp(cmd, "exit") == 0)
//     {
//         exit(0);
//         return;
//     }
//     if (strcmp(cmd, "base_test") == 0)
//     {
//         exec("/bin/app_print_backtrace", "");
//         exec("/bin/app_errorline", "");
//         return;
//     }
//     if (strcmp(cmd, "print_backtrace") == 0)
//     {
//         exec("/bin/app_print_backtrace", arg ? arg : "");
//         return;
//     }
//     if (strcmp(cmd, "errorline") == 0)
//     {
//         exec("/bin/app_errorline", arg ? arg : "");
//         return;
//     }
//     /* 可继续加：ls, cat, mkdir 等，用 exec("/bin/app_xxx", arg); */
//     printu("unknown command: %s\n", cmd);
// }

// int main(int argc, char *argv[])
// {
//     printu("=========================PKE=========================\n");

//     int stdin_fd = 0; /* 若内核把 stdin 绑到 0，直接用 0；否则用 open("/dev/stdin") 的 fd */
//     char line[MAXLINE];
//     char *cmd, *arg;

//     while (1)
//     {
//         printu("> ");
//         int len = read_line(stdin_fd, line, MAXLINE);
//         if (len <= 0)
//             break;
//         parse_cmd_arg(line, &cmd, &arg);
//         run_cmd(cmd, arg);
//     }

//     exit(0);
//     return 0;
// }