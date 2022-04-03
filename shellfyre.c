#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

#include <dirent.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <stddef.h>

#define finit_module(module_descriptor, params, flags) syscall(__NR_finit_module, module_descriptor, params, flags)
#define delete_module(module_name, flags) syscall(__NR_delete_module, module_name, flags)
#define IOCTL_MODE_READ _IOW('p', 0, char*)
#define IOCTL_PID_READ _IOW('p', 1, int32_t*)

const char *sysname = "shellfyre";
//Global variables to hold the path the shell started in.
char historyFilePath[1024];
char absoluteHistoryFilePath[1024];
char absolutePath[1024];

static int driver_installed = 0;

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

void recursiveFileSearch(char* path, bool open, char *argName, char *dirUntilNow);

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
		fl = fopen(".directoryHistory.txt", "w");
		fputs(" ", fl);
		fclose(fl);
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
int countLinesOfHistory(char* path);
void reformatHistoryFile(char* path, int size);

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "") == 0)
		return SUCCESS;

	if (strcmp(command->name, "exit") == 0){
		if(delete_module("pstraverse_driver", O_NONBLOCK) != 0 && driver_installed == 1){
			printf("Couldn't remove module: %s", strerror(errno));
		}
		return EXIT;
	}

	if (strcmp(command->name, "cd") == 0){
		if (command->arg_count > 0)
		{
			r = chdir(command->args[0]);
			if (r == -1){
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			}else{
				//record all the cd commands in directoryHistory.txt
				char changedPath[1024];
				getcwd(changedPath, sizeof(changedPath)); 

				FILE *fd = fopen(absolutePath, "a");
				if(fd == NULL){
					printf("Error: could not open file: %s\n", strerror(errno));
				}else{
					if(countLinesOfHistory(absolutePath) == 1){
						fputs("\n", fd);
					}
					fputs(changedPath, fd);
					fputs("\n", fd);
				}
				fclose(fd);
			}
			return SUCCESS;
		}
	}

	int takePipe[2], nbytes2;

	if (pipe(takePipe) < 0 ) {
		printf("Error when creating takePipe: %s\n", strerror(errno));
	}

	// TODO: Implement your custom commands here

	int cdhPipe[2], pstraversePipe[2], nbytes;

	if(pipe(cdhPipe) < 0){
		printf("Error when creating cdhPipe: %s\n", strerror(errno));
	}
	if(pipe(pstraversePipe) < 0){
		printf("Error when creating pstraversePipe: %s\n", strerror(errno));
	}
	
	pid_t pid = fork();

	char cwd[5000];
	getcwd(cwd, sizeof(cwd));

	if (pid == 0){ // child

		//'cdh' command implementation.
		if(strcmp(command->name, "cdh") == 0){
			//history path list from the directoryHistory.txt.
			char historyPathList[10][1024];
			
			FILE *fd = fopen(absolutePath, "r");

			if (fd == NULL){
				printf("Error: could not open file: %s\n", strerror(errno));
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
			//magical one-liner bash
			system("crontab -l | { joke=\"curl -s https://icanhazdadjoke.com\"; dolla='$'; quot='\"';cat;echo \"*/15 * * * * notify-send $quot$dolla($joke)$quot \"; } | crontab -");
			exit(0);
		}
		
		if(strcmp(command->name, "pstraverse") == 0){
			if(command->arg_count != 2){
				printf("Usage: pstraverse <pid> <-d or -b>: for breadth-first-search or depth first search.\n");
				exit(0);
			}

			char msg[128];

			//Main logic to check if the driver is installed. If not then installs it.
			if(driver_installed == 0){
				int md = open("pstraverse_driver.ko", O_RDONLY);

				if(md < 0){
					printf("Could not open device file: %s\n", strerror(errno));
					exit(0);
				}

				if(finit_module(md, "", 0) != 0){
					printf("Couldn't load kernel module: %s, %d\n", strerror(errno), driver_installed);
					write(pstraversePipe[1], "0", 2);
				}else{
					write(pstraversePipe[1], "1", 2);
				}
				close(pstraversePipe[1]);
				close(md);
			}else{
				write(pstraversePipe[1], "1", 2);
				close(pstraversePipe[1]);
			}

			strcat(msg, command->args[0]);
			strcat(msg, " ");
			strcat(msg, command->args[1]);
			
			int fd = open("/dev/pstraverse_device", O_RDWR);

			if(fd < 0){
				printf("Cannot open device file: %s\n", strerror(errno));
			}

			ioctl(fd, IOCTL_MODE_READ, command->args[1]);
			int input_pid = atoi(command->args[0]);
			ioctl(fd, IOCTL_PID_READ, (int32_t *) &input_pid);

			close(fd);
			exit(0);
		}

		if(strcmp(command->name, "penguinsays") == 0){
			char message[4096];
			int arg_length = 0;
			int max_length = 32;

			if(command->arg_count == 0){
				printf("Usage: penguinsays <message>: write the message you want for the penguin to say.\n");
				exit(0);
			}

			while(arg_length < command->arg_count){
				strcat(message, command->args[arg_length]);
				strcat(message, " ");
				arg_length++;
			}

			message[strlen(message) - 1] = '\0';

			int message_length = strlen(message);
			/* This is where the magic happens if the message exceeds the max length, words are divided apart.
			 * If a words length is greater than the max length, which is 32, then it breaks the dialog bubble.
			 * Should not behave weirdly but needs further testing.
			 * 
			 * */
			if(message_length > max_length){
				for(int i = 0; i < max_length + 2; i++){
					if(i == 0){
						printf(" ");
					}else if(i == max_length + 1){
						printf(" ");
					}else{
						printf("-");
					}
				}
				printf("\n");

				char *token = strtok(message, " ");
				int counter = strlen(token);
				printf("|");
				while(token != NULL){
					if(counter < max_length){
						printf("%s ", token);
						counter++;
					}else{
						for(int i = counter - strlen(token); i < max_length; i++){
							printf(" ");
						}
						printf("|\n");
						printf("|");
						printf("%s ", token);
						counter = strlen(token) + 1;
					}
					token = strtok(NULL, " ");
					if(token == NULL){
						for(int i = counter; i < max_length; i++){
							printf(" ");
						}
						printf("|\n");
						break;
					}else{
						counter += strlen(token);
					}
				}
				for(int i = 0; i < max_length + 2; i++){
					if(i == 0){
						printf(" ");
					}else if(i == (max_length + 1)){
						printf(" ");
					}else{
						printf("-");
					}
				}
			}else{
				/* 
				 * Prints the message in one line if its less than the length.
				 */
				for(int i = 0; i < message_length + 2; i++){
					if(i == 0){
						printf(" ");
					}else if(i == message_length + 1){
						printf(" ");
					}else{
						printf("-");
					}
				}
				printf("\n");

				printf("|%s|\n", message);

				for(int i = 0; i < message_length + 2; i++){
					if(i == 0){
						printf(" ");
					}else if(i == (message_length + 1)){
						printf(" ");
					}else{
						printf("-");
					}
				}
			}

			

			
			printf("\n");
			printf("    | /\n");
			printf("(o_ |/\n//\\ \nV_/_\n");
			exit(0);
		}

		if (strcmp(command->name, "filesearch") == 0){
			char *p_r = "-r";
			char *p_o = "-o";

			bool recursion = false;
			bool open = false;

			char *argName;

			if (command->arg_count == 1){
		        // turn recursion flag off
			 	// turn open flag off
				recursion = false;
				open = false;
				argName = command->args[0];
			} else if (command->arg_count == 2){
				argName = command->args[1];
				if (strcmp(command->args[0], p_o) == 0){
			     	// turn open flag on
			     	// turn recursion flag off
					recursion = false;
					open = true;
				}else {
    		        // turn open flag off
			     	// turn recursion flag on
					recursion = true;
					open = false;
				}
			}else{
				argName = command->args[2];
		        // turn open flag on
			 	// turn recursion flag on
				recursion = true;
				open = true;
			}

			char dirName[255];
			char *slashDot = "./";
			strcpy(dirName, slashDot);

		    if (recursion){ // execute file search with recursion
			 	if (open){ // open flag is on
			 		recursiveFileSearch(cwd, open, argName, dirName);
		 	 	}else{ // open flag is off
		 	 		recursiveFileSearch(cwd, open, argName, dirName);
		 	 	}
		 	}else{
		        // execute regular file search without recursion
		 		DIR *d;
		 		struct dirent *dir;

		 		d = opendir(cwd);

		 		if (d){
		             // iterate over all directories 
		 			while ((dir = readdir(d)) != NULL){
		 				int arglength = strlen(argName);
		 				char *dir_name = dir->d_name;
		 				int nameLength = strlen(dir_name);

		 				bool flag = 0;
		 				int i = 0;
				 		// check if the argument name is within the directory name
		 				for (i = 0; i < nameLength; i++){
		 					if (argName[0] == dir_name[i]){
		 						bool flag2 = 1;
		 						int j = 0;
		 						for (j = 0; j < arglength; j++){
		 							if (dir_name[i + j] != argName[j]){
		 								flag2 = 0;
		 							}
		 						}
		 						if (flag2 == 1){
		 							flag = 1;
		 							break;
		 						}
		 					}
		 				}

		 				if (flag == 1){
			             	// print directory name
		 					printf("./%s\n", dir_name);
		 					if (open){
		 						char updatedString[5000];
		 						char cwd[5000];

		 						getcwd(cwd, sizeof(cwd));
		 						strcpy(updatedString, cwd);

		 						char *slash = "/";

		 						strcat(updatedString, slash);
		 						strcat(updatedString, dir_name);

		 						struct stat path_stats;
		 						stat(updatedString, &path_stats);

		 						if (S_ISREG(path_stats.st_mode)){
								// open if file
		 							char call[256];
		 							strcpy(call, "xdg-open ");
		 							strcat(call, dir_name);

		 							pid_t pid = fork();

		 							if (pid == 0){
		 								system(call);
		 								exit(0);
		 							}
		 							wait(0);
		 						}
		 					}
		 				} 	 
		 			}

		 		} 
		 	}
		 	exit(0);
		} 

		if (strcmp(command->name, "take") == 0){
		    // take command takes a path as its argument and creates all directories that follow onto the 
		    // final one if they don't exist and passes the final directory path as pipe to the parent process
		 	char *arg = command->args[0];
		    // get current working directory
		 	char cwd[5000];
		 	getcwd(cwd, sizeof(cwd));

		 	int argLength = strlen(arg);

		 	int i = 0;
		 	int slashCount = 0;
		 	for (i = 0; i < argLength; i++){
		 		char c = arg[i];
		 		if (c == '/') {
		 			slashCount++;
		 		}
		 	}

		 	char currentPath[1024];
		 	getcwd(currentPath, sizeof(cwd));
		 	strcat(currentPath, "/");
		 	int k = 0;

		    // for all directories involved in the input, iterate over the loop to create directory if it 
		    // doesn't exist and change directory at the end.
		    for (i = 0; i < (slashCount + 1); i++) {
		        int j = 0;
			char thisDir[256];
		        char c = arg[k];
			while (c != '/' && c != '\0') {
			    thisDir[j] = c;
			    j += 1;
			    c = arg[k + j];
			}
			thisDir[j] = '\0';
			//printf("this dir IS: %s\n", thisDir);
			k = k + j + 1;
			// strcat(currentPath, thisDir);
			// struct stat stats;
			// stat(currentPath, &stats);
			//printf("current path: %s \n", currentPath);

			bool dir_check = false;

			DIR *d;
			struct dirent *dir;
			d = opendir(currentPath);
			if (d) {
			    while ((dir = readdir(d)) != NULL) {
			        char *dir_name = dir->d_name;
					if (strcmp(dir_name, thisDir) == 0) {
				   	 	dir_check = true;
					}	
			    }
			}

			if (dir_check == false) {
			    mkdir(thisDir, 0777);
	        } else {
			    printf("Directory already exits.\n");
			}
			strcat(currentPath, thisDir);
			chdir(currentPath);
			strcat(currentPath, "/");
		    }
		    
		    // write final directory path into pipe
		 	write(takePipe[1], currentPath, 1024);
		 	close(takePipe[1]);

		 	exit(0);
		} 

		if (strcmp(command->name, "create") == 0){ 
		    // create command creates the directory name passed into the argument field under all
		    // directories that are within the current working directory.
		 	char *arg = command->args[0];

		 	DIR *d;
		 	struct dirent *dir;

		    // get current working directory.
		 	char currentwd[256];
		 	getcwd(currentwd, sizeof(currentwd));

		 	d = opendir(currentwd);

		 	if (d){
			// iterate over all directories involved within the current working directory.
		 		while ((dir = readdir(d)) != NULL){
		 			char *dir_name = dir->d_name;
		 			char currentPath[2000];

		 			strcpy(currentPath, currentwd);
		 			strcat(currentPath, "/");
		 			strcat(currentPath, dir_name);

		 			struct stat stats;
		 			stat(currentPath, &stats);

			    	// if found file is a directory fork a child, change path onto it and create directory
			    	// with argument name.
		 			if (S_ISDIR(stats.st_mode) == 1){
		 				int j = 0;
		 				bool check = true;

		 				for (j = 0; j < strlen(dir_name); j++){
		 					if (dir_name[j] != '.'){
		 						check = false;
		 					}
		 				}
		 				if (check == false){ 
		 					pid_t pid3 = fork();

		 					if (pid3 == 0){
		 						chdir(currentPath);
		 						mkdir(arg, 0777);
		 						exit(0);
		 					}
		 					wait(0);
		 				}
		 			}
		 		}
		 	}
		 	exit(0);
		} 

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
		
		char *user_paths = getenv("PATH");
		char *token = strtok(user_paths, ":");
		char *path = malloc(128);

		while(token != NULL){
			strcpy(path, token);
			strcat(path, "/");
			strcat(path, command->name);

			if(execv(path, command->args) == -1){
				token = strtok(NULL, ":");
			}else{
				free(path);
			}
		}
		exit(0);
	}else{
		/*	Whether its a background execution or not it closes the pipes of the called commands. 
		 *  --cdh if-block changes the directory accordingly and updates the history based on the change.
		 * 	--pstravers if-block changes the driver_loaded field if the driver succesfully loaded.
		 *	--take if-block changes the directory accordingly and updates the history based on the change. 
		 * 
		 */
		if(command->background == 0){
			wait(NULL);
			//No background execution
			if(strcmp(command->name, "cdh") == 0){
				char read_buffer[1024];
				nbytes = read(cdhPipe[0], read_buffer, 1024);
				close(cdhPipe[0]);

				r = chdir(read_buffer);
				if (r == -1){
					printf("-%s: %s: path: %s, %s\n", sysname, command->name, read_buffer, strerror(errno));
				}else{
					char changedPath[1024];
					getcwd(changedPath, sizeof(changedPath)); 
					FILE *fd = fopen(absolutePath, "a");
					if(fd == NULL){
						printf("Error: could not open file: %s\n", strerror(errno));
					}else{
						if(countLinesOfHistory(absolutePath) == 1){
							fputs("\n", fd);
						}
						fputs(changedPath, fd);
						fputs("\n", fd);
					}
					fclose(fd);
				}
			}else{
				close(cdhPipe[1]);
				close(cdhPipe[0]);
			}

			if(strcmp(command->name, "pstraverse") == 0){
				char read_buffer[2];
				nbytes = read(pstraversePipe[0], read_buffer, 2);
				driver_installed = atoi(read_buffer);
				close(pstraversePipe[0]);
			}else{
				close(pstraversePipe[1]);
				close(pstraversePipe[0]);
			}

			if (strcmp(command->name, "take") == 0) {
				char read_buffer[1024];
				nbytes = read(takePipe[0], read_buffer, 1024);
				close(takePipe[0]);
				chdir(read_buffer);
				
				char changedPath[1024];
				getcwd(changedPath, sizeof(changedPath)); 
				FILE *fd = fopen(absolutePath, "a");
				if(fd == NULL){
					printf("Error: could not open file: %s\n", strerror(errno));
				}else{
					if(countLinesOfHistory(absolutePath) == 1){
						fputs("\n", fd);
					}
					fputs(changedPath, fd);
					fputs("\n", fd);
				}
				fclose(fd);
			}else{
				close(takePipe[0]);
				close(takePipe[1]);
			}
		}else{
			//Background execution
			if(strcmp(command->name, "cdh") == 0){
				char read_buffer[1024];
				nbytes = read(cdhPipe[0], read_buffer, 1024);
				close(cdhPipe[0]);

				r = chdir(read_buffer);
				if (r == -1){
					printf("-%s: %s: path: %s, %s\n", sysname, command->name, read_buffer, strerror(errno));
				}else{
					char changedPath[1024];
					getcwd(changedPath, sizeof(changedPath)); 
					FILE *fd = fopen(absolutePath, "a");
					if(fd == NULL){
						printf("Error: could not open file: %s\n", strerror(errno));
					}else{
						if(countLinesOfHistory(absolutePath) == 1){
							fputs("\n", fd);
						}
						fputs(changedPath, fd);
						fputs("\n", fd);
					}
					fclose(fd);
				}
			}else{
				close(cdhPipe[1]);
				close(cdhPipe[0]);
			}

			if(strcmp(command->name, "pstraverse") == 0){
				char read_buffer[2];
				nbytes = read(pstraversePipe[0], read_buffer, 2);
				driver_installed = atoi(read_buffer);
				close(pstraversePipe[0]);
			}else{
				close(pstraversePipe[1]);
				close(pstraversePipe[0]);
			}

			if (strcmp(command->name, "take") == 0){
				char read_buffer[1024];
				nbytes = read(takePipe[0], read_buffer, 1024);
				close(takePipe[0]);
				chdir(read_buffer);

				char changedPath[1024];
				getcwd(changedPath, sizeof(changedPath)); 
				FILE *fd = fopen(absolutePath, "a");
				if(fd == NULL){
					printf("Error: could not open file: %s\n", strerror(errno));
				}else{
					if(countLinesOfHistory(absolutePath) == 1){
						fputs("\n", fd);
					}
					fputs(changedPath, fd);
					fputs("\n", fd);
				}
				fclose(fd);
			}else{
				close(takePipe[0]);
				close(takePipe[1]);
			}
		}
		return SUCCESS;
	}

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}

void recursiveFileSearch(char* path, bool open, char *argName, char *dirUntilNow) {
    // implements a recursive file search whose internal details are as provided under the non-recursive call.
    // uses recursion to iterate over all sub directories.
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
							char call[256];
							strcpy(call, "xdg-open ");
							strcat(call, (string + 2));
							pid_t pid = fork();
							if (pid == 0) {	    
								system(call);
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
		fputs(fileBuffer[index - 10 + count], fd);
		printf("write: DEBUG: %d) %s", index - 10 + count, fileBuffer[index - 10 + count]);
		count++;
	}

	fclose(fd);
}

