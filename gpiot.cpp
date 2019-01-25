#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <vector>
#include <string>
#include <regex>
#include <iostream>
#include <sstream>
#include <fstream>
#include <fcntl.h>
#include <linux/gpio.h>
#include <inttypes.h>
#include <poll.h>
#include <fcntl.h>

#define debug_printf(...) fprintf (stdout, __VA_ARGS__)
#define err_printf(...) fprintf(stderr, __VA_ARGS__)

#define DEBOUNCE_MS (20U)

struct sound_job {
  pid_t pid;
  pid_t child_pid;
  std::string file_name;
  struct gpioevent_request req;
  struct gpiohandle_data data;
  std::string dev;
  int gpio_fd;
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
  std::regex pin_regex(R"B(pin(\d+)\s*=\s*([\w./-_]+))B");

  std::string line;
  int line_no = 1;
  while( std::getline( input, line ) ) {
    std::smatch match;
    std::regex_match(line, match, pin_regex);
    debug_printf("Parsing configuration file Line #%i:\t\t\"%s\"\n\t", line_no, line.c_str());
    if (match.size() == 3) {
      job.req.lineoffset = atoi(match[1].str().c_str());
      job.file_name = match[2].str();
      jobs.push_back(job);
      debug_printf("\t Found \"Pin %i\" connection to Sound file \"%s\"\n", job.req.lineoffset, job.file_name.c_str());
    } else {
      debug_printf("Error parsing configuration file:\n\t Line %s has invalid syntax\n", line.c_str());
      exit(-1);
    }
    ++line_no;
  }
}

static void do_listen_and_play(struct sound_job &job)
{
  job.req.handleflags = GPIOHANDLE_REQUEST_INPUT;
  job.req.eventflags = GPIOEVENT_REQUEST_BOTH_EDGES;
  snprintf(job.req.consumer_label, sizeof(job.req.consumer_label) , "gpio-button-ev%i", job.req.lineoffset);
  int ret = ioctl(job.gpio_fd, GPIO_GET_LINEEVENT_IOCTL, &job.req);
  if (ret == -1) {
    ret = -errno;
    debug_printf("Failed to issue GET EVENT IOCTL (%d) for gpio %s:%i\n", ret, job.dev.c_str(), job.req.lineoffset);
    exit(-1);
  }
  int flags = fcntl(job.req.fd, F_GETFL, 0);
  debug_printf("fcntl for gpio %s : %i returned %i flags on fd\n", job.dev.c_str(), job.req.lineoffset, flags);
  flags |= O_NONBLOCK;

  if(fcntl(job.req.fd, F_SETFL, flags) != 0) {
    debug_printf("Unable to set fcntl for gpio %s : %i\n",job.dev.c_str(), job.req.lineoffset);
    exit (-1);
  }
  ret = ioctl(job.req.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &job.data);
  if (ret == -1) {
    debug_printf("Failed to issue GPIOHANDLE GET LINE VALUES IOCTL (%d)\n", ret);
    exit(-errno);
  }
  debug_printf("Monitoring line %i on %s\n", job.req.lineoffset, job.dev.c_str());
  debug_printf("%s : %d initial line value: %d\n", job.dev.c_str(), job.req.lineoffset, job.data.values[0]);

  while (1) {
    pid_t this_pid = getpid();
    struct pollfd poll_fd = {0};
    /* We will fork new mpg123 processes for each new sound job. We will kill these jobs if a new "play" or "stop" command
       arrives BEFORE the sound job has finished. If we do nothing here these mpg123 processes will be zombied because theire return value is not
       checked by the parent. To avoid this we will register a signal handler which will simply ignore the return value
       REF: https://www.win.tue.nl/~aeb/linux/lk/lk-5.html */
    signal(SIGCHLD, SIG_IGN);
    poll_fd.fd = job.req.fd;
    poll_fd.events = POLLIN;

    while(poll(&poll_fd, 1, -1) == 1) {
      struct gpioevent_data event;
      if (poll_fd.revents & POLLIN) {
        /* debounce in a very trivial way by waiting and let the gpiolib fill the kfifo */
        usleep(DEBOUNCE_MS * 1000);
        while(1) {
          /* read all the garbage from debouncing and just use the last entry (the first one can also be used)*/
          int read_err = read(job.req.fd, &event, sizeof(event));
          if (read_err < 0 && errno == -EAGAIN) {
            if (job.child_pid != -1) {
              /* check if PID is still running. FIXME: this is not save if pids are reused by the kernel which seems not to happen in linux*/
              if (kill(job.child_pid, 0) == 0) {
                debug_printf("Child of %s : %i is still alive playing %s\n",job.dev.c_str(), job.req.lineoffset, job.file_name.c_str());
                /* send the SIGTERM signal to the mpg123 process */
                kill(job.child_pid, SIGTERM);
              }
            }
            switch (event.id) {
            case GPIOEVENT_EVENT_RISING_EDGE:
              printf("rising edge detected\n");
              break;
            case GPIOEVENT_EVENT_FALLING_EDGE:
              printf("falling edge detected\n");
              pid_t cpid = fork();
              if (cpid < 0) {
                debug_printf("Could not fork child pid n");
              } else if (cpid == 0) {
                debug_printf("Start playing \"%s\"(%i)\n", job.file_name.c_str(), this_pid);
                execlp("mpg123", "mpg123", "-q", job.file_name.c_str(), NULL);
              } else {
                job.child_pid = cpid;
              }
              break;
            }
            break;
          }
        }
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
    //FIXME: need to be adjusted according to the pin
    job.dev = "/dev/gpiochip0";
    job.gpio_fd = open(job.dev.c_str(), O_NONBLOCK);
    if (job.gpio_fd < 0) {
      debug_printf("gpio fd open failed with %i for %s : %i\n", errno, job.dev.c_str(), job.req.lineoffset);
      exit(-1);
    }
    pid_t child_pid = fork();
    if (child_pid < 0) {
      perror("Fork error");
    } else if (child_pid == 0) {
      do_listen_and_play(job);
    } else {
      job.pid = child_pid;
    }
  }
  while(1) {sleep(1);}
}
