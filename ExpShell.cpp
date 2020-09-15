// ==========================
// ExpShell.cpp
// A simple shell for Linux.
// by z0gSh1u @ 2020-09
// ==========================

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <iostream>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace std;

const string WHITE_SPACE = " \t\r\n";
const string SYMBOL = "|<>";

#define MAX_ARGV_LEN 128

// OFLAG for file open
#define REDIR_IN_OFLAG O_RDONLY
#define REDIR_OUT_OFLAG O_WRONLY | O_CREAT | O_TRUNC

// fd
int pipe_fd[2]; // r/w pipe file descriptor
int redir_in_fd;
int redir_out_fd;

// this buffer is used for C-style functions to get a string
#define CHAR_BUF_SIZE 1024
char char_buf[CHAR_BUF_SIZE];

// ==========================
// string utilities
// ==========================
vector<string> string_split(const string &s, const string &delims) {
  vector<string> vec;
  int p = s.find_first_of(delims), q = 0;
  while (q != string::npos) {
    if (q != p)
      vec.push_back(s.substr(p, q - p));
    p = q + 1;
    q = s.find_first_of(delims, p);
  }
  if (p != s.length())
    vec.push_back(s.substr(p));
  return vec;
}

string string_split_last(const string &s, const string &delims) {
  vector<string> split_res = string_split(s, delims);
  return split_res.at(split_res.size() - 1);
}

string string_split_first(const string &s, const string &delims) {
  vector<string> split_res = string_split(s, delims);
  return split_res.at(0);
}

string trim(const string &s) {
  if (s.length() == 0)
    return string(s);
  int p = 0, q = s.length() - 1;
  while (is_white_space(s[p++]))
    ;
  while (is_white_space(s[q--]))
    ;
  return s.substr(p, q - p);
}

string join(vector<string> &vec, const string &with = " ") {
  string res = "";
  for (vector<string>::iterator it = vec.begin(); it < vec.end(); it++) {
    res += (*it);
    if (it != vec.end() - 1)
      res += with;
  }
  return res;
}

string read_line() {
  string line;
  getline(cin, line);
  return line;
}

bool is_white_space(char ch) { return WHITE_SPACE.find(ch) != -1; }

bool is_symbol(char ch) { return SYMBOL.find(ch) != -1; }

// FIXME: unstable
char **vector_string_to_char_star_array(vector<string> &vec) {
  char *res[vec.size()];
  for (int i = 0; i < vec.size(); i++) {
    res[i] = (char *)malloc(MAX_ARGV_LEN * sizeof(char));
    // TODO: free it?
    strcpy(res[i], vec.at(i).c_str());
  }
  return res;
}

// ==========================
// show the command prompt in front of each line
// **example** [root@localhost tmp]>
// ==========================
void show_command_prompt() {
  // get username
  passwd *pwd = getpwuid(getuid());
  string username(pwd->pw_name);
  // get current working directory
  getcwd(char_buf, CHAR_BUF_SIZE);
  string cwd(char_buf);
  // consider home path (~)
  if ((username == "root" && cwd == "/root") /* home for root */ ||
      cwd == "/home/" + username /* home for other user*/)
    cwd = "~";
  else if (cwd != "/") {
    // consider root path (/)
    // keep only the last level of directory
    cwd = string_split_last(cwd, "/");
  }
  // get hostname
  gethostname(char_buf, CHAR_BUF_SIZE);
  string hostname(char_buf);
  // sometimes, hostname is like localhost.locald.xxx here, should split it
  hostname = string_split_first(hostname, ".");
  // output
  cout << "[" << username << "@" << hostname << " " << cwd << "]> ";
}

// ==========================
// proxy functions
// ==========================
void panic(string hint, bool exit_ = false, int exit_code = 0) {
  cerr << "[ExpShell panic]: " << hint << endl;
  if (exit_)
    exit(exit_code);
}

// wrapped fork function that panics
int fork_wrap() {
  int pid = fork();
  if (pid == -1)
    panic("fork failed.", true, 1);
  return pid;
}

// wrapped pipe function that panics
int pipe_wrap(int pipe_fd[2]) {
  int ret = pipe(pipe_fd);
  if (ret == -1)
    panic("pipe failed", true, 1);
  return ret;
}

// ==========================
// command line parsing
// ==========================
#define CMD_TYPE_NULL 0      // initial value
#define CMD_TYPE_EXEC 1      // common exec command
#define CMD_TYPE_PIPE 2      // pipe command
#define CMD_TYPE_REDIR_IN 4  // redirect using <
#define CMD_TYPE_REDIR_OUT 8 // redirect using >

// base class for any cmd
class cmd {
public:
  int type;
  cmd() { this->type = CMD_TYPE_NULL; }
};

// most common type of cmd
// argv[0] ...argv[1~n]
class exec_cmd : public cmd {
public:
  vector<string> argv;
  exec_cmd(vector<string> &argv) {
    this->type = CMD_TYPE_EXEC;
    this->argv = vector<string>(argv);
  }
};

// pipe cmd
// left | right
class pipe_cmd : public cmd {
public:
  cmd *left;
  cmd *right;
  pipe_cmd() { this->type = CMD_TYPE_PIPE; }
  pipe_cmd(cmd *left, cmd *right) {
    this->type = CMD_TYPE_PIPE;
    this->left = left;
    this->right = right;
  }
};

// redirect cmd
// ls > a.txt; some_program < b.txt
class redirect_cmd : public cmd {
public:
  cmd *cmd_;
  string file;
  int fd;
  redirect_cmd() {}
  redirect_cmd(int type, cmd *cmd_, string file, int fd) {
    this->type = type;
    this->cmd_ = cmd_;
    this->file = file;
    this->fd = fd;
  }
};

// divide-and-conquer
// ** test cases: **
// ls -a < a.txt | grep linux > b.txt
// some_bin "hello world" > b.txt > c.txt
cmd *parse(string line) {
  line = trim(line);
  string cur_read = "";
  cmd *cur_cmd = new cmd();
  int i = 0;
  while (i < line.length()) {
    if (line[i] == '<' || line[i] == '>') {
      cmd *lhs = parse_exec_cmd(cur_read); // [lhs] < (or >) [rhs]
      int j = i + 1;
      while (j < line.length() && !is_symbol(line[j]))
        j++;
      string file = trim(line.substr(i, j - i));
      cur_cmd = new redirect_cmd(line[i] == '<' ? CMD_TYPE_REDIR_IN
                                                : CMD_TYPE_REDIR_OUT,
                                 lhs, file, -1);
      i = j;
    } else if (line[i] == '|') {
      cmd *rhs = parse(line.substr(i + 1));
      if (cur_cmd->type == CMD_TYPE_NULL)
        cur_cmd = parse_exec_cmd(cur_read);
      cur_cmd = new pipe_cmd(cur_cmd, rhs);
    } else
      cur_read += line[i++];
  }
  if (cur_cmd->type == CMD_TYPE_NULL)
    return parse_exec_cmd(cur_read);
  else
    return cur_cmd;
}

// parse seg as is exec_cmd
cmd *parse_exec_cmd(string seg) {
  seg = trim(seg);
  vector<string> argv = string_split(seg, WHITE_SPACE);
  return new exec_cmd(argv);
}

// deal with builtin command
// returns: 0-nothing_done, 1-success, -1-failure
int process_builtin_command(const string &line) {
  // 1 - cd
  if (line.substr(0, 2) == "cd") {
    int chdir_ret = chdir(trim(line.substr(2)).c_str());
    if (chdir_ret < 0) {
      panic("chdir failed");
      return -1;
    } else
      return 1; // successfully processed
  }
  // 2 - quit
  if (line.substr(0, 4) == "quit")
    exit(0);
  return 0; // nothing done
}

// TODO:
void run_cmd(cmd *cmd_) {
  switch (cmd_->type) {
  case CMD_TYPE_EXEC:
    exec_cmd *ecmd = static_cast<exec_cmd *>(cmd_);
    break;
  case CMD_TYPE_PIPE:
    pipe_cmd *pcmd = static_cast<pipe_cmd *>(cmd_);
    break;
  case CMD_TYPE_REDIR_IN:
  case CMD_TYPE_REDIR_OUT:
    redirect_cmd *rcmd = static_cast<redirect_cmd *>(cmd_);
    break;
  default:
    panic("unknown or null cmd type");
  }
}

// entry method of the shell
int main() {
  system("stty erase ^H"); // fix ^H when using backspace on SSH
  string line;
  int wait_status;
  while (true) {
    show_command_prompt();
    line = trim(read_line());
    // deal with builtin commands
    int builtin_ret;
    builtin_ret = process_builtin_command(line);
    if (builtin_ret > 0)
      continue;
    // TODO: fork to execute the typed command
    wait(&wait_status);
    // deal with abnormal exit of child
    if (WIFEXITED(wait_status) != 0)
      panic("child exit with code " + to_string(WEXITSTATUS(wait_status)));
  }
  return 0;
}