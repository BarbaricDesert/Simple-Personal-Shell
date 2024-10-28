/* 
 * A tiny shell program with job control
 * 
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* Global variables */
extern char **environ;      
char prompt[] = "tsh> ";    
int verbose = 0;            
char sbuf[MAXLINE];         

struct job_t {              
    pid_t pid;              
    int jid;                
    int state;              
    char cmdline[MAXLINE];  
};
struct job_t jobs[MAXJOBS]; 

volatile sig_atomic_t ready; 

void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);
void sigusr1_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int freejid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; 

    
    dup2(STDOUT_FILENO, STDERR_FILENO);

    
    while ((c = getopt(argc, argv, "hvp")) != -1) {
        switch (c) {
            case 'h':             
                usage();
                break;
            case 'v':             
                verbose = 1;
                break;
            case 'p':             
                emit_prompt = 0;  
                break;
            default:
                usage();
        }
    }

    

    Signal(SIGUSR1, sigusr1_handler);

    
    Signal(SIGINT,  sigint_handler);   
    Signal(SIGTSTP, sigtstp_handler);  
    Signal(SIGCHLD, sigchld_handler); 

    
    Signal(SIGQUIT, sigquit_handler); 

   
    initjobs(jobs);

    
    while (1) {

        
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { 
            fflush(stdout);
            exit(0);
        }

        
        eval(cmdline);
        fflush(stdout);
    } 

    exit(0); 
}
/* 
 * eval - Evaluate the command line that the user has just typed in
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return. 
*/
void eval(char *cmdline) {
    char *argv[MAXARGS]; // Argument list for execve()
    char buf[MAXLINE];   // Holds modified command line
    int bg;              // Should the job run in bg or fg?
    pid_t pid;           // Process id
    sigset_t mask;       // Signal set for blocking

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL) return; // Ignore empty lines

    if (!builtin_cmd(argv)) {
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTSTP);
        sigprocmask(SIG_BLOCK, &mask, NULL); // Block SIGCHLD, SIGINT, SIGTSTP

        if ((pid = fork()) == 0) { // Child process
            sigprocmask(SIG_UNBLOCK, &mask, NULL); // Unblock signals
            setpgid(0, 0); // Put the child in a new process group

            if (execvp(argv[0], argv) < 0) {
                fprintf(stderr, "%s: Command not found\n", argv[0]);
                exit(1);
            }
        }

        // Parent process
        addjob(jobs, pid, bg ? BG : FG, cmdline); // Add the job to the job list

        if (!bg) {
            waitfg(pid); // Wait for foreground job to finish
        } else {
            printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); // Print background job
        }

        sigprocmask(SIG_UNBLOCK, &mask, NULL); // Unblock signals
    }
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return number of arguments parsed.
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to space or quote delimiters */
    int argc;                   /* number of args */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    return argc;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) {
    if (strcmp(argv[0], "quit") == 0) {
        // Exit the shell
        exit(0);
    } else if (strcmp(argv[0], "jobs") == 0) {
        // List all background jobs
        listjobs(jobs);
        return 1;
    } else if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) {
        // Execute the bg or fg command
        do_bgfg(argv);
        return 1;
    }
    return 0; // Not a builtin command
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) {
    struct job_t *job = NULL;
    pid_t pid;
    int jid;

    // Argument validation
    if (argv[1] == NULL) {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    // Determine if argument is PID or JID and get the job
    if (argv[1][0] == '%') {
        jid = atoi(&argv[1][1]);
        if (jid <= 0) {
            printf("%s: argument must be a positive integer\n", argv[0]);
            return;
        }
        job = getjobjid(jobs, jid);
        if (job == NULL) {
            printf("%s: No such job\n", argv[1]);
            return;
        }
        pid = job->pid;
    } else {
        pid = atoi(argv[1]);
        if (pid <= 0) {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }
        job = getjobpid(jobs, pid);
        if (job == NULL) {
            printf("(%d): No such process\n", pid);
            return;
        }
    }

    // Send the job a continue signal
    if (kill(-pid, SIGCONT) < 0) {
        unix_error("kill error");
    }

    // Change the job state and possibly wait for it
    if (strcmp(argv[0], "bg") == 0) {
        job->state = BG;
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
    } else if (strcmp(argv[0], "fg") == 0) {
        job->state = FG;
        waitfg(job->pid);
    }
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
    int status;

    if (pid < 1) {
        printf("waitfg: Invalid PID\n");
        return;
    }

    while (1) {
        pid_t wpid = waitpid(pid, &status, WUNTRACED);

        if (wpid < 0) {
            unix_error("waitfg: waitpid error");
            return;
        }

        struct job_t *job = getjobpid(jobs, pid);
        if (!job) {
            printf("waitfg: No job found for PID %d\n", pid);
            return;
        }

        if (WIFSTOPPED(status)) {
            job->state = ST;
            printf("Job [%d] (%d) stopped by signal %d\n", job->jid, pid, WSTOPSIG(status));
            break;
        } else if (WIFSIGNALED(status)) {
            printf("Job [%d] (%d) terminated by signal %d\n", job->jid, pid, WTERMSIG(status));
            deletejob(jobs, pid);  // Remove the job from the job list
            break;
        } else if (WIFEXITED(status)) {
            deletejob(jobs, pid);  // Remove the job from the job list
            break;
        }
    }
}

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) {
    int olderrno = errno;
    sigset_t mask_all, prev_all;
    pid_t pid;
    int status;

    sigfillset(&mask_all);
    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        struct job_t *job = getjobpid(jobs, pid);
        if (!job) {
            printf("sigchld_handler: No job found for PID %d\n", pid);
            continue;
        }

        if (WIFEXITED(status)) {
            // If the child exited normally, remove the job from the job list
            deletejob(jobs, pid);
            printf("Job [%d] (%d) exited with status %d\n", job->jid, pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            // If the child was terminated by a signal, remove the job from the job list
            printf("Job [%d] (%d) terminated by signal %d\n", job->jid, pid, WTERMSIG(status));
            deletejob(jobs, pid);
        } else if (WIFSTOPPED(status)) {
            // If the child was stopped by a signal, change the job's state to stopped
            job->state = ST;
            printf("Job [%d] (%d) stopped by signal %d\n", job->jid, pid, WSTOPSIG(status));
        } else if (WIFCONTINUED(status)) {
            // If the child was continued, change the job's state to background
            job->state = BG;
            printf("Job [%d] (%d) continued\n", job->jid, pid);
        }
    }

    sigprocmask(SIG_SETMASK, &prev_all, NULL); // Restore previous blocked signals
    errno = olderrno; // Restore the saved errno
}

void sigint_handler(int sig) {
    int olderrno = errno; // Save current errno

    pid_t fg_pid = fgpid(jobs); // Get the PID of the foreground job

    if (fg_pid != 0) {
        // If there is a foreground job, send SIGINT to the process group of the job
        kill(-fg_pid, SIGINT);
    }

    errno = olderrno; // Restore errno
}

void sigtstp_handler(int sig) {
    int olderrno = errno; // Save current errno to prevent side-effects

    pid_t fg_pid = fgpid(jobs); // Get the PID of the foreground job

    if (fg_pid != 0) {
        // If there is a foreground job, send SIGTSTP to the process group of the job
        kill(-fg_pid, SIGTSTP);

        // Optionally, update the job state to stopped if needed
        struct job_t *job = getjobpid(jobs, fg_pid);
        if (job != NULL) {
            job->state = ST;
        }
    }

    errno = olderrno; // Restore errno to prevent side-effects
}

/*
 * sigusr1_handler - child is ready
 */
void sigusr1_handler(int sig) {
    ready = 1;
}


/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* freejid - Returns smallest free job ID */
int freejid(struct job_t *jobs) {
    int i;
    int taken[MAXJOBS + 1] = {0};
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid != 0) 
        taken[jobs[i].jid] = 1;
    for (i = 1; i <= MAXJOBS; i++)
        if (!taken[i])
            return i;
    return 0;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    int i;
    
    if (pid < 1)
        return 0;
    int free = freejid(jobs);
    if (!free) {
        printf("Tried to create too many jobs\n");
        return 0;
    }
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = free;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    return 0; /*suppress compiler warning*/
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) {
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
                case BG: 
                    printf("Running ");
                    break;
                case FG: 
                    printf("Foreground ");
                    break;
                case ST: 
                    printf("Stopped ");
                    break;
                default:
                    printf("listjobs: Internal error: job[%d].state=%d ", 
                       i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message and terminate
 */
void usage(void) {
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg) {
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg) {
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

void sigquit_handler(int sig) {
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}


