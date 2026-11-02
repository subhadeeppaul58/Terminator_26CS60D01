
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include <sys/select.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <dirent.h>
#include <sys/stat.h>
#include <glob.h>

#define MAX_TABS 10
#define HISTORY_MAX 10000
#define MAX_COMMANDS 100

typedef struct {
    char command[1024];
    char output[1024];
    int command_length;
    int cursor_pos;             /* insertion cursor for Task 7 */
    int command_y;
} terminal_comp;

typedef struct {
    terminal_comp tcomp[MAX_COMMANDS];
    int n;
    char cwd[PATH_MAX];
} Tab;

typedef struct {
    Display *display;
    Window window;
    GC gc;

    Tab tabs[MAX_TABS];
    int active_tab;
    int tab_count;
    char (*history)[1024];
    int history_count;
    int history_search_mode;
    int completion_count;
    int completion_start;
    char completion_dir[512];
    char completion_matches[128][512];
    int history_index;          /* -1 means not browsing history */
    char clipboard[1024];       /* Ctrl+Shift+C / Ctrl+Shift+V */
    int scroll_offset;           /* vertical viewport offset in pixels; -1 = follow bottom */
    int dragging_scrollbar;
} Terminal;



#define MAX_JOBS 64

typedef struct {
    int id;
    pid_t pgid;
    char command[256];
    int state;  /* 1 = Running, 2 = Stopped */
} Job;

static Job jobs[MAX_JOBS];
static int next_job_id = 1;
static pid_t foreground_pgid = -1;
static int last_exit_status = 0; /* value expanded by $? */


static int add_job(pid_t pgid, const char *cmd, int state)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid <= 0) {
            jobs[i].id = next_job_id++;
            jobs[i].pgid = pgid;
            jobs[i].state = state;

            snprintf(jobs[i].command,
                     sizeof(jobs[i].command), "%s", cmd);

            return i;
        }
    }

    return -1;
}

static int find_job_id(int id)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid > 0 && jobs[i].id == id)
            return i;
    }

    return -1;
}

static void reap_jobs(void)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid <= 0)
            continue;

        int status;
        pid_t result = waitpid(-jobs[i].pgid, &status,
                               WNOHANG | WUNTRACED | WCONTINUED);

        if (result > 0) {
            if (WIFSTOPPED(status))
                jobs[i].state = 2;
            else if (WIFCONTINUED(status))
                jobs[i].state = 1;
            else if (WIFEXITED(status) || WIFSIGNALED(status))
                jobs[i].pgid = 0;
        }
    }
}


static void handle_job_signal(Terminal *t, XKeyEvent *event)
{
    KeySym key;
    char buf[8];
    XLookupString(event, buf, sizeof(buf), &key, NULL);

    if (!(event->state & ControlMask))
        return;

    if (key == XK_c && foreground_pgid > 0) {
        kill(-foreground_pgid, SIGINT);
    } else if (key == XK_z && foreground_pgid > 0) {
        kill(-foreground_pgid, SIGTSTP);
    }
}



/* Function declarations */
void draw_terminal(Terminal *t);
void draw_tabs(Terminal *t);
void init_tab(Tab *tab);
Tab * current_tab(Terminal *t);
void handle_mouse(Terminal *t, XButtonEvent *event);
static void handle_mouse_motion(Terminal *t, XMotionEvent *event);

/* Wait for a foreground process group while continuing to process X11 signals. */
static int wait_foreground_group(Terminal *t, pid_t pgid, int *stopped)
{
    int status = 0;
    *stopped = 0;
    foreground_pgid = pgid;
    while (1) {
        pid_t r = waitpid(-pgid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (r > 0 && WIFSTOPPED(status)) { *stopped = 1; break; }
        if (r < 0 && errno == ECHILD) break;
        if (r < 0 && errno != EINTR) break;

        if (XPending(t->display)) {
            XEvent ev;
            XNextEvent(t->display, &ev);
            if (ev.type == Expose) draw_terminal(t);
            else if (ev.type == KeyPress) {
                KeySym key; char b[8];
                XLookupString(&ev.xkey, b, sizeof(b), &key, NULL);
                if ((ev.xkey.state & ControlMask) && key == XK_c)
                    kill(-pgid, SIGINT);
                else if ((ev.xkey.state & ControlMask) && key == XK_z)
                    kill(-pgid, SIGTSTP);
            } else if (ev.type == ButtonPress) handle_mouse(t, &ev.xbutton);
            else if (ev.type == MotionNotify) handle_mouse_motion(t, &ev.xmotion);
            else if (ev.type == ButtonRelease) t->dragging_scrollbar = 0;
        } else {
            struct timeval tv = {0, 20000};
            select(0, NULL, NULL, NULL, &tv);
        }
    }
    foreground_pgid = -1;
    return status;
}




/* Small command parser: arguments, |, <, > and >>. */
#define MAX_PIPE_CMDS 10
#define MAX_ARGS 32

typedef struct { 
    char *argv[MAX_ARGS]; 
    char *input, *output; 
    int append; 
     } SimpleCmd;



int parse_command(char *s, SimpleCmd c[], int *count)
{
    /* strtok() is used after protecting spaces inside quoted arguments. */
    char work[4096];
    int i = 0, j = 0;
    char quote = '\0';

  while (s[i] != '\0') {
        char ch = s[i++];

        if (j >= (int)sizeof(work) - 4)
            return -1;

        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
                work[j++] = ch;
            } else if (ch == ' ' || ch == '\t' || ch == '\n') {
                work[j++] = '\x1F';
            } else {
                work[j++] = ch;
            }
        } 
        else if(ch=='\\'&&s[i]=='\n')
        {i++; continue;}
        else if (ch == '\'' || ch == '"') {
            quote = ch;
            work[j++] = ch;
        } else if (ch == '|' || ch == '<' || ch == '>') {
            work[j++] = ' ';
            work[j++] = ch;
            if (ch == '>' && s[i] == '>')
                work[j++] = s[i++];
            work[j++] = ' ';
        } 
              
        
        else {
            work[j++] = ch;
        }
    }

    if (quote != '\0')
        return -1;
    work[j] = '\0';

    memset(c, 0, sizeof(SimpleCmd) * MAX_PIPE_CMDS);
    *count = 1;

    char *token = strtok(work, " \t\n");
    while (token != NULL) {
        SimpleCmd *a = &c[*count - 1];

        if (strcmp(token, "|") == 0) {
            if (a->argv[0] == NULL || *count >= MAX_PIPE_CMDS)
                return -1;
            (*count)++;
        } else if (strcmp(token, "<") == 0 ||
                   strcmp(token, ">") == 0 ||
                   strcmp(token, ">>") == 0) {
            char *op = token;
            token = strtok(NULL, " \t\n");
            if (token == NULL || strcmp(token, "|") == 0 ||
                strcmp(token, "<") == 0 || strcmp(token, ">") == 0 ||
                strcmp(token, ">>") == 0)
                return -1;

            if (strcmp(op, "<") == 0) {
                if (a->input != NULL)
                    return -1;
                a->input = token;
            } else {
                if (a->output != NULL)
                    return -1;
                a->output = token;
                a->append = (strcmp(op, ">>") == 0);
            }
        } else {
            int n = 0;
            while (n < MAX_ARGS && a->argv[n] != NULL)
                n++;
            if (n >= MAX_ARGS - 1)
                return -1;

            for (int k = 0; token[k] != '\0'; k++) {
                if (token[k] == '\x1F')
                    token[k] = ' ';
            }

            /* Remove matching quotes around a complete argument. */
            size_t len = strlen(token);
            if (len >= 2 && (token[0] == '\'' || token[0] == '"') &&
                token[len - 1] == token[0]) {
                token[len - 1] = '\0';
                token++;
            }
            a->argv[n] = token;
        }

        token = strtok(NULL, " \t\n");
    }

    for (int k = 0; k < *count; k++) {
        if (c[k].argv[0] == NULL)
            return -1;
    }
    return 0;
}


void expand_variables(char *text, size_t size)
{
    char result[1024];
    size_t i = 0, j = 0;
    int single_quote = 0;

    while (text[i] != '\0' && j < sizeof(result) - 1) {
        if (text[i] == '\'' ) {
            single_quote = !single_quote;
            result[j++] = text[i++];
            continue;
        }

        if (text[i] == '$' && !single_quote) {
            /* Shell special parameter: $? is the previous command's status. */
            if (text[i + 1] == '?') {
                char status_text[16];
                int sn = snprintf(status_text, sizeof(status_text), "%d", last_exit_status);
                if (sn < 0 || j + (size_t)sn >= sizeof(result)) break;
                memcpy(result + j, status_text, (size_t)sn);
                j += (size_t)sn;
                i += 2;
                continue;
            }
            char name[256];
            size_t n = 0;
            size_t k = i + 1;

            while ((isalnum((unsigned char)text[k]) ||
                    text[k] == '_') &&
                   n < sizeof(name) - 1) {
                name[n++] = text[k++];
            }

            if (n > 0) {
                name[n] = '\0';
                const char *value = getenv(name);

                if (value != NULL) {
                    size_t v = strlen(value);

                    if (j + v >= sizeof(result))
                        break;

                    memcpy(result + j, value, v);
                    j += v;
                }

                i = k;
                continue;
            }
        }

        result[j++] = text[i++];
    }

    result[j] = '\0';
    snprintf(text, size, "%s", result);
}


int execute_command_text(const char *command, const char *cwd,
                         char *output, size_t output_size)
{
    char text[1024];

    if (strlen(command) >= sizeof(text) || output_size == 0)
        return -1;

    snprintf(text, sizeof(text), "%s", command);
    output[0] = '\0';

    SimpleCmd c[MAX_PIPE_CMDS];
    int count = 0;

    /* Use your existing parser */
    if (parse_command(text, c, &count) < 0)
        return -1;

    int pipes[MAX_PIPE_CMDS - 1][2];
    int result[2];
    pid_t pids[MAX_PIPE_CMDS];
    int made = 0;

    if (pipe(result) < 0)
        return -1;

    for (int i = 0; i < count - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(result[0]);
            close(result[1]);
            return -1;
        }
    }

    for (int i = 0; i < count; i++) {
        pid_t pid = fork();

        if (pid < 0)
            break;

        if (pid == 0) {
            if (cwd != NULL && cwd[0] != '\0')
                chdir(cwd);

            /* Read from the previous command in a pipeline */
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);

            /* Write to the next command, or to the result pipe */
            if (i < count - 1)
                dup2(pipes[i][1], STDOUT_FILENO);
            else
                dup2(result[1], STDOUT_FILENO);

            /* Capture error messages too */
            dup2(result[1], STDERR_FILENO);

            /* Input redirection */
            if (c[i].input != NULL) {
                int fd = open(c[i].input, O_RDONLY);

                if (fd < 0) {
                    dprintf(STDERR_FILENO, "%s: %s\n",
                            c[i].input, strerror(errno));
                    _exit(1);
                }

                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* Output redirection */
            if (c[i].output != NULL) {
                int flags = O_WRONLY | O_CREAT |
                    (c[i].append ? O_APPEND : O_TRUNC);

                int fd = open(c[i].output, flags, 0666);

                if (fd < 0) {
                    dprintf(STDERR_FILENO, "%s: %s\n",
                            c[i].output, strerror(errno));
                    _exit(1);
                }

                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            close(result[0]);
            close(result[1]);

            for (int j = 0; j < count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            execvp(c[i].argv[0], c[i].argv);

            dprintf(STDERR_FILENO, "%s: command not found\n",
                    c[i].argv[0]);
            _exit(127);
        }

        pids[made++] = pid;
    }

    /* Parent closes its copies of the pipe ends */
    close(result[1]);

    for (int i = 0; i < count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Read all output, even if the destination buffer fills */
    size_t used = 0;
    char buf[256];
    ssize_t r;

    while ((r = read(result[0], buf, sizeof(buf))) > 0) {
        size_t available = output_size - 1 - used;
        size_t copy = (size_t)r;

        if (copy > available)
            copy = available;

        if (copy > 0) {
            memcpy(output + used, buf, copy);
            used += copy;
        }
    }

    close(result[0]);
    output[used] = '\0';

    int status = 0;

    for (int i = 0; i < made; i++) {
        int child_status;

        if (waitpid(pids[i], &child_status, 0) < 0)
            status = -1;
    }

    if (made != count)
        return -1;

    return status;
}



#define MAX_WATCH 10

static int mw_active = 0;
static int mw_count = 0;
static char mw_cmd[MAX_WATCH][256];
static pid_t mw_pid[MAX_WATCH];
static int mw_fd[MAX_WATCH];
static time_t mw_next[MAX_WATCH];
static int mw_started[MAX_WATCH];
static pid_t mw_pids[MAX_WATCH][MAX_PIPE_CMDS];
static int mw_pid_count[MAX_WATCH];
static char mw_cwd[MAX_WATCH][PATH_MAX];

static int mw_tab_index = 0;
static int mw_output_index = 0;

/* Start one command and capture its output through a pipe. */

/* Start one watcher process; execute the command every 2 seconds. */
static int mw_launch(int i)
{
    int p[2];

    if (pipe(p) < 0)
        return -1;

    pid_t pid = fork();

    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);

        close(p[0]);
        close(p[1]);

        if (mw_cwd[i][0] != '\0' && chdir(mw_cwd[i]) < 0)
            _exit(1);

        for (;;) {
            char output[1024] = {0};
            char header[384];

            /* Reuse your existing command executor.
               It supports pipelines through parse_command(). */
            if (execute_command_text(
                    mw_cmd[i],
                    mw_cwd[i],
                    output,
                    sizeof(output)) < 0) {
                snprintf(output, sizeof(output),
                         "Command execution error\n");
            }

            int h = snprintf(
                header, sizeof(header),
                "\n\"%s\", %ld:\n"
                "----------------------------------------------------\n",
                mw_cmd[i], (long)time(NULL));

            if (h > 0)
                (void)write(STDOUT_FILENO, header, (size_t)h);

            if (output[0])
                (void)write(STDOUT_FILENO, output, strlen(output));

            (void)write(
                STDOUT_FILENO,
                "\n----------------------------------------------------\n",
                strlen("\n----------------------------------------------------\n"));

            sleep(2);
        }
    }

    close(p[1]);

    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(p[0], F_SETFL, flags | O_NONBLOCK);

    mw_pid[i] = pid;
    mw_fd[i] = p[0];
    mw_started[i] = 1;

    return 0;
}


/* Start multiWatch ["command 1", "command 2", ...]. */
void start_multiwatch(Terminal *t, const char *args)
{
    mw_tab_index = t->active_tab;
    mw_output_index = t->tabs[mw_tab_index].n;

    if (mw_active) {
        return;
    }

    mw_count = 0;

    /* Skip the multiWatch command name. */
    const char *p = args;
    while (*p && !isspace((unsigned char)*p))
        p++;

    while (*p && mw_count < MAX_WATCH) {
        while (*p && (isspace((unsigned char)*p) ||
                      *p == '[' || *p == ',' || *p == ']'))
            p++;

        if (!*p)
            break;

        if (*p != '"') {
            mw_count = 0;
            return;
        }

        p++;
        int j = 0;

        while (*p && *p != '"' && j < 255)
            mw_cmd[mw_count][j++] = *p++;

        mw_cmd[mw_count][j] = '\0';

        if (*p != '"') {
            mw_count = 0;
            return;
        }

        p++;
        mw_count++;
    }

    if (mw_count == 0)
        return;

    Tab *tab = current_tab(t);

for (int i = 0; i < mw_count; i++) {
    mw_pid[i] = -1;
    mw_fd[i] = -1;
    mw_started[i] = 0;
    mw_next[i] = 0;

    /* Remember the directory for this watched command. */
    snprintf(mw_cwd[i], sizeof(mw_cwd[i]), "%s", tab->cwd);

    if (mw_launch(i) < 0) {
                for (int j = 0; j < i; j++) {
                if (mw_pid[j] > 0)
                    kill(mw_pid[j], SIGTERM);
                if (mw_fd[j] >= 0)
                    close(mw_fd[j]);
            }

            mw_count = 0;
            return;
        }
    }

    mw_active = 1;
}

/* Read available output and restart finished commands after 2 seconds. */
void multiwatch_poll(Terminal *t)
{
    if (!mw_active)
        return;

    if (mw_tab_index >= t->tab_count ||    mw_output_index >= MAX_COMMANDS)
    return;

Tab *tab = &t->tabs[mw_tab_index];
terminal_comp *line = &tab->tcomp[mw_output_index];

    for (int i = 0; i < mw_count; i++) {
        if (mw_fd[i] >= 0) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(mw_fd[i], &readfds);

            struct timeval tv = {0, 0};

            if (select(mw_fd[i] + 1, &readfds,
                       NULL, NULL, &tv) > 0) {
                char buf[256];
                ssize_t n = read(mw_fd[i], buf, sizeof(buf));
if (n > 0) {
    size_t old = strlen(line->output);
    size_t room = sizeof(line->output) - old - 1;
    size_t copy = (size_t)n;

    /* Keep the latest output if the buffer becomes full. */
    if (copy > room) {
        line->output[0] = '\0';
        old = 0;
        room = sizeof(line->output) - 1;

        if (copy > room)
            copy = room;
    }

    memcpy(line->output + old, buf, copy);
    line->output[old + copy] = '\0';

    draw_terminal(t);
}
            }
        }

        if (mw_pid[i] > 0) {
            int status;
            pid_t result = waitpid(mw_pid[i], &status, WNOHANG);

            if (result == mw_pid[i]) {
                mw_pid[i] = -1;

                if (mw_fd[i] >= 0) {
                    close(mw_fd[i]);
                    mw_fd[i] = -1;
                }

                mw_next[i] = time(NULL) + 2;
            }
        }

        if (mw_pid[i] < 0 && time(NULL) >= mw_next[i]) {
            if (mw_launch(i) < 0)
                mw_next[i] = time(NULL) + 2;
        }
    }
}

/* Stop all monitored commands. */
void stop_multiwatch(void)
{
    if (!mw_active)
        return;

    for (int i = 0; i < mw_count; i++) {
        if (mw_pid[i] > 0) {
            kill(mw_pid[i], SIGTERM);
            waitpid(mw_pid[i], NULL, 0);
            mw_pid[i] = -1;
        }

        if (mw_fd[i] >= 0) {
            close(mw_fd[i]);
            mw_fd[i] = -1;
        }
    }

    mw_active = 0;
    mw_count = 0;
}






int expand_command_substitutions(char *text, size_t size,
                                 const char *cwd)
{
    char result[1024];
    size_t i = 0, j = 0;

    while (text[i] != '\0') {
        if (text[i] == '$' && text[i + 1] == '(') {
            size_t start = i + 2;
            size_t k = start;
            int depth = 1;
            char quote = '\0';
            int escaped = 0;

            while (text[k] != '\0' && depth > 0) {
                char ch = text[k];

                if (escaped) {
                    escaped = 0;
                    k++;
                    continue;
                }

                if (ch == '\\' && quote != '\'') {
                    escaped = 1;
                    k++;
                    continue;
                }

                if (quote != '\0') {
                    if (ch == quote)
                        quote = '\0';
                } else if (ch == '\'' || ch == '"') {
                    quote = ch;
                } else if (ch == '(') {
                    depth++;
                } else if (ch == ')') {
                    depth--;
                }

                k++;
            }

            if (depth != 0)
                return -1;

            size_t inner_len = k - start - 1;

            if (inner_len >= 1024)
                return -1;

            char inner[1024];
            memcpy(inner, text + start, inner_len);
            inner[inner_len] = '\0';

            /* Expand any substitutions nested inside this one */
            if (expand_command_substitutions(
                    inner, sizeof(inner), cwd) < 0)
                return -1;

            /* Expand environment variables inside the inner command */
            expand_variables(inner, sizeof(inner));

            char output[1024];

            /* Execute using your own parser and executor */
            if (execute_command_text(
                    inner, cwd, output, sizeof(output)) < 0)
                return -1;

            size_t out_len = strlen(output);

            /* Shell command substitution removes trailing newlines */
            while (out_len > 0 && output[out_len - 1] == '\n')
                output[--out_len] = '\0';

            if (j + out_len >= sizeof(result))
                return -1;

            memcpy(result + j, output, out_len);
            j += out_len;
            i = k;
        } else {
            if (j >= sizeof(result) - 1)
                return -1;

            result[j++] = text[i++];
        }
    }

    result[j] = '\0';

    if (j >= size)
        return -1;

    memcpy(text, result, j + 1);
    return 0;
}
static void history_path(char *path, size_t size)
{
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(path, size, "%s/.terminator_history", home);
}

static void load_history(Terminal *t)
{
    char path[PATH_MAX], buf[1024];
    history_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (!*buf) continue;
        if (t->history_count == HISTORY_MAX) {
            memmove(t->history, t->history + 1, (HISTORY_MAX - 1) * sizeof(t->history[0]));
            t->history_count--;
        }
        snprintf(t->history[t->history_count++], 1024, "%s", buf);
    }
    fclose(f);
}

static void save_history(Terminal *t)
{
    char path[PATH_MAX];
    history_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    int first = t->history_count > HISTORY_MAX ? t->history_count - HISTORY_MAX : 0;
    for (int i = first; i < t->history_count; i++) fprintf(f, "%s\n", t->history[i]);
    fclose(f);
}

static void add_history(Terminal *t, const char *text)
{
    if (!t->history) return;
    if (t->history_count >= HISTORY_MAX) {
        memmove(t->history, t->history + 1, (HISTORY_MAX - 1) * sizeof(t->history[0]));
        t->history_count = HISTORY_MAX - 1;
    }
    snprintf(t->history[t->history_count++], 1024, "%s", text);
    save_history(t);
}

static void search_history(Terminal *t, terminal_comp *line, const char *term)
{
    int exact = -1, best = 0, best_i = -1;
    size_t n = strlen(term);
    for (int i = t->history_count - 1; i >= 0; i--) {
        if (strcmp(t->history[i], term) == 0) { exact = i; break; }
        /* Longest common contiguous substring, sufficient for the requested fallback. */
        int longest = 0;
        for (size_t a = 0; a < n; a++) for (size_t b = 0; t->history[i][b]; b++) {
            int k = 0;
            while (a + (size_t)k < n && t->history[i][b + k] && term[a+k] == t->history[i][b+k]) k++;
            if (k > longest) longest = k;
        }
        if (longest > best) { best = longest; best_i = i; }
    }
    line->output[0] = '\0';
    if (exact >= 0) snprintf(line->output, sizeof(line->output), "%s\n", t->history[exact]);
    else if (best > 2 && best_i >= 0) {
        int used = 0;
        for (int i = t->history_count - 1; i >= 0 && used < (int)sizeof(line->output)-2; i--) {
            int found = 0;
            for (size_t a = 0; a < n && !found; a++) for (size_t b = 0; t->history[i][b]; b++) {
                int k = 0; while (a+(size_t)k<n && t->history[i][b+k] && term[a+k]==t->history[i][b+k]) k++;
                if (k == best) { found = 1; break; }
            }
            if (found) used += snprintf(line->output+used, sizeof(line->output)-used, "%s\n", t->history[i]);
        }
    } else snprintf(line->output, sizeof(line->output), "No match for search term in history\n");
}

static void complete_filename(Terminal *t, terminal_comp *line)
{
    Tab *tab = current_tab(t);
    int pos = line->cursor_pos;
    int start = pos;
    while (start > 0 && !isspace((unsigned char)line->command[start-1])) start--;
    char word[512]; int wn = pos-start; if (wn < 0 || wn >= (int)sizeof(word)) return;
    memcpy(word, line->command+start, (size_t)wn); word[wn]='\0';
    char dirpath[PATH_MAX] = ".", prefix[512];
    char *slash = strrchr(word, '/');
    if (slash) {
        size_t dlen = (size_t)(slash-word+1);
        if (dlen >= sizeof(dirpath)) return;
        memcpy(dirpath, word, dlen); dirpath[dlen]='\0';
        snprintf(prefix, sizeof(prefix), "%s", slash+1);
    } else snprintf(prefix, sizeof(prefix), "%s", word);
    DIR *d = opendir(dirpath); if (!d) return;
    char matches[128][512]; int count=0; struct dirent *e;
    while ((e=readdir(d)) && count<128) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) {
            snprintf(matches[count++], sizeof(matches[0]), "%s", e->d_name);
        }
    }
    closedir(d); if (!count) return;
    t->completion_count = 0;
    char chosen[512];
    if (count == 1) snprintf(chosen, sizeof(chosen), "%s", matches[0]);
    else {
        size_t common = strlen(matches[0]);
        for (int i=1;i<count;i++) { size_t k=0; while(k<common && matches[i][k] && matches[0][k]==matches[i][k]) k++; common=k; }
        if (common > strlen(prefix)) { memcpy(chosen,matches[0],common); chosen[common]='\0'; }
        else {
            t->completion_count = count; t->completion_start = start;
            snprintf(t->completion_dir, sizeof(t->completion_dir), "%s", slash ? "" : "");
            for (int i=0;i<count;i++) snprintf(t->completion_matches[i], sizeof(t->completion_matches[i]), "%s", matches[i]);
            size_t used=0; for(int i=0;i<count && used<sizeof(line->output)-32;i++)
                used += snprintf(line->output+used,sizeof(line->output)-used,"%d. %s%s",i+1,matches[i],i+1==count?"\n":"  ");
            return;
        }
    }
    char replacement[1024];
    int n=snprintf(replacement,sizeof(replacement),"%.*s%s",start, line->command, slash ? "" : "");
    (void)n;
    char full[1024];
    if (slash) snprintf(full,sizeof(full),"%.*s%s",(int)(slash-word+1),word,chosen);
    else snprintf(full,sizeof(full),"%s",chosen);
    int newlen=start+(int)strlen(full)+(int)strlen(line->command+pos);
    if (newlen >= (int)sizeof(line->command)) return;
    memmove(line->command+start+strlen(full),line->command+pos,strlen(line->command+pos)+1);
    memcpy(line->command+start,full,strlen(full));
    line->command_length=newlen; line->cursor_pos=start+(int)strlen(full);
}

static int script_test(char *expr)
{
    char copy[1024], *v[4] = {0}; int n = 0;
    snprintf(copy, sizeof(copy), "%s", expr);
    char *p = copy;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '[') p++;
    char *end = strchr(p, ']'); if (end) *end = '\0';
    char *save = NULL;
    for (char *w = strtok_r(p, " \t", &save); w && n < 4; w = strtok_r(NULL, " \t", &save)) v[n++] = w;
    if (n < 2) return 0;
    for (int i=0;i<n;i++) { size_t z=strlen(v[i]); if(z>=2 && (v[i][0]=='"' || v[i][0]==39) && v[i][z-1]==v[i][0]) { memmove(v[i],v[i]+1,z-2); v[i][z-2]='\0'; } }
    if (!strcmp(v[0], "-f")) { struct stat st; return n>=2 && stat(v[1], &st)==0 && S_ISREG(st.st_mode); }
    if (!strcmp(v[0], "-d")) { struct stat st; return n>=2 && stat(v[1], &st)==0 && S_ISDIR(st.st_mode); }
    if (n < 3) return 0;
    if (!strcmp(v[1], "-eq")) return atoi(v[0]) == atoi(v[2]);
    if (!strcmp(v[1], "-gt")) return atoi(v[0]) > atoi(v[2]);
    if (!strcmp(v[1], "=" ) || !strcmp(v[1], "==")) return strcmp(v[0],v[2])==0;
    if (!strcmp(v[1], "!=")) return strcmp(v[0],v[2])!=0;
    return 0;
}

static void expand_script_args(char *line, size_t cap, char **args, int argc, int last_status)
{
    char out[2048]; size_t j=0;
    for(size_t i=0;line[i] && j<sizeof(out)-1;) {
        if(line[i]=='$' && (isdigit((unsigned char)line[i+1]) || line[i+1]=='#' || line[i+1]=='?')) {
            char val[512]=""; size_t k=i+1;
            if(line[k]=='#') snprintf(val,sizeof(val),"%d",argc-1);
            else if(line[k]=='?') snprintf(val,sizeof(val),"%d",last_status);
            else { int ix=line[k]-'0'; if(ix>=1 && ix<argc) snprintf(val,sizeof(val),"%s",args[ix]); }
            size_t n=strlen(val); if(j+n>=sizeof(out)) break; memcpy(out+j,val,n); j+=n; i=k+1;
        } else out[j++]=line[i++];
    }
    out[j]='\0'; snprintf(line,cap,"%s",out);
}

static int run_script(Terminal *t, const char *path, char **args, int argc)
{
    Tab *tab=current_tab(t); char resolved[PATH_MAX];
    if (path[0]=='/') snprintf(resolved,sizeof(resolved),"%s",path);
    else snprintf(resolved,sizeof(resolved),"%s/%s",tab->cwd,path);
    FILE *f=fopen(resolved,"r");
    if(!f) { terminal_comp *ln=&tab->tcomp[tab->n]; snprintf(ln->output,sizeof(ln->output),"%s: %s\n",path,strerror(errno)); return 1; }
    char lines[512][1024]; int count=0; char buf[1024];
    while(count<512 && fgets(buf,sizeof(buf),f)) { snprintf(lines[count++],sizeof(lines[0]),"%s",buf); }
    fclose(f);
    /* Validate block structure before executing anything. */
    int depth=0;
    for(int i=0;i<count;i++) { char tmp[1024]; snprintf(tmp,sizeof(tmp),"%s",lines[i]); char *q=tmp; while(isspace((unsigned char)*q))q++; if(!strncmp(q,"if ",3))depth++; else if(!strncmp(q,"fi",2)){if(--depth<0){snprintf(tab->tcomp[tab->n].output,1024,"syntax error: unexpected fi\n");return 2;}} }
    if(depth){snprintf(tab->tcomp[tab->n].output,1024,"syntax error: missing fi\n");return 2;}
    int active[32], taken[32], level=0, status=0;
    for(int i=0;i<count;i++) {
        char cmd[1024]; snprintf(cmd,sizeof(cmd),"%s",lines[i]); cmd[strcspn(cmd,"\r\n")]='\0';
        char *q=cmd; while(isspace((unsigned char)*q))q++; if(!*q || *q=='#')continue;
        expand_script_args(q, sizeof(cmd)-(size_t)(q-cmd), args, argc, status);
        while(isspace((unsigned char)*q))q++;
        if(!strncmp(q,"if ",3)) { int ok=script_test(q+3); if(level<31){active[level]=(level==0?1:active[level-1])&&ok;taken[level]=ok;level++;} continue; }
        if(!strncmp(q,"elif ",5) && level>0) { int ix=level-1; int ok=!taken[ix] && script_test(q+5); active[ix]=(ix==0?1:active[ix-1])&&ok; if(ok)taken[ix]=1; continue; }
        if(!strncmp(q,"else",4) && level>0) { int ix=level-1; int ok=!taken[ix]; active[ix]=(ix==0?1:active[ix-1])&&ok; taken[ix]=1; continue; }
        if(!strncmp(q,"fi",2) && level>0) { level--; continue; }
        int enabled=level==0?1:active[level-1]; if(!enabled)continue;
        if(!strncmp(q,"exit",4) && (q[4]=='\0'||isspace((unsigned char)q[4]))) { status=atoi(q+4); break; }
        char *eq=strchr(q,'=');
        if(eq && eq!=q && !strchr(q,' ') && !strchr(q,'|')) { *eq='\0'; setenv(q,eq+1,1); continue; }
        if(!strncmp(q,"cd ",3)) { char old[PATH_MAX]; getcwd(old,sizeof(old)); if(chdir(tab->cwd)==0 && chdir(q+3)==0)getcwd(tab->cwd,sizeof(tab->cwd)); chdir(old); continue; }
        expand_variables(q,sizeof(cmd)-(size_t)(q-cmd));
        char out[1024]="";
        if(execute_command_text(q,tab->cwd,out,sizeof(out))<0) status=1; else status=0;
        terminal_comp *ln=&tab->tcomp[tab->n]; size_t used=strlen(ln->output), n=strlen(out); if(n>sizeof(ln->output)-used-1)n=sizeof(ln->output)-used-1; memcpy(ln->output+used,out,n); ln->output[used+n]='\0';
    }
    return status;
}

/* Execute simple commands, redirection and pipelines. */
void execute_command(Terminal *t)
{
    Tab *tab=current_tab(t); terminal_comp *line=&tab->tcomp[tab->n];
    char text[1024]; snprintf(text,sizeof text,"%s",line->command);
if (expand_command_substitutions(text, sizeof(text),tab->cwd) < 0) {
    snprintf(line->output, sizeof(line->output),
             "command substitution error\n");
    return;
}
    expand_variables(text, sizeof(text));

    while (*text && (text[strlen(text)-1]=='\n' || text[strlen(text)-1]=='\r')) text[strlen(text)-1]='\0';
    line->output[0]='\0'; if (!*text) { last_exit_status = 0; return; }
    add_history(t, text);
    /* A successfully entered built-in starts with success; external commands
       replace this with their actual exit status below. */
    last_exit_status = 0;

    if (strncmp(text, "multiWatch", 10) == 0 &&
    (text[10] == '\0' || isspace((unsigned char)text[10]))) {
    start_multiwatch(t, text);
    snprintf(line->output, sizeof(line->output),
             "multiWatch started. Press Ctrl+C to stop.\n");
    return;
}



    /* Task 11: interpret script files inside Terminator, never through /bin/bash. */
    if (!strchr(text,'|') && !strchr(text,'<') && !strchr(text,'>')) {
        char stmp[1024]; snprintf(stmp,sizeof(stmp),"%s",text);
        char *sav=NULL, *av[MAX_ARGS]={0}; int ac=0;
        for(char *w=strtok_r(stmp," \t",&sav);w && ac<MAX_ARGS-1;w=strtok_r(NULL," \t",&sav)) av[ac++]=w;
        if(ac>0) {
            int si=0; const char *sp=NULL;
            if(!strcmp(av[0],"source") && ac>1) { si=1; sp=av[1]; }
            else if(!strcmp(av[0],"bash") && ac>1) { si=1; sp=av[1]; }
            else if(av[0][0]=='.' && av[0][1]=='/' ) { si=0; sp=av[0]+2; }
            if(sp && (strstr(sp,".sh") || access(sp,F_OK)==0)) {
                int rc=run_script(t,sp,av+si,ac-si);
                if(rc) { size_t used=strlen(line->output); if(used<sizeof(line->output)-1) snprintf(line->output+used,sizeof(line->output)-used,"script exited with status %d\n",rc); }
                return;
            }
        }
    }

    /* Built-ins run in the shell process. */
    if (!strchr(text,'|') && !strchr(text,'<') && !strchr(text,'>')) {
        char temp[1024]; snprintf(temp,sizeof temp,"%s",text); char *av[MAX_ARGS]={0}; int n=0; char *save=NULL;
        for(char *w=strtok_r(temp," \t",&save);w && n<MAX_ARGS-1;w=strtok_r(NULL," \t",&save)) av[n++]=w;

    
if (n && !strcmp(av[0], "jobs")) {
    reap_jobs();

    int used = 0;

    for (int j = 0; j < MAX_JOBS; j++) {
        if (jobs[j].pgid > 0 &&
            used < (int)sizeof(line->output) - 100) {

            used += snprintf(
                line->output + used,
                sizeof(line->output) - used,
                "[%d] %s %s\n",
                jobs[j].id,
                jobs[j].state == 2 ? "Stopped" : "Running",
                jobs[j].command
            );
        }
    }

    return;
}

if (n && (!strcmp(av[0], "fg") ||
          !strcmp(av[0], "bg"))) {

    int id = 1;

    if (n > 1) {
        char *arg = av[1];

        if (*arg == '%')
            arg++;

        id = atoi(arg);
    }

    int j = find_job_id(id);

    if (j < 0) {
        snprintf(line->output, sizeof(line->output),
                 "%s: no such job\n", av[0]);
        return;
    }

    if (!strcmp(av[0], "bg")) {
        if (kill(-jobs[j].pgid, SIGCONT) < 0) {
            snprintf(line->output, sizeof(line->output),
                     "bg: cannot resume job\n");
            return;
        }

        jobs[j].state = 1;

        snprintf(line->output, sizeof(line->output),
                 "[%d]+ %s &\n",
                 jobs[j].id, jobs[j].command);
        return;
    }

    foreground_pgid = jobs[j].pgid;
    if (kill(-jobs[j].pgid, SIGCONT) < 0) {
        foreground_pgid = -1;
        snprintf(line->output, sizeof(line->output), "fg: cannot resume job\n");
        return;
    }
    jobs[j].state = 1;
    int stopped = 0;
    wait_foreground_group(t, jobs[j].pgid, &stopped);
    if (stopped) {
        jobs[j].state = 2;
        snprintf(line->output, sizeof(line->output), "[%d]+ Stopped %s\n", jobs[j].id, jobs[j].command);
    } else {
        jobs[j].pgid = 0;
        line->output[0] = '\0';
    }
    return;
}





        if(n && !strcmp(av[0],"cd")) {
            char old[PATH_MAX]; if(!getcwd(old,sizeof old)) strcpy(old,"/");
            char *dir=n>1?av[1]:getenv("HOME");
            if(chdir(tab->cwd)==0 && chdir(dir)==0) { if(!getcwd(tab->cwd,sizeof tab->cwd)) strcpy(tab->cwd,dir); }
            else snprintf(line->output,sizeof line->output,"cd: %s\n",strerror(errno));
            chdir(old); return;
        }
        if(n && !strcmp(av[0],"history")) { int used=0, first=t->history_count>1000?t->history_count-1000:0; for(int i=first;i<t->history_count && used<(int)sizeof(line->output)-32;i++) used+=snprintf(line->output+used,sizeof(line->output)-used,"%d %s\n",i+1,t->history[i]); return; }
       if(n && !strcmp(av[0],"exit")) { if(t->tab_count>1) { for (int i = t->active_tab; i < t->tab_count - 1; i++) {
            t->tabs[i] = t->tabs[i + 1];
        }t->tab_count--; if(t->active_tab>=t->tab_count)t->active_tab=0; draw_terminal(t); } else { XCloseDisplay(t->display); exit(0); } return; }
            
        }

int background = 0;
size_t textlen = strlen(text);

while (textlen > 0 && isspace((unsigned char)text[textlen - 1])) {  text[--textlen] = '\0';}

if (textlen > 0 && text[textlen - 1] == '&') {
    background = 1;
    text[--textlen] = '\0';
    while (textlen > 0 && isspace((unsigned char)text[textlen - 1])) {        text[--textlen] = '\0';    }
}

    SimpleCmd c[MAX_PIPE_CMDS]; int count=0;
    if(parse_command(text,c,&count)<0) { snprintf(line->output,sizeof line->output,"syntax error\n"); return; }
    int pipes[MAX_PIPE_CMDS-1][2], result[2]; pid_t pids[MAX_PIPE_CMDS]; int made=0;
    if(pipe(result)<0) { snprintf(line->output,sizeof line->output,"pipe error\n"); return; }
    for(int i=0;i<count-1;i++) if(pipe(pipes[i])<0) { snprintf(line->output,sizeof line->output,"pipe error\n"); close(result[0]);close(result[1]);return; }
    pid_t pgid = 0;
    for(int i=0;i<count;i++) {
        pids[i]=fork();
        if(pids[i]==0) {
            if (pgid > 0) setpgid(0, pgid); else setpgid(0, 0);
            if(tab->cwd[0]) chdir(tab->cwd);
            if(i>0) dup2(pipes[i-1][0],STDIN_FILENO);
            if(i<count-1) dup2(pipes[i][1],STDOUT_FILENO);
            else if (background) { int devnull=open("/dev/null",O_WRONLY); if(devnull>=0){dup2(devnull,STDOUT_FILENO);close(devnull);} }
            else dup2(result[1],STDOUT_FILENO);
            if (!background) dup2(result[1],STDERR_FILENO);
            else { int devnull=open("/dev/null",O_WRONLY); if(devnull>=0){dup2(devnull,STDERR_FILENO);close(devnull);} }
            if(c[i].input) { int fd=open(c[i].input,O_RDONLY); if(fd<0){dprintf(2,"%s: %s\n",c[i].input,strerror(errno));_exit(1);} dup2(fd,0);close(fd); }
            if(c[i].output) { int fd=open(c[i].output,O_WRONLY|O_CREAT|(c[i].append?O_APPEND:O_TRUNC),0666); if(fd<0){dprintf(2,"%s: %s\n",c[i].output,strerror(errno));_exit(1);} dup2(fd,1);close(fd); }
            close(result[0]);close(result[1]); for(int j=0;j<count-1;j++){close(pipes[j][0]);close(pipes[j][1]);}
            /* Expand filename patterns such as ls *.txt in the child. */
            {
                char *expanded[MAX_ARGS] = {0};
                char *owned[MAX_ARGS] = {0};
                int ec = 0;
                for (int a = 0; c[i].argv[a] != NULL && ec < MAX_ARGS - 1; a++) {
                    const char *arg = c[i].argv[a];
                    if (strpbrk(arg, "*?[")) {
                        glob_t matches;
                        memset(&matches, 0, sizeof(matches));
                        int gr = glob(arg, GLOB_NOCHECK, NULL, &matches);
                        if (gr == 0) {
                            for (size_t m = 0; m < matches.gl_pathc && ec < MAX_ARGS - 1; m++) {
                                owned[ec] = strdup(matches.gl_pathv[m]);
                                if (owned[ec]) { expanded[ec] = owned[ec]; ec++; }
                            }
                        }
                        globfree(&matches);
                    } else {
                        expanded[ec++] = (char *)arg;
                    }
                }
                expanded[ec] = NULL;
                execvp(expanded[0] ? expanded[0] : c[i].argv[0], expanded[0] ? expanded : c[i].argv);
                for (int a = 0; a < ec; a++) free(owned[a]);
            }
            dprintf(2,"%s: command not found\n",c[i].argv[0]); _exit(127);
        }
        if (pids[i] < 0) break;
        if (pgid == 0) pgid = pids[i];
        setpgid(pids[i], pgid);
        made++;
    }
    close(result[1]); for(int i=0;i<count-1;i++){close(pipes[i][0]);close(pipes[i][1]);}
    if (made != count) {
        if (pgid > 0) kill(-pgid, SIGTERM);
        close(result[0]);
        snprintf(line->output,sizeof line->output,"failed to start command\n");
        return;
    }
    if (background) {
        close(result[0]);
        int ji = add_job(pgid, text, 1);
        if (ji >= 0) snprintf(line->output,sizeof line->output,"[%d] %ld\n",jobs[ji].id,(long)pgid);
        else { kill(-pgid,SIGTERM); snprintf(line->output,sizeof line->output,"job table full\n"); }
        return;
    }

    /* Non-blocking read lets Ctrl+C/Ctrl+Z continue to be handled in the X11 loop. */
    int flags = fcntl(result[0], F_GETFL, 0);
    if (flags >= 0) fcntl(result[0], F_SETFL, flags | O_NONBLOCK);
    int used=0, stopped=0, live=made; char buf[256];
    int command_was_interrupted = 0;
    int pipeline_status = 0;
    int final_status_seen = 0;
    foreground_pgid = pgid;
    while (live > 0 || !stopped) {
        ssize_t nr;
        while ((nr=read(result[0],buf,sizeof buf)) > 0) {
            int k=(int)nr;
            if (k > (int)sizeof(line->output)-1-used) k=(int)sizeof(line->output)-1-used;
            if (k>0) { memcpy(line->output+used,buf,(size_t)k); used+=k; line->output[used]='\0'; }
        }
        live=0;
        for (int i=0;i<made;i++) {
            int st; pid_t wr=waitpid(pids[i],&st,WNOHANG|WUNTRACED|WCONTINUED);
            if (wr==0) live++;
            else if (wr==pids[i] && WIFSTOPPED(st)) { stopped=1; command_was_interrupted=2; }
            else if (wr==pids[i] && WIFCONTINUED(st)) live++;
            else if (wr==pids[i] && i==count-1) {
                pipeline_status = WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 0);
                final_status_seen = 1;
            }
        }
        if (stopped || live==0) break;
        if (XPending(t->display)) {
            XEvent ev; XNextEvent(t->display,&ev);
            if (ev.type==Expose) draw_terminal(t);
            else if (ev.type==KeyPress) {
                KeySym key; char kb[8]; XLookupString(&ev.xkey,kb,sizeof kb,&key,NULL);
                if ((ev.xkey.state&ControlMask) && key==XK_c) { kill(-pgid,SIGINT); command_was_interrupted=1; }
                else if ((ev.xkey.state&ControlMask) && key==XK_z) { kill(-pgid,SIGTSTP); command_was_interrupted=2; }
            } else if (ev.type==ButtonPress) handle_mouse(t,&ev.xbutton);
        } else { struct timeval tv={0,20000}; select(0,NULL,NULL,NULL,&tv); }
    }
    foreground_pgid=-1;
    /* Drain remaining output after the children close the pipe. */
    ssize_t nr;
    while ((nr=read(result[0],buf,sizeof buf))>0 && used<(int)sizeof(line->output)-1) {
        int k=(int)nr; if(k>(int)sizeof(line->output)-1-used) k=(int)sizeof(line->output)-1-used;
        memcpy(line->output+used,buf,(size_t)k); used+=k; line->output[used]='\0';
    }
    close(result[0]);
    if (command_was_interrupted == 1) {
        size_t n = strlen(line->output);
        if (n + 3 < sizeof(line->output)) snprintf(line->output + n, sizeof(line->output) - n, "^C\n");
        last_exit_status = 130;
    } else if (command_was_interrupted == 2 || stopped) {
        size_t n = strlen(line->output);
        if (n + 3 < sizeof(line->output)) snprintf(line->output + n, sizeof(line->output) - n, "^Z\n");
        last_exit_status = 148;
    } else if (final_status_seen) {
        last_exit_status = pipeline_status;
    }
    if (stopped) {
        int ji=add_job(pgid,text,2);
        if (ji>=0) { size_t n=strlen(line->output); snprintf(line->output+n,sizeof(line->output)-n,"\n[%d]+ Stopped %s\n",jobs[ji].id,text); }
    }

}

/* Detect some incomplete shell commands */
int command_incomplete(const char *command)
{
    int single_quote = 0;
    int double_quote = 0;
    int backtick = 0;
    int escaped = 0;
    int substitution_depth = 0;

    for (int i = 0; command[i] != '\0'; i++) {
        char ch = command[i];
        char next = command[i + 1];

        if (escaped) {
            escaped = 0;
            continue;
        }

        
        if (ch == '\\' && !single_quote) {
            if (next != '\0') {
                escaped = 1;
                continue;
            }
            return 1;
        }

        if (!double_quote && !backtick && ch == '\'') {
            single_quote = !single_quote;
            continue;
        }
        if (single_quote)
            continue;

        if (!backtick && ch == '"') {
            double_quote = !double_quote;
            continue;
        }

        if (!double_quote && ch == '`') {
            backtick = !backtick;
            continue;
        }
        if (backtick)
            continue;

     
        if (ch == '$' && next == '(') {
            substitution_depth++;
            i++;
            continue;
        }
        if (substitution_depth > 0) {
            if (ch == '(')
                substitution_depth++;
            else if (ch == ')')
                substitution_depth--;
        }
    }

    return single_quote || double_quote || backtick ||
           substitution_depth > 0 || escaped;
}




/* Return the currently active tab */
Tab *current_tab(Terminal *t)
{
    return &t->tabs[t->active_tab];
}


/* Initialize one tab */
void init_tab(Tab *tab)
{
    memset(tab, 0, sizeof(Tab));
    tab->n = 0;
    tab->tcomp[0].command_y = 50;
    if (!getcwd(tab->cwd, sizeof tab->cwd)) strcpy(tab->cwd, "/");
}


/* Create a new tab */
void new_tab(Terminal *t)
{
    if (t->tab_count >= MAX_TABS)
        return;

    int index = t->tab_count;

    init_tab(&t->tabs[index]);

    t->tab_count++;
    t->active_tab = index;

    draw_terminal(t);
}


/* Switch to an existing tab */
void switch_tab(Terminal *t, int index)
{
    if (index < 0 || index >= t->tab_count)
        return;

    t->active_tab = index;
    draw_terminal(t);
}

/* Draw tab bar */
void draw_tabs(Terminal *t)
{
    char label[40];

    for (int i = 0; i < t->tab_count; i++) {
        int x = 10 + i * 100;

        XDrawRectangle(t->display, t->window, t->gc,
                       x, 5, 90, 25);

        if (i == t->active_tab)
            snprintf(label, sizeof(label), "* Terminal %d", i + 1);
        else
            snprintf(label, sizeof(label), "Terminal %d", i + 1);

        XDrawString(t->display, t->window, t->gc,
                    x + 5, 21, label, strlen(label));
    }

    if (t->tab_count < MAX_TABS) {
        int x = 10 + t->tab_count * 100;

        XDrawRectangle(t->display, t->window, t->gc,
                       x, 5, 25, 25);

        XDrawString(t->display, t->window, t->gc,
                    x + 8, 21, "+", 1);
    }
}




/* Initialize X11 terminal */
void init_terminal(Terminal *t)
{
    memset(t, 0, sizeof(Terminal));

    t->active_tab = 0;
    t->tab_count = 1;
    t->history_index = -1;
    t->scroll_offset = -1; /* start at the newest output */
    t->history = calloc(HISTORY_MAX, sizeof(t->history[0]));
    if (!t->history) { fprintf(stderr, "history allocation failed\n"); exit(1); }
    load_history(t);

    init_tab(&t->tabs[0]);

    t->display = XOpenDisplay(NULL);

    if (t->display == NULL) {
        fprintf(stderr, "Cannot open X display\n");
        exit(1);
    }

    int screen = DefaultScreen(t->display);

    t->window = XCreateSimpleWindow(
        t->display,
        RootWindow(t->display, screen),
        100, 100, 800, 500, 1,
        WhitePixel(t->display, screen),
        BlackPixel(t->display, screen)
    );

    XStoreName(t->display, t->window, "Terminator");

    XSelectInput(t->display, t->window,
                 ExposureMask |
                 KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    t->gc = XCreateGC(t->display, t->window, 0, NULL);

    XSetForeground(t->display, t->gc, 0xFFFFFF);

    XMapWindow(t->display, t->window);
}




/* Current scroll range, recalculated whenever the terminal is redrawn. */
static int terminal_max_scroll = 0;
static int scrollbar_track_top = 36;
static int scrollbar_track_bottom = 490;

/* Count newline characters in a string. */
static int count_newlines(const char *s)
{
    int count = 0;
    if (!s) return 0;
    for (; *s; s++) if (*s == '\n') count++;
    return count;
}

/* Count visual output lines, including wrapping at the right edge. */
static int output_visual_lines(const char *s, int max_chars)
{
    int lines = 1, col = 0;
    if (!s || !*s) return 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') { lines++; col = 0; }
        else if (s[i] != '\r') {
            if (++col >= max_chars) { lines++; col = 0; }
        }
    }
    return lines;
}

/* Draw command output with newline handling, wrapping, and viewport offset. */
void draw_output(Display *display, Window window, GC gc,
                 Tab *tab, int n, int y1, int scroll_offset)
{
    XWindowAttributes attrs;
    int width = 800;
    if (XGetWindowAttributes(display, window, &attrs)) width = attrs.width;
    int max_chars = (width - 40) / 8;
    if (max_chars < 1) max_chars = 1;

    int x = 20;
    int logical_y = y1 + 20;
    int col = 0;
    const char *out = tab->tcomp[n].output;
    for (int i = 0; out[i] != '\0'; i++) {
        char ch = out[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            logical_y += 20; x = 20; col = 0;
            continue;
        }
        if (col >= max_chars) {
            logical_y += 20; x = 20; col = 0;
        }
        if (logical_y - scroll_offset >= 35 && logical_y - scroll_offset < attrs.height) {
            char one[2] = { ch, '\0' };
            XDrawString(display, window, gc, x, logical_y - scroll_offset, one, 1);
        }
        x += 8; col++;
    }
}

/* Draw active tab's contents, keeping the newest output visible by default. */
void draw_terminal(Terminal *t)
{
    Tab *tab = current_tab(t);
    XWindowAttributes attrs;
    int window_height = 500;
    if (XGetWindowAttributes(t->display, t->window, &attrs))
        window_height = attrs.height;

    /* Recompute logical positions, accounting for wrapped output lines. */
    int max_chars = (attrs.width - 40) / 8;
    if (max_chars < 1) max_chars = 1;
    tab->tcomp[0].command_y = 50;
    for (int i = 0; i < tab->n; i++) {
        terminal_comp *current = &tab->tcomp[i];
        int command_lines = count_newlines(current->command);
        int output_lines = output_visual_lines(current->output, max_chars);
        tab->tcomp[i + 1].command_y = current->command_y +
            20 * (command_lines + 1 + output_lines);
    }

    terminal_comp *last = &tab->tcomp[tab->n];
    int content_bottom = last->command_y +
        20 * (count_newlines(last->command) + 1 +
              output_visual_lines(last->output, max_chars));
    int max_scroll = content_bottom - (window_height - 10);
    if (max_scroll < 0) max_scroll = 0;

    terminal_max_scroll = max_scroll;
    scrollbar_track_top = 36;
    scrollbar_track_bottom = window_height - 10;
    if (scrollbar_track_bottom <= scrollbar_track_top)
        scrollbar_track_bottom = scrollbar_track_top + 1;

    if (t->scroll_offset < 0)
        t->scroll_offset = max_scroll;
    if (t->scroll_offset > max_scroll) t->scroll_offset = max_scroll;
    if (t->scroll_offset < 0) t->scroll_offset = 0;

    XClearWindow(t->display, t->window);
    draw_tabs(t);

    /* Keep scrolled transcript from drawing over the tab bar. */
    XRectangle clip;
    clip.x = 0; clip.y = 35;
    clip.width = (unsigned short)attrs.width;
    clip.height = (unsigned short)(attrs.height > 35 ? attrs.height - 35 : 1);
    XSetClipRectangles(t->display, t->gc, 0, 0, &clip, 1, Unsorted);

    const char prompt[] = "spaul@terminator>";
    int len = (int)strlen(prompt);

    for (int i = 0; i <= tab->n; i++) {
        int x = 150;
        int logical_y = tab->tcomp[i].command_y;
        int y = logical_y - t->scroll_offset;

        XDrawString(t->display, t->window, t->gc,
                    20, y, prompt, len);

        for (int j = 0; tab->tcomp[i].command[j] != '\0'; j++) {
            if (tab->tcomp[i].command[j] == '\n') {
                logical_y += 20;
                y = logical_y - t->scroll_offset;
                x = 35;
                XDrawString(t->display, t->window, t->gc, 20, y, ">", 1);
            } else {
                char ch[2] = { tab->tcomp[i].command[j], '\0' };
                XDrawString(t->display, t->window, t->gc, x, y, ch, 1);
                x += 8;
            }
        }

        if (i == tab->n && tab->tcomp[i].cursor_pos >= 0) {
            int cx = 150, cy = tab->tcomp[i].command_y;
            for (int j = 0; j < tab->tcomp[i].cursor_pos &&
                            tab->tcomp[i].command[j] != '\0'; j++) {
                if (tab->tcomp[i].command[j] == '\n') {
                    cy += 20;
                    cx = 35;
                } else cx += 8;
            }
            cy -= t->scroll_offset;
            XDrawLine(t->display, t->window, t->gc, cx, cy - 13, cx, cy + 3);
        }

        draw_output(t->display, t->window, t->gc,
                    tab, i, logical_y, t->scroll_offset);
    }
    XSetClipMask(t->display, t->gc, None);

    /* Draw a visible scrollbar at the right edge of the output area. */
    int track_x = attrs.width - 14;
    int track_top = scrollbar_track_top;
    int track_bottom = scrollbar_track_bottom;
    int track_h = track_bottom - track_top;
    if (track_h < 1) track_h = 1;
    XSetForeground(t->display, t->gc, 0x404040);
    XFillRectangle(t->display, t->window, t->gc,
                   track_x, track_top, 10, (unsigned int)track_h);
    if (terminal_max_scroll > 0) {
        int thumb_h = (int)((double)track_h * (double)(window_height - 35) /
                            (double)(content_bottom > 35 ? content_bottom - 35 : 1));
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > track_h) thumb_h = track_h;
        int travel = track_h - thumb_h;
        int thumb_y = track_top + (terminal_max_scroll > 0
                       ? (int)((double)t->scroll_offset * travel / terminal_max_scroll) : 0);
        XSetForeground(t->display, t->gc, 0xB0B0B0);
        XFillRectangle(t->display, t->window, t->gc,
                       track_x + 1, thumb_y, 8, (unsigned int)thumb_h);
    } else {
        XSetForeground(t->display, t->gc, 0xB0B0B0);
        XFillRectangle(t->display, t->window, t->gc,
                       track_x + 1, track_top, 8, (unsigned int)track_h);
    }
    XSetForeground(t->display, t->gc, 0xFFFFFF);
}

/* Handle mouse clicks on tabs, scrollbar, and mouse wheel. */
void handle_mouse(Terminal *t, XButtonEvent *event)
{
    XWindowAttributes attrs;
    XGetWindowAttributes(t->display, t->window, &attrs);

    /* Clicking the scrollbar track jumps to the corresponding transcript position. */
    if (event->button == Button1 && event->x >= attrs.width - 18 && event->y >= scrollbar_track_top) {
        int track_h = scrollbar_track_bottom - scrollbar_track_top;
        if (track_h < 1) track_h = 1;
        int usable_y = event->y - scrollbar_track_top;
        if (usable_y < 0) usable_y = 0;
        if (usable_y > track_h) usable_y = track_h;
        if (terminal_max_scroll > 0)
            t->scroll_offset = (int)((double)usable_y * terminal_max_scroll / track_h);
        t->dragging_scrollbar = 1;
        draw_terminal(t);
        return;
    }
    /* Mouse wheel: Button 4 scrolls up; Button 5 scrolls down. */
    if (event->button == Button4 || event->button == Button5) {
        if (t->scroll_offset < 0) draw_terminal(t);
        int step = 60; /* three text lines */
        if (event->button == Button4) t->scroll_offset -= step;
        else t->scroll_offset += step;
        if (t->scroll_offset < 0) t->scroll_offset = 0;
        draw_terminal(t);
        return;
    }

    if (event->y < 5 || event->y > 30 ||
        event->x < 10)
        return;

    int relative_x = event->x - 10;
    int index = relative_x / 100;
    int offset = relative_x % 100;

    if (index < t->tab_count && offset < 90) {
        switch_tab(t, index);
    } else if (index == t->tab_count && offset < 25 && t->tab_count < MAX_TABS) {
        new_tab(t);
    }
}


static void handle_mouse_motion(Terminal *t, XMotionEvent *event)
{
    if (!t->dragging_scrollbar) return;
    int track_h = scrollbar_track_bottom - scrollbar_track_top;
    if (track_h < 1) track_h = 1;
    int y = event->y - scrollbar_track_top;
    if (y < 0) y = 0;
    if (y > track_h) y = track_h;
    t->scroll_offset = terminal_max_scroll > 0
        ? (int)((double)y * terminal_max_scroll / track_h) : 0;
    draw_terminal(t);
}


/* Handle keyboard input and Task 7 hotkeys. */
void handle_keyboard(Terminal *t, XKeyEvent *event)
{
    Tab *tab = current_tab(t);
    terminal_comp *line = &tab->tcomp[tab->n];
    char buffer[32] = {0};
    KeySym keysym;
    int length = XLookupString(event, buffer, sizeof(buffer), &keysym, NULL);
    int ctrl = (event->state & ControlMask) != 0;
    int shift = (event->state & ShiftMask) != 0;

    /* PageUp/PageDown scroll the transcript without changing the command line. */
    if (keysym == XK_Page_Up || keysym == XK_Page_Down) {
        if (t->scroll_offset < 0) draw_terminal(t);
        int step = 300;
        if (keysym == XK_Page_Up) t->scroll_offset -= step;
        else t->scroll_offset += step;
        if (t->scroll_offset < 0) t->scroll_offset = 0;
        draw_terminal(t);
        return;
    }

  if (ctrl && !shift && keysym == XK_c && mw_active) {
    stop_multiwatch();

    if (mw_tab_index < t->tab_count &&
        mw_output_index < MAX_COMMANDS) {

        terminal_comp *prev =
            &t->tabs[mw_tab_index].tcomp[mw_output_index];

        size_t len = strlen(prev->output);
        size_t size = sizeof(prev->output);

        if (len + 4 < size) {
            if (len > 0 && prev->output[len - 1] != '\n') {
                prev->output[len++] = '\n';
                prev->output[len] = '\0';
            }

            memcpy(prev->output + len, "^C\n", 4);
        }
    }

    draw_terminal(t);
    return;
}
    int pos = line->cursor_pos;

    if (ctrl && keysym == XK_r) {
        t->history_search_mode = 1;
        line->command[0] = '\0'; line->command_length = line->cursor_pos = 0;
        snprintf(line->output, sizeof(line->output), "Enter search term:\n");
        draw_terminal(t); return;
    }

    if (pos < 0 || pos > line->command_length)
        pos = line->cursor_pos = line->command_length;

    if (t->completion_count > 0 && keysym >= XK_1 && keysym <= XK_9) {
        int pick = (int)(keysym - XK_1);
        if (pick < t->completion_count) {
            int start = t->completion_start;
            int word_start = start;
            while (word_start > 0 && !isspace((unsigned char)line->command[word_start-1])) word_start--;
            int old_end = pos;
            char selected[1024];
            char *lastslash = NULL;
            for (int k=word_start;k<old_end;k++) if(line->command[k]=='/') lastslash=line->command+k;
            int dirlen = lastslash ? (int)(lastslash-line->command+1) : 0;
            snprintf(selected,sizeof(selected),"%.*s%s",dirlen,line->command,t->completion_matches[pick]);
            int suffix = (int)strlen(line->command+old_end);
            int newlen = word_start + (int)strlen(selected) + suffix;
            if (newlen < (int)sizeof(line->command)) {
                memmove(line->command+word_start+strlen(selected),line->command+old_end,(size_t)suffix+1);
                memcpy(line->command+word_start,selected,strlen(selected));
                line->command_length=newlen; line->cursor_pos=word_start+(int)strlen(selected);
            }
        }
        t->completion_count = 0; draw_terminal(t); return;
    }
    if (keysym != XK_Tab) t->completion_count = 0;

    if (ctrl && shift && (keysym == XK_C || keysym == XK_c)) {
        snprintf(t->clipboard, sizeof(t->clipboard), "%s", line->command);
        XStoreBytes(t->display, t->clipboard, (int)strlen(t->clipboard));
    } else if (ctrl && shift && (keysym == XK_V || keysym == XK_v)) {
        int nbytes = 0;
        char *paste = XFetchBytes(t->display, &nbytes);
        if (paste && nbytes > 0 && line->command_length + nbytes < (int)sizeof(line->command)) {
            memmove(line->command + pos + nbytes, line->command + pos,
                    (size_t)(line->command_length - pos + 1));
            memcpy(line->command + pos, paste, (size_t)nbytes);
            line->command_length += nbytes;
            line->cursor_pos += nbytes;
        }
        if (paste) XFree(paste);
    } else if (ctrl && keysym == XK_a) {
        line->cursor_pos = 0;
    } else if (ctrl && keysym == XK_e) {
        line->cursor_pos = line->command_length;
    } else if (ctrl && keysym == XK_u) {
        memmove(line->command, line->command + pos,
                (size_t)(line->command_length - pos + 1));
        line->command_length -= pos;
        line->cursor_pos = 0;
    } else if (ctrl && keysym == XK_k) {
        line->command[pos] = '\0';
        line->command_length = pos;
    } else if (ctrl && keysym == XK_w) {
        int begin = pos;
        while (begin > 0 && isspace((unsigned char)line->command[begin - 1])) begin--;
        while (begin > 0 && !isspace((unsigned char)line->command[begin - 1])) begin--;
        memmove(line->command + begin, line->command + pos,
                (size_t)(line->command_length - pos + 1));
        line->command_length -= pos - begin;
        line->cursor_pos = begin;
    } else if (ctrl && keysym == XK_l) {
        for (int i = 0; i <= tab->n; i++)
            memset(&tab->tcomp[i], 0, sizeof(terminal_comp));
        tab->n = 0;
        tab->tcomp[0].command_y = 50;
        t->scroll_offset = 0;
    } else if (ctrl && keysym == XK_d) {
        if (line->command_length == 0) {
            if (t->tab_count > 1) {
                for (int i = t->active_tab; i < t->tab_count - 1; i++)
                    t->tabs[i] = t->tabs[i + 1];
                t->tab_count--;
                if (t->active_tab >= t->tab_count) t->active_tab = t->tab_count - 1;
            } else {
                XCloseDisplay(t->display);
                exit(0);
            }
        } else if (pos < line->command_length) {
            memmove(line->command + pos, line->command + pos + 1,
                    (size_t)(line->command_length - pos));
            line->command_length--;
        }
    } else if (keysym == XK_Left) {
        if (line->cursor_pos > 0) line->cursor_pos--;
    } else if (keysym == XK_Right) {
        if (line->cursor_pos < line->command_length) line->cursor_pos++;
    } else if (keysym == XK_Up || keysym == XK_Down) {
        if (t->history_count > 0) {
            if (keysym == XK_Up) {
                if (t->history_index < 0) t->history_index = t->history_count - 1;
                else if (t->history_index > 0) t->history_index--;
            } else {
                if (t->history_index >= t->history_count - 1) t->history_index = -1;
                else if (t->history_index >= 0) t->history_index++;
            }
            if (t->history_index >= 0)
                snprintf(line->command, sizeof(line->command), "%s", t->history[t->history_index]);
            else
                line->command[0] = '\0';
            line->command_length = (int)strlen(line->command);
            line->cursor_pos = line->command_length;
        }
    } else if (keysym == XK_Tab) {
        complete_filename(t, line);
    } else if (keysym == XK_Return) {
        t->history_index = -1;
        if (t->history_search_mode) {
            search_history(t, line, line->command);
            t->history_search_mode = 0;
            line->command[0] = '\0'; line->command_length = line->cursor_pos = 0;
            if (tab->n < MAX_COMMANDS-1) { tab->n++; memset(&tab->tcomp[tab->n],0,sizeof(terminal_comp)); tab->tcomp[tab->n].command_y=tab->tcomp[tab->n-1].command_y+20; }
        } else if (line->command_length > 0) {
            if (command_incomplete(line->command)) {
                int len = line->command_length;
                if (len < 1022) {
                    line->command[len] = '\n';
                    line->command[len + 1] = '\0';
                    line->command_length++;
                    line->cursor_pos = line->command_length;
                }
            } else {
                t->scroll_offset = -1; /* follow newest command output */
                execute_command(t);
                if (tab->n < MAX_COMMANDS - 1) {
                    tab->n++;
                    memset(&tab->tcomp[tab->n], 0, sizeof(terminal_comp));
                    tab->tcomp[tab->n].command_y =
                        tab->tcomp[tab->n - 1].command_y + 20;
                }
            }
        }
    } else if (keysym == XK_BackSpace) {
        if (pos > 0) {
            memmove(line->command + pos - 1, line->command + pos,
                    (size_t)(line->command_length - pos + 1));
            line->command_length--;
            line->cursor_pos--;
        }
    } else if (!ctrl && length > 0 && (unsigned char)buffer[0] >= 32) {
        if (line->command_length + length < (int)sizeof(line->command)) {
            memmove(line->command + pos + length, line->command + pos,
                    (size_t)(line->command_length - pos + 1));
            memcpy(line->command + pos, buffer, (size_t)length);
            line->command_length += length;
            line->cursor_pos += length;
        }
    }
    draw_terminal(t);
}

/*int main(void)
{
    Terminal terminal;

    init_terminal(&terminal);

    while (1) {
        XEvent event;

        XNextEvent(terminal.display, &event);

        if (event.type == Expose) {
            draw_terminal(&terminal);
        } else if (event.type == KeyPress) {
            handle_keyboard(&terminal, &event.xkey);
        } else if (event.type == ButtonPress) {
            handle_mouse(&terminal, &event.xbutton);
        }
    }

    return 0;
}*/
int main(void)
{
    Terminal terminal;

    init_terminal(&terminal);

    while (1) {
        XEvent event;

        /* Check for multiWatch output and restart finished commands. */
        multiwatch_poll(&terminal);
        reap_jobs();

        /* Wait briefly for an X11 event instead of blocking indefinitely. */
        if (XPending(terminal.display) == 0) {
            fd_set readfds;
            FD_ZERO(&readfds);

            int xfd = ConnectionNumber(terminal.display);
            FD_SET(xfd, &readfds);

            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000; /* 100 milliseconds */

            select(xfd + 1, &readfds, NULL, NULL, &timeout);
        }

        if (XPending(terminal.display) > 0) {
            XNextEvent(terminal.display, &event);

            if (event.type == Expose) {
                draw_terminal(&terminal);
            } else if (event.type == KeyPress) {
                handle_keyboard(&terminal, &event.xkey);
            } else if (event.type == ButtonPress) {
                handle_mouse(&terminal, &event.xbutton);
            } else if (event.type == MotionNotify) {
                handle_mouse_motion(&terminal, &event.xmotion);
            } else if (event.type == ButtonRelease) {
                terminal.dragging_scrollbar = 0;
            }
        }
    }

    return 0;
}