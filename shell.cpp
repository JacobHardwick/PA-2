#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <ctime>
#include <cerrno>
#include "Tokenizer.h"

// Colors for prompt
#define RED     "\033[1;31m"
#define GREEN   "\033[1;32m"
#define YELLOW  "\033[1;33m"
#define BLUE    "\033[1;34m"
#define WHITE   "\033[1;37m"
#define NC      "\033[0m"

using namespace std;

string prev_dir = "";
vector<pid_t> bg_pids;

// reap background processes
void reap_bg_processes() {
    for (auto it = bg_pids.begin(); it != bg_pids.end();) {
        int status;
        pid_t result = waitpid(*it, &status, WNOHANG);
        if (result > 0 || (result == -1 && errno == ECHILD)) {
            it = bg_pids.erase(it);
        } else {
            ++it;
        }
    }
}

// handle cd command
bool handle_cd(const vector<string>& args) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return false;
    string curr_dir = cwd;

    if (args.size() == 1 || args[1] == "~") {
        const char* home = getenv("HOME");
        if (!home) return false;
        if (chdir(home) < 0) return false;
        prev_dir = curr_dir;
    } else if (args[1] == "-") {
        if (prev_dir.empty()) return false;
        if (chdir(prev_dir.c_str()) < 0) return false;
        prev_dir = curr_dir;
    } else {
        if (chdir(args[1].c_str()) < 0) return false;
        prev_dir = curr_dir;
    }
    return true;
}

// convert to char** for execvp
char** vec_to_char(const vector<string>& vec) {
    char** arr = new char*[vec.size() + 1];
    for (size_t i = 0; i < vec.size(); ++i) {
        arr[i] = (char*)vec[i].c_str();
    }
    arr[vec.size()] = nullptr;
    return arr;
}

// execute single command
void exec_single_command(Command* cmd, bool is_background) {
    pid_t pid = fork();
    if (pid < 0) { 
        perror("fork"); 
        exit(1); 
    }

    if (pid == 0) {
        // Input redirection
        if (cmd->hasInput()) {
            int fd_in = open(cmd->in_file.c_str(), O_RDONLY);
            if (fd_in < 0) { 
                perror("open input"); 
                exit(1); 
            }
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }
        // Output redirection
        if (cmd->hasOutput()) {
            int fd_out = open(cmd->out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_out < 0) { 
                perror("open output"); 
                exit(1); 
            }
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }
        char** argv = vec_to_char(cmd->args);
        execvp(argv[0], argv);
        perror(argv[0]);
        exit(1);
    } else {
        if (is_background) {
            bg_pids.push_back(pid);
        } else {
            int status;
            waitpid(pid, &status, 0);
        }
    }
}

// execute multiple piped commands
void exec_piped_commands(vector<Command*>& cmds) {
    int num_cmds = cmds.size();
    vector<int> pipefds(2 * (num_cmds - 1));

    // create all pipes
    for (int i = 0; i < num_cmds - 1; ++i) {
        if (pipe(pipefds.data() + i*2) < 0) { 
            perror("pipe"); 
            return;
        }
    }

    vector<pid_t> pids;
    bool is_background = cmds.back()->isBackground();

    for (int i = 0; i < num_cmds; ++i) {
        pid_t pid = fork();
        if (pid < 0) { 
            perror("fork"); 
            return; 
        }

        if (pid == 0) {
            // input from previous 
            if (i > 0 && !cmds[i]->hasInput()) {
                dup2(pipefds[(i-1)*2], STDIN_FILENO);
            }
            // output to next 
            if (i < num_cmds - 1 && !cmds[i]->hasOutput()) {
                dup2(pipefds[i*2 + 1], STDOUT_FILENO);
            }

            // input file redirection overrides pipe
            if (cmds[i]->hasInput()) {
                int fd_in = open(cmds[i]->in_file.c_str(), O_RDONLY);
                if (fd_in < 0) { 
                    perror("open input"); exit(1); 
                }
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            }
            // output file redirection overrides pipe
            if (cmds[i]->hasOutput()) {
                int fd_out = open(cmds[i]->out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd_out < 0) { 
                    perror("open output"); exit(1); 
                }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            }

            // close pipe fds in child
            for (int j = 0; j < 2*(num_cmds-1); ++j) {
                close(pipefds[j]);
            }

            char** argv = vec_to_char(cmds[i]->args);
            execvp(argv[0], argv);
            perror(argv[0]);
            exit(1);
        } else {
            pids.push_back(pid);
        }
    }

    // parent closes pipe fds
    for (int i = 0; i < 2*(num_cmds-1); ++i) {
        close(pipefds[i]);
    }
    if (is_background) {
        for (pid_t pid : pids) {
            bg_pids.push_back(pid);
        }
    } else {
        for (pid_t pid : pids) {
            waitpid(pid, nullptr, 0);
        }
    }
}

int main() {
    int original_stdin = dup(STDIN_FILENO);

    while (true) {
        reap_bg_processes();
        dup2(original_stdin, STDIN_FILENO);

        char cwd[1024]; 
        getcwd(cwd, sizeof(cwd));

        // get time
        time_t now = time(nullptr);
        char time_str[100]; 
        strftime(time_str, sizeof(time_str), "%b %d %H:%M:%S", localtime(&now));

        // get username or set to root if none
        const char* username = getenv("USER");
        if (username == nullptr) {
            username = "root";
        }

        // display prompt
        cout << username << " " << time_str << ":" << cwd << "$ ";
        cout.flush();

        // get user input
        string input;
        if (!getline(cin, input)) break;
        if (input.empty()) continue;

        if (input == "exit") { 
            cout << RED << "Now exiting shell...\nGoodbye" << NC << endl; 
            break; 
        }

        Tokenizer tknr(input);
        if (tknr.hasError()) continue;
        if (tknr.commands.empty()) continue;

        // handle cd
        if (tknr.commands.size() == 1 && !tknr.commands[0]->args.empty() && tknr.commands[0]->args[0] == "cd") {
            handle_cd(tknr.commands[0]->args);
            continue;
        }

        // execute commands
        if (tknr.commands.size() == 1) {
            exec_single_command(tknr.commands[0], tknr.commands[0]->isBackground());
        } else {
            exec_piped_commands(tknr.commands);
        }
    }

    close(original_stdin);
    return 0;
}
