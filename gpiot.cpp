#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <vector>
#include <string>
#include <regex>
#include <iostream>
#include <sstream>
#include <fstream>
#include <fcntl.h>

#define debug_printf(...) fprintf (stdout, __VA_ARGS__)
#define err_printf(...) fprintf(stderr, __VA_ARGS__)

struct sound_job {
  pid_t pid;
  pid_t child_pid;
  std::string file_name;
  int pipefd[2];
  int pin;
};

static int parse_sound_jobs(std::string &&config_file, std::vector<struct sound_job> &jobs)
{
  struct sound_job job = {0};
  job.child_pid = -1;

  std::ifstream input(config_file.c_str());
  if (!input.is_open()) {
    err_printf("Unable to open configuration file -> dying\n");
    exit (-1);
  }
  /* config file should be sth like this "pinX = fileY"*/
  std::regex pin_regex(R"B(pin(\d+)\s*=\s*([\w./]+))B");

  std::string line;
  int line_no = 1;
  while( std::getline( input, line ) ) {
    std::smatch match;
    std::regex_match(line, match, pin_regex);
    debug_printf("Parsing configuration file Line #%i:\t\t\"%s\"\n\t", line_no, line.c_str());
    if (match.size() == 3) {
      job.pin = atoi(match[1].str().c_str());
      job.file_name = match[2].str();
      jobs.push_back(job);
      debug_printf("\t Found \"Pin %i\" connection to Sound file \"%s\"\n", job.pin, job.file_name.c_str());
    } else {
      debug_printf("Error parsing configuration file:\n\t Line %s has invalid syntax\n", line.c_str());
      exit(-1);
    }
    ++line_no;
  }
}

static void do_listen_and_play(struct sound_job &job)
{
  char cmd;
  /* close the write side of the pipe because the child processes will only listen on commands from the parent */
  close(job.pipefd[1]);
  while (1) {
    pid_t this_pid = getpid();
    /* We will fork new mpg123 processes for each new sound job. We will kill these jobs if a new "play" or "stop" command
       arrives BEFORE the sound job has finished. If we do nothing here these mpg123 processes will be zombied because theire return value is not
       checked by the parent. To avoid this we will register a signal handler which will simply ignore the return value
       REF: https://www.win.tue.nl/~aeb/linux/lk/lk-5.html */
    signal(SIGCHLD, SIG_IGN);
    /* wait for the next command from the parent process */
    while(read(job.pipefd[0], &cmd, 1) > 0) {
      debug_printf("Read %c(%i)\n", cmd, this_pid);
      /* just "p" (play) and "s" (stop) are accepted*/
      if (cmd != 'p' && cmd != 's') {
        debug_printf("Invalid character from parent process received\n");
        continue;
      }
      if (job.child_pid != -1) {
        /* check if PID is still running. FIXME: this is not save if pids are reused by the kernel which seems not to happen in linux*/
        if (kill(job.child_pid, 0) == 0) {
          debug_printf("Child is still alive\n");
          /* send the SIGTERM signal to the mpg123 process */
          kill(job.child_pid, SIGTERM);
        }
      }
      if (cmd == 's')
        continue;
      /* we come here just in case of an p (play) command */
      pid_t cpid = fork();
      if (cpid < 0) {
        debug_printf("Could not fork child pid n");
      } else if (cpid == 0) {
        debug_printf("Start playing \"%s\"(%i)\n", job.file_name.c_str(), this_pid);
        execlp("mpg123", "mpg123", "-q", job.file_name.c_str(), NULL);
      } else {
        job.child_pid = cpid;
      }
    }
  }
}

int main(int argc, char **argv)
{
  std::vector<struct sound_job> all_jobs;
  parse_sound_jobs("config.cfg", all_jobs);
  debug_printf("Start forking child processes to execute sounds\n");
  for (struct sound_job &job: all_jobs) {
    if (pipe(job.pipefd) == -1) {
      perror("pipe");
      exit(EXIT_FAILURE);
    }
    pid_t child_pid = fork();
    if (child_pid < 0) {
      perror("Fork error");
    } else if (child_pid == 0) {
      do_listen_and_play(job);
    } else {
      job.pid = child_pid;
      debug_printf("Parent closing read side pipe for child process pid = %i (%s)\n", child_pid, job.file_name.c_str());
      close(job.pipefd[0]);
    }
  }
  // Here we are in the parent process an have to open all available gpio devices and poll on them
  // when a state change is detected we need to send an 'p' into the corresponding pipe to start the sound_job
  int err = mkfifo("/tmp/gpio-test-fifo", 0666);
  if (err != 0 && errno != EEXIST) {
    perror("mkfifo error");
  } else {
    debug_printf("Successfully opened FIFO\n");
  }
  int fd = open("/tmp/gpio-test-fifo", O_RDONLY);
  debug_printf("Opened FIFO for writing\n");

  while(1) {
    char mb;
    if (read(fd, &mb, 1) > 0) {
      debug_printf("received %c from pipe\n", mb);
      write(all_jobs[0].pipefd[1], (const void *)&mb, 1);
    }
  }
  wait(NULL);
}