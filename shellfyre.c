#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>


const char *sysname = "shellfyre";
const char cwdHistory[1024];

enum return_codes
{
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t
{
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3];		// in/out redirection
	struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command)
{
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");
	for (i = 0; i < 3; i++)
		printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i = 0; i < command->arg_count; ++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i = 0; i < 3; ++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next = NULL;
	}
	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);
	while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
		buf[--len] = 0; // trim right whitespace

	if (len > 0 && buf[len - 1] == '?') // auto-complete
		command->auto_complete = true;
	if (len > 0 && buf[len - 1] == '&') // background
		command->background = true;

	char *pch = strtok(buf, splitters);
	command->name = (char *)malloc(strlen(pch) + 1);
	if (pch == NULL)
		command->name[0] = 0;
	else
		strcpy(command->name, pch);

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		if (len == 0)
			continue;										 // empty arg, go for next
		while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
			arg[--len] = 0; // trim right whitespace
		if (len == 0)
			continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|") == 0)
		{
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0)
			continue; // handled before

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<')
			redirect_index = 0;
		if (arg[0] == '>')
		{
			if (len > 1 && arg[1] == '>')
			{
				redirect_index = 2;
				arg++;
				len--;
			}
			else
				redirect_index = 1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"') || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}
		command->args = (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;
	return 0;
}

void prompt_backspace()
{
	putchar(8);	  // go back 1
	putchar(' '); // write empty over
	putchar(8);	  // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	// FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state = 0;
	buf[0] = 0;

	while (1)
	{
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c == 9) // handle tab
		{
			buf[index++] = '?'; // autocomplete
			break;
		}

		if (c == 127) // handle backspace
		{
			if (index > 0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c == 27 && multicode_state == 0) // handle multi-code keys
		{
			multicode_state = 1;
			continue;
		}
		if (c == 91 && multicode_state == 1)
		{
			multicode_state = 2;
			continue;
		}
		if (c == 65 && multicode_state == 2) // up arrow
		{
			int i;
			while (index > 0)
			{
				prompt_backspace();
				index--;
			}
			for (i = 0; oldbuf[i]; ++i)
			{
				putchar(oldbuf[i]);
				buf[i] = oldbuf[i];
			}
			index = i;
			continue;
		}
		else
			multicode_state = 0;

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}
	if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
		index--;
	buf[index++] = 0; // null terminate string

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);
void formatFilePath(char* path);
void recursiveFileSearch(char* path, bool open, char *argName, char *dirUntilNow);
void executeTake(char *arg, char *path);
int main()
{
	getcwd(cwdHistory, sizeof(cwdHistory));
	printf("directory: %s\n",cwdHistory);
	formatFilePath(cwdHistory);
	printf("directory: %s\n",cwdHistory);
	while (1)
	{
		struct command_t *command = malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code == EXIT)
			break;

		code = process_command(command);
		if (code == EXIT)
			break;

		free_command(command);
	}

	printf("\n");
	return 0;
}
char* getFilePath(char* cmd);

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "") == 0)
		return SUCCESS;

	if (strcmp(command->name, "exit") == 0)
		return EXIT;

	if (strcmp(command->name, "cd") == 0)
	{
		if (command->arg_count > 0)
		{
			r = chdir(command->args[0]);
			if (r == -1){
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			}else{
				
				char directory[128] = "echo ";
				strcat(directory, command->args[0]);
				strcat(directory, " >> ");
				strcat(directory, cwdHistory);
				strcat(directory, "/.test1.txt");
				system(directory);
			}
			return SUCCESS;
		}
	}

	// TODO: Implement your custom commands here

	pid_t pid = fork();

	char cwd[5000];

	getcwd(cwd, sizeof(cwd));

	if (pid == 0) // child
	{
		if (strcmp(command->name, "filesearch") == 0) {
		     char *p_r = "-r";
		     char *p_o = "-o";

		     bool recursion = false;
		     bool open = false;

		     char *argName;

		     if (command->arg_count == 1) {
		         // turn recursion flag off
			 // turn open flag off
			 recursion = false;
			 open = false;
			 argName = command->args[0];
		     } else if (command->arg_count == 2) {
			 argName = command->args[1];
		         if (strcmp(command->args[1], p_o) == 0) {
			     // turn open flag on
			     // turn recursion flag off
			     recursion = false;
			     open = true;
			 } else {
    		             // turn open flag off
			     // turn recursion flag on
			     recursion = true;
			     open = false;
			 }
		     } else {
			 argName = command->args[2];
		         // turn open flag on
			 // turn recursion flag on
			 recursion = true;
			 open = true;
		     }

		     char dirName[255];
		     char *slashDot = "./";
		     strcpy(dirName, slashDot);
		     
		     if (recursion) {
			 if (open) {
		             recursiveFileSearch(cwd, open, argName, dirName);
		 	 } else {
			     recursiveFileSearch(cwd, open, argName, dirName);
			 }
		     } else {
		         // execute regular file search without recursion
			 DIR *d;
			 struct dirent *dir;

			 d = opendir(cwd);
			 
			 if (d) { 
		             while ((dir = readdir(d)) != NULL) {
			         int arglength = strlen(argName);
			         char *dir_name = dir->d_name;
			         int nameLength = strlen(dir_name);

			         bool flag = 0;
			         int i = 0;
			         for (i = 0; i < nameLength; i++) {
				     if (argName[0] == dir_name[i]) {
				         bool flag2 = 1;
					 int j = 0;
					 for (j = 0; j < arglength; j++) {
					     if (dir_name[i + j] != argName[j]) {
					         flag2 = 0;
					     }
					 }
					 if (flag2 == 1) {
					     flag = 1;
					     break;
					 }
				     }
				 }	         
			         if (flag == 1) {
				     printf("./%s\n", dir_name);
				 } 	 
			     }
			 } 
		     }
		     exit(0);
                } else if (strcmp(command->name, "take") == 0) {
		    char *arg = command->args[0];
		    char cwd[5000];
		    getcwd(cwd, sizeof(cwd));
		    executeTake(arg, cwd);
		    exit(0);
		} else {

		// increase args size by 2
		command->args = (char **)realloc(
			command->args, sizeof(char *) * (command->arg_count += 2));

		// shift everything forwar"d by 1
		for (int i = command->arg_count - 2; i > 0; --i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a copy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		/// TODO: do your own exec with path resolving using execv()
		char *path = getFilePath(command->name);
		if (execv(path, command->args) == -1) {
		  printf("returns -1/n");
		}
		exit(0);
		exit(0);

		}
	}
	else
	{
		/// TODO: Wait for child to finish if command is not running in background
		if (command->background == 0) {
		    wait(NULL);
		}
		//      wait(NULL);

		return SUCCESS;
	}

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}

void executeTake(char *arg, char *path) {

    printf("enter method\n");

    printf("arg value is %s\n", arg);

    printf("ABCDJLKA\n");

    char thisDir[256];
    char next = arg[0];
    int index = 0;

    printf("enter\n");

    while (next != '/' && next != '\0') {

	    printf("%c\n", next);
        thisDir[index] = next;
	index++;
	next = arg[index];
    }

    thisDir[index] = '\0';

    char newPath[5000];
    strcpy(newPath, path);
    strcat(newPath, "/");
    strcat(newPath, thisDir);

    // DIR *dir = opendir(newPath);


    printf("the new path dir is %s\n", newPath);

    struct stat stats;
    stat(newPath, &stats);

    bool check;
    if (S_ISDIR(stats.st_mode) == 1) {

	printf("directory exists\n");
        check = 1;
    } else {
        check = 0;
    }

    if (check == 0) {
	printf("enoent works\n");
        // directory doesn't exist
	char *mkdir = getFilePath("mkdir");
	char *args[] = {thisDir, NULL};
	printf("here\n");
	execl(mkdir, mkdir, thisDir, NULL);
    }
    pid_t pid = fork();
    if (pid == 0) {
        exit(0);
    }
    if (chdir(newPath) == 0) {
        printf("succes\n");
    } else {
        printf("fail\n");
    }
    if (next == '/') {
        executeTake((arg + (index + 1)), newPath);   
    } 
}

void recursiveFileSearch(char* path, bool open, char *argName, char *dirUntilNow) {
    DIR *d;
    struct dirent *dir;
    d = opendir(path);

    if (d) {
        while((dir = readdir(d)) != NULL) {
	    char *dir_name = dir->d_name;
	    int arglength = strlen(argName);
	    int nameLength = strlen(dir_name);

	    bool only_dots = 1;
	    int dir_length = strlen(dir_name);

	    int k = 0;
	    for (k = 0; k < dir_length; k++) {
	        if (dir_name[k] != '.') {
		    only_dots = 0;
		}
	    }
	    if (only_dots == 0) {
	        bool flag = 0;
	        int i = 0;
	        for (i = 0; i < nameLength; i++) {
	            if (argName[0] == dir_name[i]) {
		        bool flag2 =1;
		        int j = 0;
		        for (j = 0; j < arglength; j++) {
		            if (dir_name[i + j] != argName[j]) {
			        flag2 = 0;
			    }
		        }
		        if (flag2 == 1) {
		            flag = 1;
			    break;
		        }
		    }
	        }
	        char string[5000];
	        strcpy(string, dirUntilNow);
	        strcat(string, dir_name);
	        char updatedString[5000];
	        char *slash = "/";
	        strcpy(updatedString, path);
	        strcat(updatedString, slash);
	        strcat(updatedString, dir_name);
	        if (flag == 1) {
	            printf("%s\n", string);
		    if (open) {
                        struct stat path_stats;
			stat(updatedString, &path_stats);
			if (S_ISREG(path_stats.st_mode)) {
			    char *path2 = getFilePath("xdg-open");
			    char file_name[255];
			    strcpy(file_name, (string + 2));
		            pid_t pid = fork();
		            if (pid == 0) {	    
              		        execl(path2, path2, file_name, NULL);
			        exit(0);
			    }
			    wait(0);  
		        }
		    }
	        }
		strcat(string, slash);
	        recursiveFileSearch(updatedString, open, argName, string);
	    }
	}
    }
}

char* getFilePath(char* cmd){
	char whichCommand[128] = "which ";
	strcat(whichCommand, cmd);
	strcat(whichCommand, " > path.txt");
	system(whichCommand);

	FILE *fd = fopen("path.txt", "r");
	char *buf = (char *)malloc(128 * sizeof(char));
	fscanf(fd, "%s", buf);
	fclose(fd);
	system("rm path.txt");
	return buf;
}
void formatFilePath(char* path){
	const char s[2] = " ";
	char result[1024];
	char* token;

	memset(result, 0, sizeof(result));
	//strcat(result, "~");
	token = strtok(path, s);

	while(token != NULL){
		strcat(result, token);
		token = strtok(NULL, s);
		if(token == NULL) break;
		strcat(result, "\\ ");
	}
	printf("%s", result);
	memset(path, 0, sizeof(path));
	memcpy(path, result, sizeof(result));
}
