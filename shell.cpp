#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include "Tokenizer.h"

// all the basic colours for a shell prompt
#define RED     "\033[1;31m"
#define GREEN	"\033[1;32m"
#define YELLOW  "\033[1;33m"
#define BLUE	"\033[1;34m"
#define WHITE	"\033[1;37m"
#define NC      "\033[0m"

using namespace std;

string prev_dir = "";
vector<pid_t> bg_pids;

// function to reap background processess
void reap_bg_processess() {
    for (auto it = bg_pids.begin(); it != bg_pids.end();) {
        int status;
        pid_t result = waitpid(*it, &status, WNOHANG);
        if (result > 0) {
            // removes process once finished
            it = bg_pids.erase(it);
        } else {
            it++;
        }
    }
}

// function to handle cd commands
bool handle_cd(const vector<string> &args) {
    if (args.size() > 2) {
        cerr << "cd: too many arguments" << endl;
        return false;
    }

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        perror("getcwd");
        return false;
    }

    string curr_dir = cwd;

    // if just cd or cd ~, goes back to home
    if (args.size() == 1 || args[1] == "~") {
        const char* home = getenv("HOME");
        if (home == nullptr) {
            cerr << "cd: HOME not set" << endl;
            return false;
        }
        if (chdir(home) < 0) {
            perror("chdir");
            return false;
        }
        // update prev directory
        prev_dir = curr_dir;
    } 
    // if cd -, goes back to prev directory
    else if (args[1] == "-") {
        if (prev_dir.empty()) {
            cerr << "cd: OLDPWD not set" << endl;
            return false;
        }
        if (chdir(prev_dir.c_str()) < 0) {
            perror("chdir");
            return false;
        }
        string temp = prev_dir;
        prev_dir = curr_dir;
    } 
    // goes to specified directory
    else {
        if (chdir(args[1].c_str()) < 0) {
            perror("chdir");
            return false;
        }
        prev_dir = curr_dir;
    }
    return true;
}

// convert vector string to char** for execvp
char** vec_to_char(const vector<string> &vec) {
    char** arr = new char*[vec.size() + 1];
    for (size_t i = 0; i < vec.size(); i++) {
        arr[i] = (char*)vec[i].c_str();
    }
    arr[vec.size()] = nullptr;
    return arr;
}

// executes single command
void exec_single_command(Command* cmd, bool is_background) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }

    if (pid == 0) {
        // handle input
        if (cmd->hasInput()) {
            int fd_in = open(cmd->in_file.c_str(), O_RDONLY);
            if (fd_in < 0) {
                perror("open input file");
                exit(2);
            }
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }
        // handle output
        if (cmd->hasOutput()) {
            int fd_out = open(cmd->out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_out < 0) {
                perror("open output file");
                exit(2);
            }
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }

        // execute command
        char** args = vec_to_char(cmd->args);
        if (execvp(args[0], args) < 0) {
            perror("execvp");
            exit(2);
        }
    } else {
        if (is_background) {
            // add to background process list
            bg_pids.push_back(pid);
        } else {
            int status = 0;
            waitpid(pid, &status, 0);
            if (status > 1) {
                exit(status);
            }
        }
    }
}

// execute piped commands
void exec_piped_commands(vector<Command*> &commands) {
    int num_commands = commands.size();
    vector<pid_t> pids;

    // save original stdin
    int saved_stdin = dup(STDOUT_FILENO);
    int prev_pipe_read = -1;

    for (int i = 0; i < num_commands; i++) {
        int pipefd[2];

        // create pipe for all commands except last
        if (i < num_commands - 1) {
            if (pipe(pipefd) < 0) {
                perror("pipe");
                exit(2);
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(2);
        }

        if (pid == 0) {
            // handle input
            if (i == 0 && commands[i]->hasInput()) {
                int fd_in = open(commands[i]->in_file.c_str(), O_RDONLY);
                if (fd_in < 0) {
                    perror("open input file");
                    exit(2);
                }
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            } else if (prev_pipe_read != -1) {
                dup2(prev_pipe_read, STDIN_FILENO);
            }

            if (prev_pipe_read != -1) {
                close(prev_pipe_read);
            }

            // handle output
            if (i == num_commands - 1) {
                if (commands[i]->hasOutput()) {   
                    int fd_out = open(commands[i]->out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

                    if (fd_out < 0) {
                        perror("open output file");
                        exit(2);
                    }
                    dup2(fd_out, STDOUT_FILENO);
                    close(fd_out);
                }
            } else {
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
                close(pipefd[0]);
            }

            // execute command
            char** args = vec_to_char(commands[i]->args);
            if (execvp(args[0], args) < 0) {
                perror("execvp");
                exit(2);
            }
        } else {
            pids.push_back(pid);

            if (prev_pipe_read != -1) {
                close(prev_pipe_read);
            }

            if (i < num_commands - 1) {
                close(pipefd[1]);
                prev_pipe_read = pipefd[0];
            }
        }
    }

    // restore stdin
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);

    for (pid_t pid : pids) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

int main () {
    // save original stdin at start
    int original_stdin = dup(STDIN_FILENO);

    for (;;) {
        // reap any background processess and restore stdin
        reap_bg_processess();
        dup2(original_stdin, STDIN_FILENO);

        // get current time
        time_t now = time(nullptr);
        char time_str[100];
        strftime(time_str, sizeof(time_str), "%b %d %H:%M:%S", localtime(&now));
        
        // get username or set to root if none
        const char* username = getenv("USER");
        if (username == nullptr) {
            username = "root";
        }

        // get current directory
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) {
            perror("getcwd");
            strcpy(cwd, "unknown");
        }

        // outputs prompt
        cout << username << " " << time_str << ":" << cwd << "$ ";
        cout.flush();
        
        // get user input command
        string input;
        getline(cin, input);

        if (input == "exit") {
            cout << RED << "Now exiting shell..." << endl << "Goodbye" << NC << endl;
            break;
        }

        if (input.empty()) {
            continue;
        }

        // get tokenized commands
        Tokenizer tknr(input);
        if (tknr.hasError()) {
            continue;
        }

        // check if cd command
        if (tknr.commands.size() == 1 &&
                tknr.commands[0]->args.size() > 0 &&
                tknr.commands[0]->args[0] == "cd") {
            handle_cd(tknr.commands[0]->args);
            continue;
        }

        // check if background process
        bool is_background = false;
        if (tknr.commands.size() > 0) {
            is_background = tknr.commands.back()->isBackground();
        }

        // execute commands
        if (tknr.commands.size() == 1) {
            exec_single_command(tknr.commands[0], is_background);
        } else {
            if (is_background) {
                pid_t pid = fork();
                if (pid < 0) {
                    perror("fork");
                    exit(2);
                }
                if (pid == 0) {
                    exec_piped_commands(tknr.commands);
                    exit(0);
                } else {
                    bg_pids.push_back(pid);
                }
            } else {
                exec_piped_commands(tknr.commands);
            }
        }
    }
    // close and exit
    close(original_stdin);
    return 0;
}
