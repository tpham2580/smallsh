#define  _POSIX_C_SOURCE 200809L
#define _POSIX_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

//   smallsh is a shell program written in C with the following features:
//      1. Provide a prompt for running commands
//      2. Handle blank lines and comments, which are lines beginning with 
//         the # character
//      3. Provide expansion for the variable $$
//      4. Execute 3 commands exit, cd, and status via code built into the shell
//      5. Execute other commands by creating new processes using a function
//         from the exec family of functions
//      6. Support input and output redirection
//      7. Support running commands in foreground and background processes
//      8. Implement custom handlers for 2 signals, SIGINT and SIGTSTP

struct command_line {
    char *command;
    char **args;
    int argc;
    char *input_file;
    char *output_file;
    bool bg_process;
};

// Global Var to handle SIGTSTP signal
volatile sig_atomic_t foreground_only = 0;

// sets SIG_IGN as handler for SIGINT, which will be ignored
void ignoreSIGINT() {
    // init ignore_action struct
    struct sigaction ignore_action = {{0}};

    // Assign signal handler SIG_IGN so that action struct is ignored
    ignore_action.sa_handler = SIG_IGN;

    // Register ignore_action as signal handler for SIGINT
    sigaction(SIGINT, &ignore_action, NULL);
}

// Sets SIG_DFL as handler for SIGIT, which will stop it from being ignored
void restoreSIGINT() {
    // init restore_action struct
    struct sigaction restore_action = {{0}};

    // Assign signal handler SIG_DFL to restore_action struct
    restore_action.sa_handler = SIG_DFL;

    // Register restore_action as the signal handler for SIGINT
    sigaction(SIGINT, &restore_action, NULL);
}

// Switches foreground-only mode on and off and
void handleSIGTSTP(int signo) {
    if (foreground_only) {
        char *message2 = "\nExiting foreground-only mode\n: ";
        write(STDOUT_FILENO, message2, 32);
        fflush(stdout);
        // set foreground only mode to false
        foreground_only = 0; 
    } else {
        char *message3 = "\nEntering foreground-only mode (& is now ignored)\n: ";
        write(STDOUT_FILENO, message3, 53);
        fflush(stdout);
        // set foreground only to true
        foreground_only = 1;
    }
}

// Have child foreground and background processes ignore SIGTSTP
void ignoreSIGTSTP() {
    // init ignore_action struct
    struct sigaction ignore_action = {{0}};

    // Assign signal handler SIG_IGN to ignore_action struct
    ignore_action.sa_handler = SIG_IGN;

    // Register ignore_action as the signal handler for SIGTSTP
    sigaction(SIGTSTP, &ignore_action, NULL);
}

// Init SIGTSTP_action struct and assign the appropriate struct members
void signalHandler() {
    // Init SIGTSTP_action
    struct sigaction SIGTSTP_action = {{0}};

    // Register handle_SIGSTP as the signal handler
    SIGTSTP_action.sa_handler = handleSIGTSTP;

    // Block all catchable signals while handleSIGTSTP is running
    sigfillset(&SIGTSTP_action.sa_mask);

    // SA_RESTART flag will automatically restart for the system call/library 
    // that was interrupted when the signal handler is finished
    // Otherwise, it returns an error if interupted
    SIGTSTP_action.sa_flags = SA_RESTART;

    // Installs the signal handler
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}

// removes value at a specific index in array
void removeVal(int *arr, int *length, int index) {
    for (int i = index; i < *length - 1; i++) {
        arr[i] = arr[i + 1];
    }
    *length -= 1;
}

// Goes through BG process array and uses waitpid on each process
void checkBgProcess(int *bg_processes, int *bg_count, int *status) {
    int i;
    int length = *bg_count;
    int check_pid;
    int child_status;
    for (i = 0; i < length; i++) {
        check_pid = waitpid(bg_processes[i], &child_status, WNOHANG);

        // if waitpid returnes the child pid, the child process is complete and
        // can be removed from the bg_processes list
        if (check_pid == bg_processes[i]) {
            // remove PID from bg_processes list
            removeVal(bg_processes, bg_count, i);
            // if value was removed, go back by one
            i -= 1;
            // display background PID status and exit value 
            // otherwise terminate signal
            if(WIFEXITED(child_status)) {
                printf("background pid %d is done: exit value %d\n", check_pid, WEXITSTATUS(child_status));
                fflush(stdout);
            } else {
                printf("background pid %d is done: terminated by signal %d\n", check_pid, WTERMSIG(child_status));
                fflush(stdout);
            }
        }
    }
}

// expands all instances of "$$" in a command to the PID
char *expandVar(char *cmd_str) {
    // allocate memory for new string after variable expansion
    char *cmd_expanded = calloc(2048, sizeof(char));        
    char *var = "$$"; 
    char *return_str;    // point to substring returned from strstr
    char *str_pos = cmd_str;  // keep track of position in cmd_str
    char *pid = calloc(sizeof(char), 100); // used to convert PID to string

    // convert PID to string
    sprintf(pid, "%d", getpid());

    // search for var in string pointed to by str_pos
    return_str = strstr(str_pos, var);

    // continue to search for var and replace with PID, move str_pos down
    // the command_line string
    while (return_str != NULL) {
        // concatenate the string up until the var is found
        strncat(cmd_expanded, str_pos, (strlen(str_pos) - strlen(return_str)));

        // concatenate the PID and replace var
        strcat(cmd_expanded, pid);

        // move pointer to position after found var
        str_pos += strlen(str_pos) - strlen(return_str) + strlen(var);

        // search for next var and assign substring to return_str
        return_str = strstr(str_pos, var);
    }
    // add on the last of the string after final $$ is found
    // -1 is to avoid newline character
    strncat(cmd_expanded, str_pos, strlen(str_pos) - 1);
    free(pid);
    return cmd_expanded;
}

void initStruct(struct command_line *cmd_line_parsed) {
    cmd_line_parsed->command = NULL;
    cmd_line_parsed->args = NULL;
    cmd_line_parsed->argc = 0;
    cmd_line_parsed->input_file = NULL;
    cmd_line_parsed->output_file = NULL;
    cmd_line_parsed->bg_process = 0;
}

// Parses the command line, using strtok_r to get tokens, 
// check for any special symbols, and store the command in
// an array. All data is saved to struct.
// info on strtok_r: https://linux.die.net/man/3/strtok_r
struct command_line *parseCmd(char *command_line_str) {
    // allocate memory for parsed command line struct
    struct command_line *cmd_line_parsed = malloc(sizeof(struct command_line));
    // initialize the new command line struct to NULL and 0 values
    initStruct(cmd_line_parsed);
    
    char *ptr;

    // add command, which is first token, to command_line struct
    char *token = strtok_r(command_line_str, " ", &ptr);
    cmd_line_parsed->command = calloc(strlen(token) + 1, sizeof(char));
    strcpy(cmd_line_parsed->command, token);

    // allocate memory for arg list (per assignment specs, max 512 arguments)
    char **args_list = malloc(512 * sizeof(*args_list));

    // allocate memory and copy in 1st token (command) to args_list
    args_list[0] = calloc(strlen(token) + 1, sizeof(char));
    strcpy(args_list[0], token);
    int argc = 1;     // keep track of length of args_list

    // Get next token before entering while loop
    token = strtok_r(NULL, " ", &ptr);
    while (token) {
        if ((token[0] == '<') & (strlen(token) == 1)) {
            // if < , next token will be input_file
            token = strtok_r(NULL, " ", &ptr);
            cmd_line_parsed->input_file = calloc(strlen(token) + 1, sizeof(char));
            strcpy(cmd_line_parsed->input_file, token);
        }
        else if ((token[0] == '>') & (strlen(token) == 1)) {
            // if >, next token will be output_file
            token = strtok_r(NULL, " ", &ptr);
            cmd_line_parsed->output_file = calloc(strlen(token) + 1, sizeof(char));
            strcpy(cmd_line_parsed->output_file, token);
        }
        else {
            // if none of the above apply, add token to args array 
            // increment argc
            args_list[argc] = calloc(strlen(token) + 1, sizeof(char));
            strcpy(args_list[argc], token);
            argc++;
        }
        // get next token and repeat loop
        token = strtok_r(NULL, " ", &ptr);
    }
    // if there is more than one arg, check for & character, 
    //      if present, update command_line struct member and remove &
    if (argc > 1) {
        if (!strcmp(args_list[argc - 1], "&")) {
            cmd_line_parsed->bg_process = true;
            free(args_list[argc - 1]);
            argc--;  
        }
    }
    // assign args list and count to command_line struct
    cmd_line_parsed->args = args_list;
    cmd_line_parsed->argc = argc;
        
    return cmd_line_parsed;
}

// change the cwd to the specified path
void changeDir(char *envpath) {
    int cd_num = chdir(envpath);
    // On chdir success, zero is returned.  On error, -1 is returned
    if (cd_num == -1) {
        printf("Error changing directories.\n");
        fflush(stdout);
    }
    char *buffer = NULL;
    size_t len = 0;
    // src: https://man7.org/linux/man-pages/man2/getcwd.2.html
    char *cwd = getcwd(buffer, len);
    free(cwd);
}

// displays exit status or terminating signal
// If status is 0 or 1, print the exit status message
// Otherwise, if status is greater than 1, print terminating message
void displayStatus(int *status) {
    if ((*status == 0) || (*status == 1)) {
        printf("exit value %d\n", *status);
        fflush(stdout);
    } else {
        printf("terminated by signal %d\n", *status);
        fflush(stdout);
    }
}

// redirects the input file
// default redirection to /dev/null if file not specified
void redirectInput(struct command_line *command_line, int *status) {
    if (command_line->input_file) {
        // open input file
        int input_fd = open(command_line->input_file, O_RDONLY);
        if (input_fd == -1) {
            printf("unable too open %s for input\n", command_line->input_file);
            fflush(stdout);
            exit(1);
        }
        // redirect stdin to input file
        int result = dup2(input_fd, 0);
        if (result == -1) {
            printf("error redirecting stdin to input file\n");
            fflush(stdout);
            exit(1);
        }
    }
    if (command_line->bg_process & !foreground_only & !command_line->input_file) {
        int input_fd = open("/dev/null", O_RDONLY);
        if (input_fd == -1) {
            printf("unable to open /dev/null for input\n");
            fflush(stdout);
            exit(1);
        }
        int result = dup2(input_fd, 0);
        if (result == -1) {
            printf("error redirecting stdin to /dev/null\n");
            fflush(stdout);
            exit(1);
        }
    }
}

// redirects the output file
// default redirection to /dev/null if file not specified
void redirectOutput(struct command_line *command_line, int *status) {
    if (command_line->output_file) {
        // open output file
        int output_fd = open(command_line->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (output_fd == -1) {
            printf("unable to open %s for output\n", command_line->output_file);
            fflush(stdout);
            exit(1);
        }
        // redirect stdout to output file
        int result = dup2(output_fd, 1);
        if (result == -1) {
            printf("error redirecting stdout to output file\n");
            fflush(stdout);
            exit(1);
        }
    }
    if (command_line->bg_process & !foreground_only
            & !command_line->output_file) 
    {
        int output_fd = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (output_fd == -1) {
            printf("unable to open /dev/null for output\n");
            fflush(stdout);
            exit(1);
        }
        int result = dup2(output_fd, 1);
        if (result == -1) {
            printf("error redirecting stdout to /dev/null\n");
            fflush(stdout);
            exit(1);
        }
    }
}

// Forks a child process
void forkChild(struct command_line *command_line, int *status, int *bg_processes, int *bg_count) 
{
    int child_status;
    pid_t new_pid = fork();

    // If process to run in background and program is NOT in foreground 
    // only mode, add child PID to background_proc array
    if (command_line->bg_process & !foreground_only) {
        bg_processes[*bg_count] = new_pid;
        *bg_count += 1;
    }
    switch(new_pid) {
        case -1:
            perror("fork()\n");
            exit(1);
            break;
        case 0: ;
            // when it is a child process
            // If process is to run in foreground, restore SIGINT
            if (!command_line->bg_process || !foreground_only) {
                restoreSIGINT();
            }

            // Child processes ignore SIGTSTP
            ignoreSIGTSTP();

            // Setup input and output redirection
            redirectInput(command_line, status);
            redirectOutput(command_line, status);
            execvp(command_line->args[0], command_line->args);
            perror(command_line->args[0]);
            exit(1);
            break;
        default:
            // When it is a parent process
            if (command_line->bg_process & !foreground_only) {
                // run child in background, do not wait for child to terminate
                printf("background PID is %d\n", new_pid);
                fflush(stdout);
            } else {
                // run child in foreground and wait for child to terminate
                new_pid = waitpid(new_pid, &child_status, 0);

                // check exit status of foreground process
                if (WIFEXITED(child_status)) {
                    *status = WEXITSTATUS(child_status);
                } else {
                    printf("terminated by signal %d\n", WTERMSIG(child_status));
                    fflush(stdout);
                    *status = WTERMSIG(child_status);
                }
            }
            break;
    }
}

// Executes the proper command depending on the command
// If command is "cd" or status "status", will call change
// directory or get status
// Otherwise, will call fork child function
void executeCmd(struct command_line *command_line, int *status,
                         int *bg_processes, int *bg_count) 
{
    if (!strcmp(command_line->command, "cd")) {
        // Handle "cd" command
        if (command_line->argc == 1) {
            // change directory to Home environment variable if "cd" is
            // the only arg in args_list
            changeDir(getenv("HOME"));
        } else {
            // change directory to path specified after "cd" command
            changeDir(command_line->args[1]);
        }
    }
    else if (!strcmp(command_line->command, "status")) {
        // handle status command    
        displayStatus(status);
    }
    else {
        // add NULL to args list
        command_line->args[command_line->argc] = NULL;
        command_line->argc += 1;
        forkChild(command_line, status, bg_processes, bg_count);
    }
}

void killAllProcesses(int *background_procs, int bg_count) {
    int child_status;
    for (int i = 0; i < bg_count; i++) {
        kill(background_procs[i], SIGKILL);
        waitpid(background_procs[i], &child_status, WNOHANG);
    }
}

// prompts user and gets the command line string
char *getCMD() {
    char *buffer = NULL;  
    size_t len = 0;
    ssize_t lread;                  
    printf(": ");
    fflush(stdout);
    lread = getline(&buffer, &len, stdin);
    if (lread == -1) {
        printf("error reading line\n");
    }
    return buffer;
}

int main(int arc, char **argv) {
    ignoreSIGINT(); // parent and background process ignore SIGINT
    signalHandler(); // create signal handler for SIGTSTP

    int status = 0;
    char *cmd_str = NULL;  // used to read command line from user
    char *cmd_expanded;    // points to string after variable expansion
    struct command_line *cmd_parsed;

    // allocate memory for array to hold PIDs of background processes PIDs
    int *bg_processes = calloc(100, sizeof(int));     
    int bg_count = 0;    // length of bg_processes array
    // outer loop checks for exit command
    do {
        // inner loop checks background processes and gets user command
        do {
            // free previous cmd_str
            free(cmd_str);
            // check status of background processes
            checkBgProcess(bg_processes, &bg_count, &status);
            cmd_str = getCMD();
        } while (isspace(cmd_str[0]) | (cmd_str[0] == '#'));

        // if 1st command not exit, perform variable expansion, parse the
        // command line, and then execute command
        if (strcmp("exit\n", cmd_str)) {
            cmd_expanded = expandVar(cmd_str);
            cmd_parsed = parseCmd(cmd_expanded);
            executeCmd(cmd_parsed, &status, bg_processes, &bg_count);
            free(cmd_expanded);
            free(cmd_parsed->command);
            free(cmd_parsed->input_file);
            free(cmd_parsed->output_file);
            for (int i = 0; i < cmd_parsed->argc; i++) {
                free(cmd_parsed->args[i]);
            }
            free(cmd_parsed->args);
            free(cmd_parsed);
        }
    } while (strcmp("exit\n", cmd_str));
    // exit
    // kills all processes before exiting
    killAllProcesses(bg_processes, bg_count);
    free(bg_processes);
    free(cmd_str);

    return 0;
}