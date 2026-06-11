#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

/* ─── globals ─────────────────────────────────────────────── */
char prompt[256]    = "hello";   /* feature 1  */
int  last_status    = 0;         /* feature 2  */
char last_cmd[1024] = "";        /* features 4,5 */
pid_t last_bg_pid   = -1;

/* ─── feature 12: SIGCHLD handler ────────────────────────── */
void sigchld_handler(int sig) {
    int status;
    pid_t pid;
    /* reap any finished background children */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("\nChild %d finished with exit status %d\n",
               pid, status >> 8);
        fflush(stdout);
    }
}

/* ─── feature 10: SIGINT handler ─────────────────────────── */
void sigint_handler(int sig) {
    printf("\nYou typed Control-C!\n");
    printf("%s: ", prompt);
    fflush(stdout);
}

/* ─── parse a single command string into argv ────────────── */
int parse_argv(char *cmdstr, char **argv) {
    int i = 0;
    char *token = strtok(cmdstr, " ");
    while (token != NULL) {
        argv[i++] = token;
        token = strtok(NULL, " ");
    }
    argv[i] = NULL;
    return i;
}

/* ─── execute one segment (handles redirect flags) ───────── */
void exec_segment(char **argv, int in_fd, int out_fd, int err_fd) {
    if (in_fd != STDIN_FILENO) {
        dup2(in_fd,  STDIN_FILENO);
        close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
    }
    if (err_fd != STDERR_FILENO) {
        dup2(err_fd, STDERR_FILENO);
        close(err_fd);
    }
    execvp(argv[0], argv);
    perror(argv[0]);
    exit(1);
}

/* ─── feature 11: pipeline ───────────────────────────────── */
void run_pipeline(char *segments[], int nseg, int in_fd, int out_fd, int err_fd) {
    int i;
    int prev_read = in_fd;   /* read-end carried from previous pipe */

    for (i = 0; i < nseg; i++) {
        /* parse this segment's argv */
        char buf[1024];
        strncpy(buf, segments[i], sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
        char *argv[64];
        parse_argv(buf, argv);
        if (argv[0] == NULL) continue;

        int pipefd[2];
        int this_out;

        if (i < nseg - 1) {
            /* not the last command → create a new pipe */
            pipe(pipefd);
            this_out = pipefd[1];   /* write end goes to child */
        } else {
            /* last command → write to the final out_fd */
            this_out = out_fd;
        }

        pid_t pid = fork();
        if (pid == 0) {
            /* child */
            if (i < nseg - 1) close(pipefd[0]); /* child doesn't read this pipe */
            exec_segment(argv, prev_read, this_out, err_fd);
        }
        /* parent */
        if (prev_read != in_fd)   close(prev_read);  /* done with previous read-end */
        if (i < nseg - 1) {
            close(pipefd[1]);        /* parent doesn't write */
            prev_read = pipefd[0];   /* carry read-end to next iteration */
        }
    }

    /* wait for all children */
    int status;
    for (i = 0; i < nseg; i++) {
        pid_t p = wait(&status);
        (void)p;
    }
    last_status = WEXITSTATUS(status);
}

/* ════════════════════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════════════════════ */
int main() {
    char command[1024];
    char *token;
    char *outfile, *infile, *errfile;
    int  i, fd, amper, redirect, redirect_append, redirect_err, redirect_in;
    int  retid, status;
    char *argv[64];

    /* register signal handlers */
    signal(SIGINT,  sigint_handler);   /* feature 10 */
    signal(SIGCHLD, sigchld_handler);  /* feature 12 */

    while (1) {
        printf("%s: ", prompt);
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL) {
            /* EOF (Ctrl-D) */
            printf("\n");
            break;
        }
        command[strlen(command) - 1] = '\0';   /* strip newline */

        /* ── feature 5: up-arrow sends ESC [ A  (\x1B[A) ── */
        if (strcmp(command, "\x1B[A") == 0) {
            strcpy(command, last_cmd);
            printf("(repeat) %s\n", command);
        }

        /* ── feature 4: !! ── */
        if (strcmp(command, "!!") == 0) {
            if (last_cmd[0] == '\0') {
                printf("No previous command.\n");
                continue;
            }
            strcpy(command, last_cmd);
            printf("%s\n", command);   /* echo repeated command */
        }

        /* save for !! / up-arrow (skip saving !! itself) */
        if (strcmp(command, "!!") != 0)
            strncpy(last_cmd, command, sizeof(last_cmd)-1);

        /* ────────────────────────────────────────────────────
           feature 11: check for pipe '|' BEFORE normal parse
           ──────────────────────────────────────────────────── */
        if (strchr(command, '|') != NULL) {
            /* split on '|' */
            char pipe_buf[1024];
            strncpy(pipe_buf, command, sizeof(pipe_buf)-1);
            pipe_buf[sizeof(pipe_buf)-1] = '\0';

            char *segments[32];
            int   nseg = 0;
            char *seg  = strtok(pipe_buf, "|");
            while (seg != NULL) {
                /* trim leading space */
                while (*seg == ' ') seg++;
                segments[nseg++] = seg;
                seg = strtok(NULL, "|");
            }

            /* check for output redirection on last segment */
            int  final_out = STDOUT_FILENO;
            int  final_err = STDERR_FILENO;
            char *last_seg = segments[nseg - 1];
            char *gt2 = strstr(last_seg, " >> ");
            char *gt  = strstr(last_seg, " > ");

            if (gt2) {
                *gt2 = '\0';
                char fname[256];
                sscanf(gt2 + 4, "%255s", fname);
                final_out = open(fname, O_WRONLY|O_CREAT|O_APPEND, 0660);
            } else if (gt) {
                *gt = '\0';
                char fname[256];
                sscanf(gt + 3, "%255s", fname);
                final_out = creat(fname, 0660);
            }

            run_pipeline(segments, nseg, STDIN_FILENO, final_out, final_err);

            if (final_out != STDOUT_FILENO) close(final_out);
            continue;
        }

        /* ─── normal (non-pipe) command parse ─────────────── */
        char parse_buf[1024];
        strncpy(parse_buf, command, sizeof(parse_buf)-1);

        i = 0;
        token = strtok(parse_buf, " ");
        while (token != NULL) {
            argv[i++] = token;
            token = strtok(NULL, " ");
        }
        argv[i] = NULL;

        if (argv[0] == NULL) continue;

        /* ── background ── */
        if (i > 0 && strcmp(argv[i-1], "&") == 0) {
            amper = 1;
            argv[--i] = NULL;
        } else {
            amper = 0;
        }

        /* ── feature 7: stderr redirect  2> ── */
        redirect_err = 0;  errfile = NULL;
        if (i >= 3 && strcmp(argv[i-2], "2>") == 0) {
            redirect_err = 1;
            errfile = argv[i-1];
            argv[i-2] = NULL;
            i -= 2;
        }

        /* ── feature 8: stdout append  >> ── */
        redirect_append = 0;
        if (i >= 3 && strcmp(argv[i-2], ">>") == 0) {
            redirect_append = 1;
            outfile = argv[i-1];
            argv[i-2] = NULL;
            i -= 2;
        }

        /* ── feature 7 (stdout): stdout redirect  > ── */
        redirect = 0;  outfile = NULL;
        if (!redirect_append && i >= 3 && strcmp(argv[i-2], ">") == 0) {
            redirect = 1;
            outfile = argv[i-1];
            argv[i-2] = NULL;
            i -= 2;
        }

        /* ── feature 9: stdin redirect  < ── */
        redirect_in = 0;  infile = NULL;
        if (i >= 3 && strcmp(argv[i-2], "<") == 0) {
            redirect_in = 1;
            infile = argv[i-1];
            argv[i-2] = NULL;
            i -= 2;
        }

        /* ════════════════════════════════════════════════════
           Built-in commands (executed BEFORE fork)
           ════════════════════════════════════════════════════ */

        /* feature 6: quit */
        if (strcmp(argv[0], "quit") == 0) {
            exit(0);
        }

        /* feature 1: prompt = newprompt */
        if (strcmp(argv[0], "prompt") == 0 &&
            argv[1] != NULL && strcmp(argv[1], "=") == 0 &&
            argv[2] != NULL) {
            strncpy(prompt, argv[2], sizeof(prompt)-1);
            continue;
        }

        /* feature 2: status */
        if (strcmp(argv[0], "status") == 0) {
            printf("%d\n", last_status);
            continue;
        }

        /* feature 3: cd */
        if (strcmp(argv[0], "cd") == 0) {
            char *dir = argv[1] ? argv[1] : getenv("HOME");
            if (chdir(dir) != 0)
                perror("cd");
            continue;
        }

        /* ════════════════════════════════════════════════════
           Fork and exec
           ════════════════════════════════════════════════════ */
        pid_t pid = fork();
        if (pid == 0) {
            /* ── stdout redirect ── */
            if (redirect) {
                fd = creat(outfile, 0660);
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            /* ── stdout append ── */
            if (redirect_append) {
                fd = open(outfile, O_WRONLY|O_CREAT|O_APPEND, 0660);
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            /* ── stderr redirect ── */
            if (redirect_err) {
                fd = creat(errfile, 0660);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            /* ── stdin redirect ── */
            if (redirect_in) {
                fd = open(infile, O_RDONLY);
                if (fd < 0) { perror(infile); exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* restore default SIGINT in child so it can be killed */
            signal(SIGINT, SIG_DFL);

            execvp(argv[0], argv);
            perror(argv[0]);
            exit(1);
        }

        /* parent */
        if (amper == 0) {
            retid = waitpid(pid, &status, 0);
            last_status = WEXITSTATUS(status);
        } else {
            last_bg_pid = pid;
            /* background: SIGCHLD handler will reap */
        }
    }
    return 0;
}

