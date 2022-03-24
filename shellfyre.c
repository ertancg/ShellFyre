#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>

const char *sysname = "shellfyre";
//Global variables to hold the path the shell started in.
char historyFilePath[1024];
char absoluteHistoryFilePath[1024];
char absolutePath[1024];

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

	//print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);

//Helper to parse the file path. Adds escape characters to the file path.
void formatFilePath(char* path);

int main()
{
	getcwd(historyFilePath, sizeof(historyFilePath));
	//Getting current directory that shell started and parsing the file path to handle escape characters.
	strcat(historyFilePath, "/.directoryHistory.txt");
	memcpy(absoluteHistoryFilePath, historyFilePath, sizeof(historyFilePath));
	formatFilePath(historyFilePath);

	//opening directoryHistory.txt.
	FILE *fl = fopen(".directoryHistory.txt", "r");

	//if file doesn't exits it creates it.
	if(fl == NULL){
		system("echo >> .directoryHistory.txt");
	}else{
		fclose(fl);
	}

	//canonicalized absolute pathname of the file, without the formats.
	char *ptr = realpath(absoluteHistoryFilePath, absolutePath);

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

//Helper functions for cdh command.
char* getFilePath(char* cmd);
int countLinesOfHistory(char* path);
void reformatHistoryFile(char* path, int size);

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "") == 0)
		return SUCCESS;

	if (strcmp(command->name, "exit") == 0)
		return EXIT;

	if (strcmp(command->name, "cd") == 0){
		if (command->arg_count > 0)
		{
			r = chdir(command->args[0]);
			if (r == -1){
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			}else{
				//record all the cd commands in directoryHistory.txt
				char cdHistoryCommand[128] = "echo ";
				char changedPath[1024];
				getcwd(changedPath, sizeof(changedPath)); 
				strcat(cdHistoryCommand, changedPath);
				strcat(cdHistoryCommand, " >> ");
				strcat(cdHistoryCommand, historyFilePath);
				system(cdHistoryCommand);
			}
			return SUCCESS;
		}
	}

	// TODO: Implement your custom commands here

	int cdhPipe[2], nbytes;

	if(pipe(cdhPipe) < 0){
		printf("Error when creating cdhPipe: %s", strerror(errno));
	}
	
	pid_t pid = fork();

	if (pid == 0){ // child

		//'cdh' command implementation.
		if(strcmp(command->name, "cdh") == 0){
			//history path list from the directoryHistory.txt.
			char historyPathList[10][1024];
			
			FILE *fd = fopen(absolutePath, "r");
			fd = fopen(absolutePath, "r");

			if (fd == NULL){
				printf("Error: could not open file.\n");
				write(cdhPipe[1], " ", 2);
				close(cdhPipe[1]);
			}else{

				//directoryHistory.txt buffer.
				char buffer[1024];

				//how many entries in the directoryHistory.txt
				int directoryIndex = 0;
				//user input to select which directory it wants to switch to. 
				char userDirectoryInput[128];
				//real index based on the user input.
				int indexOfInput;

				//line count of the directoryHistory.txt.
				int fileLength = countLinesOfHistory(absolutePath);

				//checks whether the file exceeds 10, if it is it reformats the directoryHistory.txt.
				if(fileLength > 10){
					reformatHistoryFile(absolutePath, fileLength);
				}

				//reading directoryHistory.txt
				while(fgets(buffer, 1024, fd) != NULL){
					memcpy(historyPathList[directoryIndex], buffer, sizeof(buffer));
					if(historyPathList[directoryIndex][strlen(buffer) - 1] == '\n'){
						historyPathList[directoryIndex][strlen(buffer) - 1] = '\0';
					}
					directoryIndex++;
				}

				fclose(fd);


				while(1){
					char letter = 'a';

					if(directoryIndex == 0) exit(0);

					//print all the entries in the historyPathList
					directoryIndex--;
					printf("%c %d) ~%s\n", letter + directoryIndex, directoryIndex + 1, historyPathList[directoryIndex]);

					//after all the entries printed user selection is required to switch the directory.
					if(directoryIndex == 0){
						printf("Select directory by letter or number: ");

						if(fgets(userDirectoryInput, 128, stdin) != NULL){

							//this if-else block checks the given input is integer or char.
							if(isdigit(userDirectoryInput[0]) > 0){
								indexOfInput = atoi(userDirectoryInput) - 1;
								if(indexOfInput > 10 || indexOfInput < 0 || indexOfInput > fileLength - 1){
									printf("Input cannot be larger than 10 or less than 0 or greater than the history list.\n");
									write(cdhPipe[1], " ", 2);
									close(cdhPipe[1]);
								}else{
									write(cdhPipe[1], historyPathList[indexOfInput], 1024);
									close(cdhPipe[1]);
								}
							}else{
								indexOfInput = userDirectoryInput[0] - letter;
								if(indexOfInput > 10 || indexOfInput > fileLength - 1){
									printf("Input cannot be larger than 10 or less than 0 or greater than the history list.\n");
									write(cdhPipe[1], " ", 2);
									close(cdhPipe[1]);
								}else{
									write(cdhPipe[1], historyPathList[indexOfInput], 1024);
									close(cdhPipe[1]);
								}
							}
						}
						break;
					} 
				}
			}
			exit(0);
		}

		if(strcmp(command->name, "joker") == 0){
			/*
			printf("\033[2J");        // clear the screen  
	
			printf("\033[H");         //  position cursor at top-left corner 
 
			for (int i=1; i<=10; i++){
    			printf("The current count is: %d", i);
    			fflush(stdout);
    			
    			printf(i < 10 ? "\033[H" : "\n");
			}
			fflush(stdout);*/
			exit(0);
		}
		
		// increase args size by 2
		command->args = (char **)realloc(
			command->args, sizeof(char *) * (command->arg_count += 2));

		// shift everything forward by 1
		for (int i = command->arg_count - 2; i > 0; --i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a copy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		/// TODO: do your own exec with path resolving using execv()
		char *path = getFilePath(command->name);
		printf("\n");
		execv(path, command->args);
		exit(0);
	}else{
		/// TODO: Wait for child to finish if command is not running in background

		if(command->background == 0){
			wait(NULL);
			if(strcmp(command->name, "cdh") == 0){
				char read_buffer[1024];
				nbytes = read(cdhPipe[0], read_buffer, 1024);
				close(cdhPipe[0]);

				r = chdir(read_buffer);
				if (r == -1){
					printf("-%s: %s: path: %s, %s\n", sysname, command->name, read_buffer, strerror(errno));
				}
			}else{
				close(cdhPipe[1]);
				close(cdhPipe[0]);
			}
		}else{
			if(strcmp(command->name, "cdh") == 0){
				char read_buffer[1024];
				nbytes = read(cdhPipe[0], read_buffer, 1024);
				close(cdhPipe[0]);

				r = chdir(read_buffer);
				if (r == -1){
					printf("-%s: %s: path: %s, %s\n", sysname, command->name, read_buffer, strerror(errno));
				}
			}else{
				close(cdhPipe[1]);
				close(cdhPipe[0]);
			}
		}
		return SUCCESS;
	}

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}
/**	
 *	Helper function for execv. Finds the path of the main unix commands by calling
 * 'which' command, which searches files in users paths.
 *	@param  cmd 	description: command name to search for in users paths.
 *	@return buf 	description: path of command that is searched.
 */
char* getFilePath(char* cmd){
	char whichCommand[128] = "which ";
	strcat(whichCommand, cmd);
	strcat(whichCommand, " > .path.txt");
	system(whichCommand);

	FILE *fd = fopen(".path.txt", "r");
	char *buf = (char *)malloc(128 * sizeof(char));
	fscanf(fd, "%s", buf);
	fclose(fd);
	system("rm .path.txt");
	return buf;
}
/** 
 *	This functions takes a path and formats it by removing spaces and adding 
 *  escape character '\'.
 *
 *	@param 	path 	description: path to be formatted
 */
void formatFilePath(char* path){
	char s[2] = " ";
	char result[1024];
	char* token;

	memset(result, 0, sizeof(result));
	
	token = strtok(path, s);

	while(token != NULL){
		strcat(result, token);
		token = strtok(NULL, s);
		if(token == NULL) break;
		strcat(result, "\\ ");
	}
	memset(path, 0, 1024);
	memcpy(path, result, sizeof(result));
}
/** 
 *	This functions takes a file path and returns the line count of the texts in it.
 *
 *	@param 	path 	description: path to be counted.
 *  @return count 	description: line count of the file.
 */
int countLinesOfHistory(char* path){
	FILE *fd = fopen(path, "r");
	int count = 0;
	char buffer[1024];
	while(fgets(buffer, 1024, fd) != NULL){
		count++;
	}
	fclose(fd);
	return count;
}
/** 
 *	This functions takes a file path, reads all the lines and overwrites it to have
 *  the last 10 lines of the file.
 *
 *	@param 	path 	description: path to be modified.
 *  @param 	size 	description: line count of the file
 */
void reformatHistoryFile(char *path, int size){
	char fileBuffer[size][1024];
	FILE *fd = fopen(path, "r");

	char buffer[1024];
	int index = 0;
	int count = 0;

	while(fgets(buffer, 1024, fd) != NULL){
		memcpy(fileBuffer[index], buffer, sizeof(buffer));
		index++;
	}

	fclose(fd);
	
	fd = fopen(path, "w");

	while(count < 10){
		fputs(fileBuffer[index-1], fd);
		index--;
		count++;
	}

	fclose(fd);
}

