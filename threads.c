#include <complex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/types.h>
#include <time.h>

#include <string.h>
#include <termios.h>
#include <unistd.h> // for STDIN_FILENO

#include <pthread.h>

#include "prg_io_nonblock.h"

#define MY_DEVICE_OUT "/tmp/pipe.out"
#define MY_DEVICE_IN "/tmp/pipe.in"

#define NUM_CHUNKS 100
#define SIZE_C_W 64
#define SIZE_C_H 48
#define W 640
#define H 480
#define N_RE 10
#define N_IM 10

#define RED(t) (uint8_t)(9 * (1 - t) * t * t * t * 255)
#define GREEN(t) (uint8_t)(15 * (1 - t) * (1 - t) * t * t * 255)
#define BLUE(t) (uint8_t)(8.5 * (1 - t) * (1 - t) * (1 - t) * t * 255)

#include "messages.h"
#include "xwin_sdl.h"

typedef struct { // shared data structure
  int alarm_period;
  int cid;
  int prev_cid;
  bool quit;
  int fd;              // forwarding
  int rd;              // recieving
  bool is_serial_open; // if comunication established
  bool abort;
  pthread_mutex_t *mtx;
  pthread_cond_t *cond;
  bool compute_used;
  bool is_compute_set;

  bool refresh_screen;

  bool module_quit;

  bool compute_done;

  uint8_t n;

  double c_re;
  double c_im;
  double d_re;
  double d_im;
  double re;
  double im;

  unsigned char *img;

  bool local_compute;

  int num_chunks;
  int w;
  int h;
  int n_re;
  int n_im;

} data_t;

void call_termios(int reset); // raw mode terminal
void create_threads(pthread_t threads[], void *(*thr_functions[])(void *),
                    data_t *data, const char *threads_names[]);
void join_threads(pthread_t threads[], const char *threads_names[]);

void init_mutex_cond(pthread_mutex_t *mtx, pthread_cond_t *cond, data_t *data);
void call_termios(int reset);
bool get_values_from_argv(int argc, char *argv[], data_t *data);

void process_input(char c, data_t *data);

void handle_get_version(data_t *data);
void handle_set_compute(data_t *data);
void handle_compute_start(data_t *data);
void handle_refresh_screen(data_t *data);
void handle_abort(data_t *data);
void handle_reset(data_t *data);
void handle_local_compute(data_t *data);

void out_handle_version(data_t *data, uint8_t *c, unsigned char *img);
void out_handle_error(uint8_t *c);
void out_handle_refresh(data_t *data, unsigned char *img);
void out_handle_done(data_t *data, uint8_t *c);
void out_handle_abort(data_t *data, uint8_t *c);
void out_handle_compute_data(data_t *data, uint8_t *c, unsigned char *img);

void exit_output_thread(data_t *data, unsigned char *img);
void exit_input_thread(data_t *data);

void compute_julia_set(data_t *data, unsigned char *img);

void open_files(data_t *data);

unsigned char *allocate_image_buf(int width, int height);
void default_redraw(unsigned char *img, int width, int height, data_t *data);

void save_image_as_png(unsigned char *img, unsigned width, unsigned height,
                       const char *filename);

void *input_thread(void *);
void *output_thread(void *);
void *alarm_thread(void *);
bool send_message(data_t *data, message *msg);
message *buffer_parse(data_t *data, int message_type);

enum { INPUT, OUTPUT, ALARM, NUM_THREADS };

// - main function -----------------------------------------------------------
int main(int argc, char *argv[]) {

  data_t data = {
      .alarm_period = 0,
      .quit = false,
      .fd = EOF,
      .is_serial_open = false,
      .abort = false,
      .cid = 0,
      .compute_used = false,
      .is_compute_set = false,
      .refresh_screen = false,
      .compute_done = false,
      .module_quit = false,
      .re = -1.6,
      .im = 1.1,
      .c_re = -0.4,
      .c_im = 0.6,
      .d_re = 0.005,
      .d_im = (double)-11 / 2400,
      .n = 60,
      .img = NULL,
      .local_compute = false,
      .w = W,
      .h = H,
      .n_re = N_RE,
      .n_im = N_IM,
      .num_chunks = NUM_CHUNKS,
  };

  if (!get_values_from_argv(argc, argv, &data)) {
    printf("\033[1;33mWARNING\033[0m: Invalid input arguments - using default "
           "values\r\n");
  }

  call_termios(0);
  const char *threads_names[] = {
      "Input",
      "Output",
      "Alarm",
  };
  void *(*thr_functions[])(void *) = {input_thread, output_thread,
                                      alarm_thread};

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
  bool qq = false;
  while ((!qq)) { // until pipe isnt open - dont do anything
    pthread_mutex_lock(data->mtx);
    qq = data->is_serial_open;
    pthread_mutex_unlock(data->mtx);
  }
  uint8_t c;
  while ((c = getchar()) != 'q') {
    process_input(c, data);
  }
  pthread_mutex_lock(data->mtx);
  data->quit = true;
  pthread_mutex_unlock(data->mtx);
  exit_input_thread(data);
  return &r;
}

void *output_thread(void *d) {
  data_t *data = (data_t *)d;
  static int r = 0;
  bool q = false;
  open_files(data);

  message msg = {.type = MSG_STARTUP, .data.startup = {.message = "Henlo"}};
  send_message(data, &msg);

  // open SDL window
  xwin_init(data->w, data->h);

  unsigned char *img = allocate_image_buf(data->w, data->h);
  pthread_mutex_lock(data->mtx);
  data->img = img;
  pthread_mutex_unlock(data->mtx);
  default_redraw(img, data->w, data->h, data);

  if (io_putc(data->fd, 'i') != 1) { // sends init byte
    fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to send the init byte\n");
    exit(1);
  }
  data->is_serial_open = true;
  while (!q) { // main loop for data output
    uint8_t c = '\0';
    io_getc_timeout(data->rd, 0, &c);
    if (c == MSG_VERSION) {
      out_handle_version(data, &c, img);
    }
    if (c == MSG_ERROR) {
      out_handle_error(&c);
    }
    if (data->refresh_screen) {
      out_handle_refresh(data, img);
    }
    if (c == MSG_DONE) {
      out_handle_done(data, &c);
    }
    if (c == MSG_ABORT) {
      out_handle_abort(data, &c);
    }
    if (c == MSG_COMPUTE_DATA) {
      out_handle_compute_data(data, &c, img);
    }
    if (data->local_compute) {
      pthread_mutex_lock(data->mtx);
      data->compute_used = true;
      pthread_mutex_unlock(data->mtx);
      compute_julia_set(data, img);
      xwin_redraw(data->w, data->h, img);
      pthread_mutex_lock(data->mtx);
      data->local_compute = false;
      data->compute_used = false;
      pthread_mutex_unlock(data->mtx);
    }
    q = data->quit;
    fflush(stdout);
  }

  exit_output_thread(data, img);
  r=1;
  return &r;
}

// - function -----------------------------------------------------------------
void *alarm_thread(void *d) {
  data_t *data = (data_t *)d;
  bool qq = false;
  while ((!qq)) { // until pipe isnt open - dont do anything
    pthread_mutex_lock(data->mtx);
    qq = data->is_serial_open;
    pthread_mutex_unlock(data->mtx);
  }
  printf("\033[1;34mINFO\033[0m: pipe unlocked\r\n");
  static int r = 0;
  bool q = data->quit;

  while (!q) {
    q = data->quit;
    xwin_poll_events();
  }
  fprintf(stdin, "\033[1;35mTHREAD\033[0m: Exit alarm thread\r\n");
  return &r;
}

bool send_message(data_t *data, message *msg) {
  uint8_t msg_buf[sizeof(message)];
  int size;
  fill_message_buf(msg, msg_buf, sizeof(message), &size);
  pthread_mutex_lock(data->mtx);
  int ret = write(data->fd, msg_buf, size);
  pthread_mutex_unlock(data->mtx);
  return size == ret;
}

message *buffer_parse(data_t *data, int message_type) {
  uint8_t c;
  int len = 0;
  uint8_t msg_buf[sizeof(message)];
  int i = 0;
  get_message_size(message_type, &len);
  msg_buf[i++] = message_type; // add the first byte
  while ((i < len)) {
    io_getc_timeout(data->rd, 0, &c);
    msg_buf[i++] = c;
  }
  message *msg = malloc(sizeof(message));
  msg->type = message_type;
  get_message_size(message_type, &len);
  if (!parse_message_buf(msg_buf, len, msg)) {
    fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to parse the message\n");
    free(msg);
    exit(1);
  }
  return msg;
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
    printf("\033[1;35mTHREAD\033[0m: Joining the thread %s has been %s\r\n",
           threads_names[i], (r == 0 ? "OK" : "FAIL"));
  }
}

void init_mutex_cond(pthread_mutex_t *mtx, pthread_cond_t *cond, data_t *data) {
  pthread_mutex_init(mtx, NULL); // initialize mutex with default attributes
  pthread_cond_init(
      cond, NULL); // initialize condition variable with default attributes
  data->mtx = mtx; // make the mutex accessible from the shared data structure
  data->cond = cond;
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

void handle_get_version(data_t *data) {
  message msg = {.type = MSG_GET_VERSION};
  send_message(data, &msg);
  printf("\033[1;34mINFO\033[0m: Get version set\r\n");
}
void handle_set_compute(data_t *data) {
  message msg = {.type = MSG_SET_COMPUTE,
                 .data.set_compute = {.c_re = data->c_re,
                                      .c_im = data->c_im,
                                      .d_re = data->d_re,
                                      .d_im = data->d_im,
                                      .n = data->n}};
  send_message(data, &msg);
  pthread_mutex_lock(data->mtx);
  data->is_compute_set = true;
  pthread_mutex_unlock(data->mtx);
  printf("\033[1;34mINFO\033[0m: Set compute message sent\r\n");
}
void handle_compute_start(data_t *data) {
  if (!data->is_compute_set) {
    printf("\033[1;33mWARNING\033[0m: Compute message is not set\r\n");
    printf("\033[1;32mHINT:\033[0m: If you want to set compute message, press "
           "s\r\n");
    return;
  }
  if (data->compute_used) {
    printf("\033[1;33mWARNING\033[0m: Compute thread is already running\r\n");
    printf("\033[1;32mHINT:\033[0m: If you want to abort computation, press "
           "a\r\n");
    return;
  }
  if (data->compute_done) {
    printf("\033[1;33mWARNING\033[0m: Compute thread is already done\r\n");
    printf("\033[1;32mHINT:\033[0m: If you want to reset cid, press r\r\n");
    return;
  }
  pthread_mutex_lock(data->mtx);
  data->abort = false;
  data->prev_cid = data->cid;
  pthread_mutex_unlock(data->mtx);
  message msg2 = {.type = MSG_COMPUTE,
                  .data.compute = {.cid = data->cid,
                                   .re = data->re,
                                   .im = data->im,
                                   .n_re = data->n_re,
                                   .n_im = data->n_im}};
  send_message(data, &msg2);
  pthread_mutex_lock(data->mtx);
  data->compute_used = true;
  pthread_mutex_unlock(data->mtx);
}

void handle_refresh_screen(data_t *data) {
  if (!data->compute_used){
    pthread_mutex_lock(data->mtx);
    data->refresh_screen = true;
    pthread_mutex_unlock(data->mtx);
  }
  
   
  else {
    printf("\033[1;33mWARNING\033[0m: Computing is underway - cant refresh "
           "window\r\n");
    printf(
        "\033[1;32mHINT:\033[0m: If you want to abort computing, press a\r\n");
  }
}

void handle_abort(data_t *data) {
  pthread_mutex_lock(data->mtx);
  data->abort = true;
  data->compute_used = false;
  pthread_mutex_unlock(data->mtx);
  message msg2 = {.type = MSG_ABORT};
  send_message(data, &msg2);
}

void handle_reset(data_t *data) {
  pthread_mutex_lock(data->mtx);
  data->cid = 0;
  printf("\033[1;34mINFO\033[0m: Reset cid\r\n");

  data->compute_done = false;
  pthread_mutex_unlock(data->mtx);
}

void open_files(data_t *data) {
  data->fd = io_open_write(MY_DEVICE_OUT);
  if (data->fd == EOF) {
    fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to open the file %s\n",
            MY_DEVICE_OUT);
    exit(1);
  }
  data->rd = io_open_read(MY_DEVICE_IN);
  if (data->rd == EOF) {
    fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to open the file %s\n",
            MY_DEVICE_IN);
    exit(1);
  }
}

unsigned char *allocate_image_buf(int width, int height) {
  unsigned char *img = malloc(width * height * 3); // 3 bytes per pixel for RGB
  if (img == NULL) {
    fprintf(stderr, "\033[1;31mERROR\033[0m: Failed to allocate memory for image\n");
    exit(1);
  }
  return img;
}

void default_redraw(unsigned char *img, int width, int height, data_t *data) {
  for (int y = 0; y < data->h; ++y) { // fill the image with some color
    for (int x = 0; x < data->w; ++x) {
      int idx = (y * data->w + x) * 3;
      img[idx] = 100;    // red component
      img[idx + 1] = 0;  // green component
      img[idx + 2] = 10; // blue component
    }
  }
  printf("\033[1;34mINFO\033[0m: Default image set\r\n");
  xwin_redraw(data->w, data->h, img);
}

void process_input(char c, data_t *data) {
  switch (c) {
  case 'g':
    handle_get_version(data);
    break;
  case 's':
    handle_set_compute(data);
    break;
  case '1':
    handle_compute_start(data);
    break;
  case 'l':
    handle_refresh_screen(data);
    break;
  case 'a':
    handle_abort(data);
    break;
  case 'r':
    handle_reset(data);
    break;
  case 'c':
    handle_local_compute(data);
    break;
  case 'd':
    save_image_as_png(data->img, data->w, data->h, "julia.png");
    break;
  }
}

void out_handle_version(data_t *data, uint8_t *c, unsigned char *img) {
  message *msg = buffer_parse(data, MSG_VERSION);
  printf("\033[1;32mRECIEVED VERSION\033[0m: %c. %c. %c\r\n", msg->data.version.major,
         msg->data.version.minor, msg->data.version.patch);
  free(msg);
  *c = '\0';
}

void out_handle_error(uint8_t *c) {
  printf("\033[1;31mERROR\033[0m: Module sent error\r\n");
}

void out_handle_refresh(data_t *data, unsigned char *img) {
  printf("\033[1;34mINFO\033[0m: Refreshing screen\r\n");
  pthread_mutex_lock(data->mtx);
  data->refresh_screen = false;
  pthread_mutex_unlock(data->mtx);
  default_redraw(data->img, data->w, data->w, data);
}

void out_handle_done(data_t *data, uint8_t *c) {
  message *msg = buffer_parse(data, MSG_DONE);
  printf("\033[1;34mINFO\033[0m: Done message recieved\r\n");
  pthread_mutex_lock(data->mtx);
  data->compute_used = false;
  data->compute_done = true;
  pthread_mutex_unlock(data->mtx);
  free(msg);
  *c = '\0';
}

void out_handle_abort(data_t *data, uint8_t *c) {
  message *msg = buffer_parse(data, MSG_ABORT);
  printf("\033[1;33mWARNING\033[0m: Abort message recieved\r\n");
  pthread_mutex_lock(data->mtx);
  data->compute_used = false;
  data->abort = true;
  pthread_mutex_unlock(data->mtx);
  free(msg);
  *c = '\0';
}

void out_handle_compute_data(data_t *data, uint8_t *c, unsigned char *img) {
  message *msg = buffer_parse(data, MSG_COMPUTE_DATA);
  uint8_t i_re = msg->data.compute_data.i_re;
  uint8_t i_im = msg->data.compute_data.i_im;
  int x_im = (msg->data.compute_data.cid % data->n_re) *
             SIZE_C_W; // starting pos for redraw - one chunk
  int y_im = (msg->data.compute_data.cid / data->n_im) * SIZE_C_H;
  data->cid = msg->data.compute_data.cid;
  int x = x_im + i_re;             // x coordinate of the pixel in the image
  int y = y_im + i_im;             // y coordinate of the pixel in the image
  int idx = (y * data->w + x) * 3; // index of the pixel in the 1D array
  double t = (double)msg->data.compute_data.iter / data->n; // t is in [0, 1]
  img[idx] = RED(t);
  ;                                  // red component
  img[idx + 1] = GREEN(t);           // green component
  img[idx + 2] = BLUE(t);            // blue component
  if (data->cid != data->prev_cid) { // if the chunk is done (cid changed
    xwin_redraw(data->w, data->h, img);
  } else if ((data->cid == data->num_chunks - 1 &&
              idx == 3 * data->w * data->h)) { // if the last chunk is done
    xwin_redraw(data->w, data->h, img);
  }
  pthread_mutex_lock(data->mtx);
  data->prev_cid = data->cid;
  pthread_mutex_unlock(data->mtx);
  free(msg);
  *c = '\0';
}

void exit_output_thread(data_t *data, unsigned char *img) {
  message msg = {.type = MSG_ABORT};
  send_message(data, &msg);
  if (io_putc(data->fd, 'q') != 1) { // sends exit byte
    fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to send the end byte\r\n");
    exit(1);
  }
  io_close(data->fd);
  io_close(data->rd);
  fprintf(stdout, "\033[1;35mTHREAD\033[0m: Exit output thread\r\n");
  xwin_close();
  free(img);
  data->module_quit = true;
}

void exit_input_thread(data_t *data) {
  pthread_mutex_lock(data->mtx);
  pthread_cond_broadcast(data->cond);
  pthread_mutex_unlock(data->mtx);
  fprintf(stdout, "\033[1;35mTHREAD\033[0m: Exit input thread\r\n");
}

void handle_local_compute(data_t *data) {

  if (data->compute_used) {
    printf("\033[1;33mWARNING\033[0m: Compute thread is already running\r\n");
    printf("\033[1;32mHINT:\033[0m: If you want to abort computation, press "
           "a\r\n");
    return;
  }
  pthread_mutex_lock(data->mtx);
  data->local_compute = true;
  pthread_mutex_unlock(data->mtx);
}

void compute_julia_set(data_t *data, unsigned char *img) {

  uint8_t iter;
  double complex Z;
  double complex C = data->c_re + data->c_im * I;
  for (int x = 0; x <= data->w; x++) {   // for size of chunk
    for (int y = 0; y <= data->h; y++) { // for size of chunk
      Z = (data->re + x * data->d_re) + (data->im + y * data->d_im) * I;
      iter = 0;
      while (cabs(Z) < 2 && iter < data->n) {
        Z = Z * Z + C;
        iter++;
      }
      // printf("\033[1;34mINFO\033[0m: : Chunk %d: x = %d, y = %d, iter =
      // %d\r\n", data->cid, x, y, iter);

      int idx = (y * data->w + x) * 3;
      double t = (double)iter / data->n; // t is in [0, 1]
      img[idx] = RED(t);
      ;                        // red component
      img[idx + 1] = GREEN(t); // green component
      img[idx + 2] = BLUE(t);  // blue component

      // printf("\033[1;34mINFO\033[0m: : sent compute data\r\n");
    }
  }
  // printf("\033[1;34m-->\033[0m: : Chunk %d is done\n\r", data->cid);
}
/* end of threads.c */

void save_image_as_png(unsigned char *img, unsigned width, unsigned height,
                       const char *filename) {
  FILE *tmpFile = fopen("tmp.ppm", "wb"); // binary write
  if (tmpFile == NULL) {
    fprintf(stderr, "\033[1;31mERROR\033[0m: Failed to open temp file");
    return;
  }

  // Write the PPM header
  fprintf(tmpFile, "P6\n%d %d\n255\n", width,
          height); // magic number P6 - using ImageMagick

  // Write the image data
  if (fwrite(img, 3, width * height, tmpFile) != width * height) {
    fprintf(stderr, "\033[1;31mERROR\033[0m: Failed to write image data");
  }

  fclose(tmpFile);

  // Convert the PPM image to PNG using ImageMagick
  char command[256];
  snprintf(command, sizeof(command), "convert tmp.ppm %s", filename);
  system(command);

  // Delete the temporary PPM file
  remove("tmp.ppm");
}
bool get_values_from_argv(int argc, char *argv[], data_t *data) {
  double values[6];
  uint8_t n;
  if (argc == 9) {
    char *end;
    for (int i = 0; i < 6; i++) {
      values[i] = strtod(argv[i + 1], &end);
      if (*end != '\0') {
        return false;
      }
    }

    n = (uint8_t)strtol(argv[7], &end, 10);
    if (*end != '\0') {
      return false;
    }

    int integer_param = strtol(argv[8], &end, 10);
    if (*end != '\0' || integer_param < 1 || integer_param > 3) {
      return false;
    }

    if (integer_param == 1) {
      pthread_mutex_lock(data->mtx);
      data->num_chunks = 144;
      data->w = 768;
      data->h = 576;
      data->n_re = 12;
      data->n_im = 12;
      pthread_mutex_unlock(data->mtx);
    } else if (integer_param == 2) {
      pthread_mutex_lock(data->mtx);
      data->num_chunks = 100;
      data->w = 640;
      data->h = 480;
      data->n_re = 10;
      data->n_im = 10;
      pthread_mutex_unlock(data->mtx);

    } else if (integer_param == 3) {
      pthread_mutex_lock(data->mtx);
      data->num_chunks = 13 * 13;
      data->w = 832;
      data->h = 624;
      data->n_re = 13;
      data->n_im = 13;
      pthread_mutex_unlock(data->mtx);
    }
  } else {
    return false;
  }
  pthread_mutex_lock(data->mtx);
  data->c_re = values[0];
  data->c_im = values[1];
  data->im = values[2];
  data->re = values[3];
  data->d_im = values[4];
  data->d_re = values[5];
  data->n = n;
  pthread_mutex_unlock(data->mtx);
  return true;
}