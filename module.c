#include <complex.h> // for julia set complex numbers
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <threads.h>
#include <unistd.h> // for STDIN_FILENO

#include "messages.h"
#include "prg_io_nonblock.h" // send and recieves bites through pipe
#include <pthread.h>

#define MY_DEVICE_OUT "/tmp/pipe.out"
#define MY_DEVICE_IN "/tmp/pipe.in"

void call_termios(int reset);

// constants
#define NUM_CHUNKS 100
#define CHUNK_PER_ROW 10

typedef struct { // shared date structure;
  bool quit;
  int fd;              // forwarding
  int rd;              // recieving
  bool is_serial_open; // if comunication established
  bool abort;
  bool is_cond_signaled;
  bool is_message_recieved;
  pthread_mutex_t *mtx;
  pthread_cond_t *cond;

  bool is_abort;

  // set compute data
  double c_re;
  double c_im;
  double d_re;
  double d_im;
  int n;

  // computation data
  uint8_t cid;
  double re;
  double im;
  uint8_t n_re;
  uint8_t n_im;

} data_t;

// constants definitions
enum { INPUT, CALCULATION, INPUT_FROM_STDIN, NUM_THREADS };
#define CHUNK_SIZE_W 64
#define CHUNK_SIZE_H 48

void *input_thread(void *);
void *calculation_thread(void *);
void *input_from_stdin_thread(void *);

// message comunication
message *buffer_parse(data_t *data, int message_type);
bool send_message(data_t *data, message *msg);
void process_message(data_t *data, uint8_t c);
int open_file(const char *filename, bool is_read);

// julia set computation
void compute_julia_set(data_t *data);
void calculate_chunk_coordinates(data_t *data, double start_re,
                                 double start_im);

// thread management functions
void create_threads(pthread_t threads[], void *(*thr_functions[])(void *),
                    data_t *data, const char *threads_names[]);
void join_threads(pthread_t threads[], const char *threads_names[]);
void init_mutex_cond(pthread_mutex_t *mtx, pthread_cond_t *cond, data_t *data);

int main(int argc, char *argv[]) {
  call_termios(0); // set terminal settings

  // shared data structure
  data_t data = {.quit = false,
                 .fd = EOF,
                 .is_serial_open = false,
                 .abort = false,
                 .is_cond_signaled = false,
                 .cid = 0,
                 .re = 0,
                 .im = 0,
                 .n_re = 0,
                 .n_im = 0,
                 .is_message_recieved = false,
                 .mtx = NULL,
                 .cond = NULL,
                 .c_re = 0,
                 .c_im = 0,
                 .d_re = 0,
                 .d_im = 0,
                 .n = 0};

  // thread names and functions
  const char *threads_names[] = {"Input", "Calculation", "Stdin"};
  void *(*thr_functions[])(void *) = {input_thread, calculation_thread,
                                      input_from_stdin_thread};
  pthread_t threads[NUM_THREADS];

  // mutex and condition variable
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  init_mutex_cond(&mtx, &cond, &data);

  // create threads and wait for their termination
  create_threads(threads, thr_functions, &data, threads_names);
  join_threads(threads, threads_names);

  // clean up
  pthread_mutex_destroy(&mtx);
  pthread_cond_destroy(&cond);
  call_termios(1); // restore terminal settings
  return EXIT_SUCCESS;
}

void *input_thread(void *d) {
  data_t *data = (data_t *)d;
  static int r = 0;
  // open comunication pipes
  data->fd = open_file(MY_DEVICE_OUT, true);
  data->rd = open_file(MY_DEVICE_IN, false);

  printf("\033[1;34mINFO\033[0m: : Input thread is running\r\n");

  // wait for recieving startup message
  while (!data->quit) {
    uint8_t c;
    io_getc_timeout(data->fd, 0, &c);
    if (c == MSG_STARTUP) {
      message *msg = buffer_parse(data, MSG_STARTUP);
      printf("\033[1;34mINFO\033[0m: : Startup: %s\r\n",
             msg->data.startup.message);
      free(msg);
      c = '\0';
      break;
    } else if (c == 'q') {
      data->quit = true;
      break;
    }
  }
  if (!data->quit)
    printf("\033[1;34mINFO\033[0m: : Startup message recieved\r\n");

  while (!data->quit) {
    uint8_t c = '\0';
    io_getc_timeout(data->fd, 0, &c);
    process_message(data, c);
  }
  data->quit = true;
  r = 1;
  pthread_cond_broadcast(data->cond);
  fprintf(stderr, "\033[1;35mTHREAD\033[0m: Exit input thread\r\n");
  return &r;
}

void *calculation_thread(void *d) {
  data_t *data = (data_t *)d;
  static int r = 1;

  bool q = false;
  pthread_mutex_lock(data->mtx);
  while (!q) {
    while (!q && !data->is_cond_signaled) {
      pthread_cond_wait(data->cond, data->mtx);
      q = data->quit;
    }
    if (!data->abort && !q) {
      // compute julia set for each chunk (64x48 pixels on 640 x 480 screen)
      // send the result back to the input thread

      // init the starting points
      double start_re = data->re;
      double start_im = data->im;
      while (!q) {
        if (data->cid == NUM_CHUNKS) {
          printf("\033[1;34mINFO\033[0m: : Calculation thread is done\r\n");
          pthread_mutex_unlock(data->mtx);
          message msg = {.type = MSG_DONE};
          send_message(data, &msg);
          fsync(data->rd);
          pthread_mutex_lock(data->mtx);
          break;
        }

        calculate_chunk_coordinates(data, start_re, start_im);
        compute_julia_set(data);
        if (data->is_abort) {
          break;
        }
        data->cid++;
        q = data->quit;
      }
    }

    data->is_cond_signaled = false;
    q = data->quit;
  }
  pthread_mutex_unlock(data->mtx);
  printf("\033[1;34mINFO\033[0m: : Calculation thread is exiting\r\n");
  return &r;
}

void *input_from_stdin_thread(void *d) {
  struct pollfd fds[1]; // this is copied from internet - i found no other way
                        // to read from stdin without blocking
  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;

  data_t *data = (data_t *)d;
  static int r = 1;
  while (!data->quit) {
    int ret = poll(fds, 1, 0);

    if (ret > 0) {
      char c;
      read(STDIN_FILENO, &c, 1);
      if (c == 'a') {
        message msg = {.type = MSG_ABORT};
        send_message(data, &msg);
        fsync(data->rd);
        data->abort = true;
        data->is_abort = true;
        printf("\033[1;33mWARNING\033[0m: : Aborted from module\r\n");
      }
    }
  }
  printf("\033[1;34mINFO\033[0m: : Stdin thread is exiting\r\n");
  return &r;
}

/*___________________FUNCTIONS___________________*/
bool send_message(data_t *data, message *msg) {
  uint8_t msg_buf[sizeof(message)];
  int size;
  fill_message_buf(msg, msg_buf, sizeof(message), &size);
  pthread_mutex_lock(data->mtx);
  int ret = write(data->rd, msg_buf, size);
  pthread_mutex_unlock(data->mtx);
  if (size != ret) {
    exit(1);
  }
  return size == ret;
}

message *buffer_parse(data_t *data, int message_type) {
  uint8_t c = 0;
  int len = 0;
  uint8_t msg_buf[sizeof(message)];
  int i = 0;

  get_message_size(message_type, &len);
  msg_buf[i++] = message_type; // add the first byte
  while ((i < len)) {
    io_getc_timeout(data->fd, 0, &c);
    msg_buf[i++] = c;
  }
  message *msg = malloc(sizeof(message));
  if (msg == NULL) {
    fprintf(stderr, "\033[1;31mERROR\033[0m:: Unable to allocate memory\r\n");
    exit(1);
  }
  msg->type = message_type;
  get_message_size(message_type, &len);
  if (!parse_message_buf(msg_buf, len, msg)) {
    fprintf(stderr, "\033[1;31mERROR\033[0m:: Unable to parse the message\r\n");
    message msg2 = {.type = MSG_ERROR};
    send_message(data, &msg2);
    fsync(data->rd);
    free(msg);
    exit(1);
  }
  return msg;
}

void call_termios(int reset) {
  static struct termios tio, tioOld;
  tcgetattr(STDIN_FILENO, &tio);
  if (reset) {
    tcsetattr(STDIN_FILENO, TCSANOW, &tioOld);
  } else {
    tioOld = tio; // backup
    cfmakeraw(&tio);
    tcsetattr(STDIN_FILENO, TCSANOW, &tio);
  }
}

void compute_julia_set(data_t *data) {

  uint8_t iter;
  double complex Z;
  double complex C = data->c_re + data->c_im * I;
  for (uint8_t x = 0; x <= CHUNK_SIZE_W; x++) {   // for size of chunk
    for (uint8_t y = 0; y <= CHUNK_SIZE_H; y++) { // for size of chunk
      Z = (data->re + x * data->d_re) + (data->im + y * data->d_im) * I;
      iter = 0;
      while (cabs(Z) < 2 && iter < data->n) {
        Z = Z * Z + C;
        iter++;
      }
      if (data->abort) {
        pthread_mutex_unlock(data->mtx);
        message msg = {.type = MSG_ABORT};
        send_message(data, &msg);
        fsync(data->rd);
        data->is_cond_signaled = false;
        data->is_abort = true;
        pthread_mutex_lock(data->mtx);
        return;
      }
      // printf("\033[1;34mINFO\033[0m: : Chunk %d: x = %d, y = %d, iter =
      // %d\r\n", data->cid, x, y, iter);
      pthread_mutex_unlock(data->mtx);
      message msg = {
          .type = MSG_COMPUTE_DATA,
          .data.compute_data = {data->cid, x, y,
                                iter}}; // for each pixel = x, y in given chunk
      send_message(data, &msg);
      fsync(data->rd);
      // printf("\033[1;34mINFO\033[0m: : sent compute data\r\n");
      pthread_mutex_lock(data->mtx);
    }
  }
  // printf("\033[1;34m-->\033[0m: : Chunk %d is done\n\r", data->cid);
}

void create_threads(pthread_t threads[], void *(*thr_functions[])(void *),
                    data_t *data, const char *threads_names[]) {
  for (int i = 0; i < NUM_THREADS; ++i) { // create threads
    int r = pthread_create(&threads[i], NULL, thr_functions[i], data);
    printf("\033[1;35mTHREAD\033[0m: Create thread '%s' %s\r\n",
           threads_names[i], (r == 0 ? "OK" : "FAIL"));
  }
}

void join_threads(pthread_t threads[], const char *threads_names[]) {
  int *ex;
  for (int i = 0; i < NUM_THREADS;
       ++i) { // join threads so main doesnt end before threads
    printf("\033[1;35mTHREAD\033[0m: Call join to the thread %s\r\n",
           threads_names[i]);
    int r = pthread_join(threads[i], (void *)&ex);
    printf("\033[1;35mTHREAD\033[0m: Joining the thread %s has been %s - exit "
           "value %i\r\n",
           threads_names[i], (r == 0 ? "OK" : "FAIL"), *ex);
  }
}

void init_mutex_cond(pthread_mutex_t *mtx, pthread_cond_t *cond, data_t *data) {
  pthread_mutex_init(mtx, NULL); // initialize mutex with default attributes
  pthread_cond_init(
      cond, NULL); // initialize condition variable with default attributes
  data->mtx = mtx; // make the mutex accessible from the shared data structure
  data->cond = cond;
}

void process_message(data_t *data, uint8_t c) {
  if (c == 'q') {
    data->quit = true;
  } else if (c == MSG_GET_VERSION) {
    printf("\033[1;34mINFO\033[0m: : sending version\r\n");
    message msg = {.type = MSG_VERSION, .data.version = {'1', '2', '2'}};
    if (!send_message(data, &msg))
      exit(1);
    fsync(data->rd);
  } else if (c == MSG_STARTUP) {
    message *msg = buffer_parse(data, MSG_STARTUP);
    printf("\033[1;34mINFO\033[0m: : Startup: %s\r\n",
           msg->data.startup.message);
    free(msg);
  } else if (c == MSG_SET_COMPUTE) {
    printf("\033[1;34mINFO\033[0m: : received set compute\r\n");
    message *msg = buffer_parse(data, MSG_SET_COMPUTE);
    data->c_re = msg->data.set_compute.c_re;
    data->c_im = msg->data.set_compute.c_im;
    data->d_re = msg->data.set_compute.d_re;
    data->d_im = msg->data.set_compute.d_im;
    data->n = msg->data.set_compute.n;
    printf("c_re = %lf, c_im = %lf, d_re = %lf, d_im = %lf, n = %d\r\n",
           data->c_re, data->c_im, data->d_re, data->d_im, data->n);
    free(msg);
  } else if (c == MSG_COMPUTE) {
    printf("\033[1;34mINFO\033[0m: : received compute\r\n");
    message *msg = buffer_parse(data, MSG_COMPUTE);
    data->cid = msg->data.compute.cid;
    data->re = msg->data.compute.re;
    data->im = msg->data.compute.im;
    data->n_re = msg->data.compute.n_re;
    data->n_im = msg->data.compute.n_im;
    data->is_cond_signaled = true;
    data->abort = false;
    data->is_abort = false;
    pthread_cond_broadcast(data->cond);
    free(msg);
  } else if (c == MSG_ABORT) {
    data->abort = true;
    pthread_mutex_unlock(data->mtx);
    message *msg = buffer_parse(data, MSG_ABORT);
    free(msg);
  }
}

int open_file(const char *filename, bool is_read) {
  int fd = is_read ? io_open_read(filename) : io_open_write(filename);
  if (fd == EOF) {
    fprintf(stderr, "\033[1;31mERROR\033[0m:: Unable to open the file %s\r\n",
            filename);
    exit(1);
  }
  return fd;
}
void calculate_chunk_coordinates(data_t *data, double start_re,
                                 double start_im) {
  int x_im = (data->cid % CHUNK_PER_ROW) * CHUNK_SIZE_W; // first chunk (real)
  int y_im =
      (data->cid / CHUNK_PER_ROW) * CHUNK_SIZE_H; // first chunk (imaginary)
  data->re = start_re + x_im * data->d_re;
  data->im = start_im + y_im * data->d_im;
}