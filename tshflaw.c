/* 
 * tsh - A tiny shell program with job control
 * 
 * <Name: 			Siyuan Hua>
 * <Andrew ID:		siyuanh>
 * <Email: 			siyuanh@andrew.cmu.edu>
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */


/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens {
    int argc;               /* Number of arguments */
    char *argv[MAXARGS];    /* The arguments list */
    char *infile;           /* The input file */
    char *outfile;          /* The output file */
    enum builtins_t {       /* Indicates if argv[0] is a builtin command */
        BUILTIN_NONE,
        BUILTIN_QUIT,
        BUILTIN_JOBS,
        BUILTIN_BG,
        BUILTIN_FG} builtins;
};

/* to save this running tsh's PID*/
static pid_t tsh_pid = 0;

/* used for redirect stdin/stdout back in parent parent process, 
 * since builtin_commands could use I/O redirections
 * */
static int saved_fd_in, saved_fd_out; 
/* End global variables */

/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void hsy(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok); 
void sigquit_handler(int sig);


void modify_job_status(pid_t pid, int job_status);
void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list); 
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid); 
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);



/*
 * main - The shell's main routine 
 */
int 
main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];    /* cmdline for fgets */
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);
	
	/* save stdin/stdout's direction */
	if(-1 == (saved_fd_in = dup(STDIN_FILENO)))
		unix_error("dup() error");
	if(-1 == (saved_fd_out = dup(STDOUT_FILENO)))
		unix_error("dup() error");
    
	/* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
	Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(job_list);


    /* Execute the shell's read/eval loop */
	tsh_pid = getpid(); // save tsh's PID for synchronizing
	while (1) {
		if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { 
            /* End of file (ctrl-d) */
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }
        
        /* Remove the trailing newline */
        cmdline[strlen(cmdline)-1] = '\0';
        
        /* Evaluate the command line */
        eval(cmdline);
        if(tcgetpgrp(STDIN_FILENO) != tsh_pid)
			tcsetpgrp(STDIN_FILENO, tsh_pid);
        fflush(stdout);
        fflush(stdout);
    } 
    close(saved_fd_in);
	close(saved_fd_out);
    exit(0); /* control never reaches here */
}


/* Description:	 Support I/O redirection in tsh, for both executable files and builtin_commands*/
void redirect(struct cmdline_tokens* p_tok)//, int mode)
{
	int fd_in,fd_out;
	if(p_tok->infile != NULL)
	{
		if(-1 != (fd_in = open(p_tok->infile, O_RDONLY)))
		{
			if(-1 != dup2(fd_in,STDIN_FILENO))
				close(fd_in);
			else
				unix_error("dup2() error");
		}
		else
			unix_error("open() error");
	}
	if(p_tok->outfile != NULL)
	{
		if(-1 != (fd_out = open(p_tok->outfile, O_WRONLY)))
		{
			if(-1 != dup2(fd_out,STDOUT_FILENO))
				close(fd_out);
			else
				unix_error("dup2() error");
		}
		else
			unix_error("open() error");
	}
}

/* Description:		We support I/O redirection for builtin_commands, but after that 
 * 					we should redirect stdin/stdout back, since it is in the parent 
 * 					process
 * */
void redirect_back()
{
	if(-1 == dup2(saved_fd_in,STDIN_FILENO))
		unix_error("dup2() redirect back error");
	if(-1 == (dup2(saved_fd_out,STDOUT_FILENO)))
		unix_error("dup2() redirect back error)");
}

/*Description:	 	parent waits for a foreground running job*/
void wait_fg(pid_t pid)
{
	struct job_t* p_job_entry = getjobpid(job_list,pid);
	while(p_job_entry && p_job_entry->state == FG) /* if current pid == fg process's pid, 
													  then wait for fg process */
		;											/* do nothing, just for next test*/
}

/* Description:		For builtin_commands fg/bg's use*/
void continue_job(struct job_t* p_job_entry, int is_bg)
{
	if(!kill(p_job_entry->pid,SIGCONT))
	{
		modify_job_status(p_job_entry->pid,is_bg?BG:FG); 
		if(!is_bg)  // if it becomes a fg job, we should wait for it
		{
			wait_fg(p_job_entry->pid);
		}
		else  //if a bg job then continues
		{
			printf("[%d] (%d) %s\n",p_job_entry->jid, p_job_entry->pid, p_job_entry->cmdline);
		}
	}
	else
		unix_error("kill() error");
}

/* Description: 	execute a bg builtin_command, include error checking */
void bg_job(char** argv)
{
	if(argv[1]) // if bg has no argument, output a warning
	{
		if(argv[1][0] == '%') // if input: tsh> bg %jibid
		{
			int job_id = atoi((char*)&argv[1][1]);
			struct job_t* p_job_entry = getjobjid(job_list, job_id);
			if(p_job_entry) // if find such a job in the joblist
			{
				if(p_job_entry->state == ST)  /* and if its status is stopped, then change to 
												 running in the background */
					continue_job(p_job_entry, 1);
			}
			else //no such job
			{
				printf("%%%d: No such job\n",job_id);
			}
		}
		else if(isdigit(argv[1][0]))// if input: tsh> bg PID
		{
			int pid = atoi(argv[1]);
			struct job_t* p_job_entry = getjobpid(job_list, pid); 
			if(p_job_entry) //find such a job
			{
				if(p_job_entry->state == ST)
					continue_job(p_job_entry,1);
			}
			else
			{
				printf("(%d): No such process\n",pid);
			}
		}
		else
			printf("bg command requires PID or %%jobid argument\n");
	}
	else
		printf("bg command requires PID or %%jobid argument\n");
}

/* Description:		execute a fg builtin_command, include error checking */
void fg_job(char** argv)
{
	if(argv[1]) // if fg has no argument, output a warning
	{
		if(argv[1][0] == '%') // if input: tsh> fg %jibid
		{
			int job_id = atoi((char*)&argv[1][1]);
			struct job_t* p_job_entry = getjobjid(job_list, job_id);
			if(p_job_entry) // if find such a job, then change its status to running in foreground
			{
				continue_job(p_job_entry,0);
			}
			else
			{
				printf("%%%d: No such job\n",job_id);
			}
		}
		else if(isdigit(argv[1][0])) // if input: tsh> fg PID
		{
			int pid = atoi(argv[1]);
			struct job_t* p_job_entry = getjobpid(job_list, pid); 
			if(p_job_entry) //find such a job
			{
				continue_job(p_job_entry,0);
			}
			else
			{
				printf("(%d): No such process\n",pid);
			}
		}
		else
			printf("fg command requires PID or %%jobid argument\n");
	}
	else
		printf("fg command requires PID or %%jobid argument\n");
}

/* Description: 	To check if the input line is a builtin_command 
 * Return: 			1 if a builtin_command; 0 if not
 * */
int builtin_command(struct cmdline_tokens* p_tok)
{
	redirect(p_tok); // support I/O redirection
	if(!strcmp(p_tok->argv[0],"quit"))
		exit(0);
	else if(!strcmp(p_tok->argv[0], "jobs"))
	{
		listjobs(job_list,STDOUT_FILENO);
		redirect_back();   // must redicrect back, since it's in parent process
		return 1;
	}
	else if(!strcmp(p_tok->argv[0], "fg"))
	{
		fg_job(p_tok->argv);
		redirect_back(); // redirect back
		return 1;
	}
	else if(!strcmp(p_tok->argv[0], "bg"))
	{
		bg_job(p_tok->argv);
		redirect_back(); // redirect back
		return 1;
	}
	redirect_back(); 
	return 0; //else, not builtin-command
}



/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void 
eval(char *cmdline) 
{
    int bg;              /* should the job run in bg or fg? */
    struct cmdline_tokens tok;

    /* Parse command line */
    bg = parseline(cmdline, &tok); 

    if (bg == -1) return;               /* parsing error */
    if (tok.argv[0] == NULL)  return;   /* ignore empty lines */
	
	
	if(!builtin_command(&tok)) // if not builtin_command
	{
		pid_t pid;
		sigset_t oldmask, newmask;
		if(sigemptyset(&newmask) != 0)		
			unix_error("sigemptyset error");
		if(sigaddset(&newmask,SIGCHLD) != 0)
			unix_error("sigaddset error");
		if(sigaddset(&newmask,SIGINT) != 0)
			unix_error("sigaddset error");
		if(sigaddset(&newmask,SIGTSTP) != 0)
			unix_error("sigaddset error");
		if(sigprocmask(SIG_BLOCK,&newmask,&oldmask) < 0) /* block SIGCHLD,SIGTSTP,SIGINT and
															save current signal mask before blocking */
			unix_error("SIG_BLOCK error");

		if((pid = fork()) < 0)
			unix_error("fork() error");
		else if(0 == pid) /* child */
		{
			if(sigprocmask(SIG_SETMASK,&oldmask,NULL) < 0) /* Since singal sets will be inherited after 
															  fork() in child we have to unblock 
															  SIGCHLD,SIGINT,SIGTSTP */
				unix_error("SIG_SETMASK error");
			
			setpgid(0,0);  //child in another process group, not in the same PG as tsh
			if(!bg) //if the child is fg job
				tcsetpgrp(STDIN_FILENO, getpgrp());	/* then change child to be a REAL fg process group, so fg child
													   can get control of input/output of controlling terminal*/
				
			/* support I/O redirection in child process, open filedes will be inherited after exec */
			redirect(&tok);
			
			if(execve(tok.argv[0],tok.argv,environ) < 0)
			{
				printf("%s: Command not found\n", tok.argv[0]);
				exit(0);
			}
		}
		else  /* parent */
		{
			addjob(job_list,pid,bg?BG:FG,cmdline);
			if(sigprocmask(SIG_SETMASK,&oldmask,NULL) < 0) /* after add child to job list, then 
															  unblock blocked signals in parent*/
				unix_error("SIG_SETMASK error");
	
			if(!bg) /* if a foreground job */
			{
				wait_fg(pid);
			}
			else /*if a background job */
			{
				struct job_t* p_job_entry = getjobpid(job_list, pid);
				printf("[%d] (%d) %s\n",p_job_entry->jid,p_job_entry->pid,p_job_entry->cmdline);
			}
		}
	}
   	return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. Characters 
 *             enclosed in single or double quotes are treated as a single
 *             argument. 
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job  
 *  -1:        if cmdline is incorrectly formatted
 * 
 * Note:       The string elements of tok (e.g., argv[], infile, outfile) 
 *             are statically allocated inside parseline() and will be 
 *             overwritten the next time this function is invoked.
 */
int 
parseline(const char *cmdline, struct cmdline_tokens *tok) 
{

    static char array[MAXLINE];          /* holds local copy of command line */
    const char delims[10] = " \t\r\n";   /* argument delimiters (white-space) */
    char *buf = array;                   /* ptr that traverses command line */
    char *next;                          /* ptr to the end of the current arg */
    char *endbuf;                        /* ptr to the end of the cmdline string */
    int is_bg;                           /* background job? */

    int parsing_state;                   /* indicates if the next token is the
                                            input or output file */

    if (cmdline == NULL) {
        (void) fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void) strncpy(buf, cmdline, MAXLINE);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf) {
        /* Skip the white-spaces */
        buf += strspn (buf, delims);
        if (buf >= endbuf) break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<') {
            if (tok->infile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>') {
            if (tok->outfile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf ++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"') {
            /* Detect quoted tokens */
            buf++;
            next = strchr (buf, *(buf-1));
        } else {
            /* Find next delimiter */
            next = buf + strcspn (buf, delims);
        }
        
        if (next == NULL) {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* Record the token as either the next argument or the input/output file */
        switch (parsing_state) {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS-1) break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL) {
        (void) fprintf(stderr, "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0)  /* ignore blank line */
        return 1;

    if (!strcmp(tok->argv[0], "quit")) {                 /* quit command */
        tok->builtins = BUILTIN_QUIT;
    } else if (!strcmp(tok->argv[0], "jobs")) {          /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    } else if (!strcmp(tok->argv[0], "bg")) {            /* bg command */
        tok->builtins = BUILTIN_BG;
    } else if (!strcmp(tok->argv[0], "fg")) {            /* fg command */
        tok->builtins = BUILTIN_FG;
    } else {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The 
 *     handler reaps all available zombie children, but doesn't wait 
 *     for any other currently running children to terminate.  
 */
void 
sigchld_handler(int sig) 
{
	pid_t pid;
	int status;
	while((pid = waitpid(-1,&status,WNOHANG | WUNTRACED)) > 0) // reap child with no blocking, if not reaped termination status, return 0
	{
		if(WIFEXITED(status)) // if it's child's termination(normally) leading waitpid to return, then delete job
		{
			deletejob(job_list, pid);
		}
		else if(WIFSIGNALED(status)) // if child terminated abnormally
		{
			printf("Job [%d] (%d) terminated by signal %d\n",pid2jid(pid),pid,WTERMSIG(status));
			deletejob(job_list, pid);
		}
		else if(WIFSTOPPED(status)) //if the child is stopped, not terminated
		{
			modify_job_status(pid,ST);
			printf("Job [%d] (%d) stopped by signal %d\n",pid2jid(pid),pid,WSTOPSIG(status));
		}
	}
//	if(errno != ECHILD)
//		unix_error("waitpid() error");
	return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void 
sigint_handler(int sig) 
{
	pid_t pid = fgpid(job_list);
	if(pid)  // if pid ==0, i.e. no fg job, then return directly
	{
		if(kill(-pid,SIGINT))
			unix_error("kill() error");
	}
	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void
sigtstp_handler(int sig) 
{
	pid_t pid = fgpid(job_list);
	if(pid)  // if pid ==0, i.e. no fg job, then return directly
	{
		if(kill(-pid,SIGTSTP)) 
			unix_error("kill() error");
	}
	return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */

void modify_job_status(pid_t pid, int job_status)
{
	struct job_t* p_job_entry = getjobpid(job_list, pid);
	p_job_entry->state = job_status;
}

void 
clearjob(struct job_t *job) {
    job->pid = 0; // all PIDs in the job list are initialized to 0
    job->jid = 0; 
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void 
initjobs(struct job_t *job_list) {
    int i;
    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int 
maxjid(struct job_t *job_list) 
{
    int i, max=0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int 
addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline) 
{
    int i;
    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1; // to ensure that nextjid in range of [1,16]
            strcpy(job_list[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", job_list[i].jid, job_list[i].pid, job_list[i].cmdline);
            }
            return 1;
        } // if job_list[i].PID != 0, then current entry is used, goto check next entry.
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int 
deletejob(struct job_t *job_list, pid_t pid) 
{
    int i;
    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t 
fgpid(struct job_t *job_list) {
    int i;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;  // return the pid of process in the fg job, or return 0
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t 
*getjobpid(struct job_t *job_list, pid_t pid) {
    int i;
    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid) 
{
    int i;
    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int 
pid2jid(pid_t pid) 
{
    int i;
    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0; // if failure return 0
}

/* listjobs - Print the job list */
void 
listjobs(struct job_t *job_list, int output_fd) 
{
    int i;
    char buf[MAXLINE];

    for (i = 0; i < MAXJOBS; i++) {
        memset(buf, '\0', MAXLINE); // all bits in buf[MAXLINE] are '\0'
        if (job_list[i].pid != 0) {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            switch (job_list[i].state) {
            case BG:
                sprintf(buf, "Running    ");
                break;
            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
    if(output_fd != STDOUT_FILENO)
        close(output_fd);
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void 
usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void 
unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void 
app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t 
*Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void 
sigquit_handler(int sig) 
{
	pid_t pid = fgpid(job_list);
	if(!pid)
	{
    	printf("Terminating after receipt of SIGQUIT signal\n");
    	exit(1);
	}
	else
	{
		if(kill(pid,SIGQUIT))
			unix_error("kill() error");
	}
}

