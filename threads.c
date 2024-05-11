
// - include guard -----------------------------------------------------------
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

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


typedef struct { // shared date structure
   int alarm_period;
   int cid;
   int prev_cid;
   bool quit;
   int fd; //forwarding
   int rd;// recieving
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


   
} data_t;

void call_termios(int reset); // raw mode terminal
void create_threads(pthread_t threads[], void *(*thr_functions[])(void *),
                    data_t *data, const char *threads_names[]);
void join_threads(pthread_t threads[], const char *threads_names[]);

void init_mutex_cond(pthread_mutex_t *mtx, pthread_cond_t *cond, data_t *data);
void call_termios(int reset);


void process_input(char c, data_t* data);

void handle_get_version(data_t* data);
void handle_set_compute(data_t* data);
void handle_compute_start(data_t* data);
void handle_refresh_screen(data_t* data);
void handle_abort(data_t* data);
void handle_reset(data_t* data);


void out_handle_version(data_t* data, uint8_t* c, unsigned char* img);
void out_handle_error(uint8_t* c);
void out_handle_refresh(data_t* data, unsigned char* img);
void out_handle_done(data_t* data, uint8_t* c);
void out_handle_abort(data_t* data, uint8_t* c);
void out_handle_compute_data(data_t* data, uint8_t* c, unsigned char* img);

void exit_output_thread(data_t* data, unsigned char* img);


void open_files(data_t* data);

unsigned char* allocate_image_buf(int width, int height);
void default_redraw(unsigned char* img, int width, int height);


void* input_thread(void*);
void* output_thread(void*);
void* alarm_thread(void*);
bool send_message(data_t *data, message *msg);
message *buffer_parse(data_t *data, int message_type);

enum { INPUT, OUTPUT, ALARM, NUM_THREADS };


// - main function -----------------------------------------------------------
int main(int argc, char *argv[])
{
   call_termios(0);
   data_t data = { .alarm_period = 0,.quit = false, .fd = EOF, .is_serial_open = false, .abort = false, .cid = 0, .compute_used = false, .is_compute_set = false, .refresh_screen = false, .compute_done = false, .module_quit = false };
   const char *threads_names[] = { "Input", "Output", "Alarm", };
   void* (*thr_functions[])(void*) = { input_thread, output_thread, alarm_thread};

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


void* input_thread(void* d)
{
   data_t *data = (data_t*)d;
   static int r = 0;
   bool qq = false;
   while((!qq)){ // until pipe isnt open - dont do anything
      pthread_mutex_lock(data->mtx); 
      qq = data->is_serial_open;
      pthread_mutex_unlock(data->mtx);
   }
   uint8_t c;
   while ((c = getchar()) != 'q') {
      process_input(c, data);
   }

   data->quit = true;
   r = 1;
   pthread_mutex_lock(data->mtx);
   pthread_cond_broadcast(data->cond);
   pthread_mutex_unlock(data->mtx);
   fprintf(stderr, "\033[1;35mTHREAD\033[0m: Exit input thread\r\n");
   return &r;
}

void* output_thread(void* d)
{
   data_t *data = (data_t*)d;
   static int r = 0;
   bool q = false;
   open_files(data);

   message msg  = {.type = MSG_STARTUP, .data.startup = { .message = "Henlo"}};
   send_message(data, &msg);

   //open SDL window
   xwin_init(W, H);
  
   unsigned char *img = allocate_image_buf(W, H);
   default_redraw(img, W, H);
   

   if (io_putc(data->fd, 'i') != 1) { // sends init byte
      fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to send the init byte\n");
      exit(1);
   }
   data->is_serial_open = true;
   while (!q) { // main loop for data output
      uint8_t c = '\0'; 
      io_getc_timeout(data->rd, 0,&c);      
      if(c == MSG_VERSION){
         out_handle_version(data, &c, img);
      }
      if(c == MSG_ERROR){
         out_handle_error(&c);
      }
      if(data->refresh_screen){
         out_handle_refresh(data, img);
      }
      if(c == MSG_DONE){
         out_handle_done(data, &c);
      }
      if(c == MSG_ABORT){
         out_handle_abort(data, &c);
      }
      if(c == MSG_COMPUTE_DATA){
         out_handle_compute_data(data, &c, img);
      }
      q = data->quit;
      fflush(stdout);
   }

   exit_output_thread(data, img);
   return &r;
}



// - function -----------------------------------------------------------------
void* alarm_thread(void* d) 
{
   data_t *data = (data_t*)d;
   bool qq = false;
   while((!qq)){ // until pipe isnt open - dont do anything
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
   fprintf(stderr, "\033[1;35mTHREAD\033[0m: Exit alarm thread %lu\r\n", (unsigned long)pthread_self());
   return &r;
}




bool send_message(data_t *data, message *msg){
   uint8_t msg_buf[sizeof(message)];
   int size;
   fill_message_buf(msg, msg_buf,sizeof(message), &size);
   pthread_mutex_lock(data->mtx);
   int ret = write(data->fd, msg_buf, size);
   pthread_mutex_unlock(data->mtx);
   return size == ret;
}

message *buffer_parse(data_t *data, int message_type){
    uint8_t c;
    int len = 0;
    uint8_t msg_buf[sizeof(message)];
    int i = 0;
    get_message_size(message_type, &len);
    msg_buf[i++] = message_type; // add the first byte 
    while((i < len)){
        io_getc_timeout(data->rd, 0, &c);
        msg_buf[i++] = c;
    }
    message *msg = malloc(sizeof(message));
    msg->type = message_type;
    get_message_size(message_type, &len);
    if(!parse_message_buf(msg_buf, len, msg)){
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

void call_termios(int reset)
{
   static struct termios tio, tioOld;
   tcgetattr(STDIN_FILENO, &tio);
   if (reset) {
      tcsetattr(STDIN_FILENO, TCSANOW, &tioOld);
   } else {
      tioOld = tio; //backup 
      cfmakeraw(&tio);
      tcsetattr(STDIN_FILENO, TCSANOW, &tio);
   }
}




void handle_get_version(data_t* data) {
    message msg = {.type = MSG_GET_VERSION};
    send_message(data, &msg);
     printf("\033[1;34mINFO\033[0m: Get version set\r\n");
}
void handle_set_compute(data_t* data) {
    message msg = {.type = MSG_SET_COMPUTE, .data.set_compute = { .c_re = -0.4, .c_im = 0.6, .d_re = 0.005, .d_im = (double)-11/2400, .n = 60}};
    data->n = 60;
    send_message(data, &msg);
    data->is_compute_set = true;
    printf("\033[1;34mINFO\033[0m: Set compute message sent\r\n");
}
void handle_compute_start(data_t* data) {
   if(!data->is_compute_set){
      printf("\033[1;33mWARNING\033[0m: Compute message is not set\r\n");
      printf("\033[1;32mHINT:\033[0m: If you want to set compute message, press s\r\n");
      return;
   }
   if(data->compute_used){
      printf("\033[1;33mWARNING\033[0m: Compute thread is already running\r\n");
      printf("\033[1;32mHINT:\033[0m: If you want to abort computation, press a\r\n");
      return;
   }
   if(data->compute_done){
      printf("\033[1;33mWARNING\033[0m: Compute thread is already done\r\n");
      printf("\033[1;32mHINT:\033[0m: If you want to reset cid, press r\r\n");
      return;
   }
   data->abort = false;

   double re = -1.6; //start of the x-coords (real)
   double im =1.1; //start of the y-coords (imaginary)
   data->prev_cid = data->cid;
   message msg2 = {.type = MSG_COMPUTE, .data.compute = { .cid = data->cid, .re = re, .im = im ,.n_re = N_RE, .n_im = N_IM}};
   send_message(data, &msg2);
   data->compute_used = true;
}

void handle_refresh_screen(data_t* data) {
    if(!data->compute_used)
        data->refresh_screen = true;
    else{
        printf("\033[1;33mWARNING\033[0m: Computing is underway - cant refresh window\r\n");
        printf("\033[1;32mHINT:\033[0m: If you want to abort computing, press a\r\n");
    }
}

void handle_abort(data_t* data) {
    data->abort = true;
    data->compute_used = false;
    message msg2 = {.type = MSG_ABORT};
    send_message(data, &msg2);
 }

void handle_reset(data_t* data) {
    data->cid = 0;
    printf("\033[1;34mINFO\033[0m: Reset cid\r\n");
    data->compute_done = false;
}

void open_files(data_t* data) {
   data->fd = io_open_write(MY_DEVICE_OUT);
   if (data->fd == EOF) {
      fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to open the file %s\n", MY_DEVICE_OUT);
      exit(1);
   }
   data->rd = io_open_read(MY_DEVICE_IN);
   if (data->rd == EOF){
      fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to open the file %s\n", MY_DEVICE_IN);
      exit(1); 
   }
}

unsigned char* allocate_image_buf(int width, int height) {
    unsigned char *img = malloc(width * height * 3);  // 3 bytes per pixel for RGB
    if (img == NULL) {
        fprintf(stderr, "Failed to allocate memory for image\n");
        exit(1);
    }
    return img;
}

void default_redraw(unsigned char* img, int width, int height) {
    for (int y = 0; y < height; ++y) { // fill the image with some color
        for (int x = 0; x < width; ++x) {
            int idx = (y * width + x) * 3;
            img[idx] = 100; // red component
            img[idx + 1] = 0; // green component
            img[idx + 2] = 10; // blue component
        }
    }
   xwin_redraw(W, H, img);
}

void process_input(char c, data_t* data) {
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
    }
}






void out_handle_version(data_t* data, uint8_t* c, unsigned char* img) {
    message *msg = buffer_parse(data, MSG_VERSION);
    printf("\033[1;32mVERSION\033[0m: %c. %c. %c\r\n", msg->data.version.major, msg->data.version.minor, msg->data.version.patch);
    free(msg);
    *c = '\0';
}

void out_handle_error(uint8_t* c) {
    printf("\033[1;31mERROR\033[0m: Module sent error\r\n");
}

void out_handle_refresh(data_t* data, unsigned char* img) {
    printf("\033[1;34mINFO\033[0m: Refreshing screen\r\n");
    data->refresh_screen = false;
    default_redraw(img, W, H);
}

void out_handle_done(data_t* data, uint8_t* c) {
    message *msg = buffer_parse(data, MSG_DONE);
    printf("\033[1;34mINFO\033[0m: Done message recieved\r\n");
    data->compute_used = false;
    data->compute_done = true;
    free(msg);
    *c = '\0';
}

void out_handle_abort(data_t* data, uint8_t* c) {
    message *msg = buffer_parse(data, MSG_ABORT);
    printf("\033[1;33mWARNING\033[0m: Abort message recieved\r\n");
    data->compute_used = false;
    data->abort = true;
    free(msg);
    *c = '\0';
}

void out_handle_compute_data(data_t* data, uint8_t* c, unsigned char* img) {
    message *msg = buffer_parse(data, MSG_COMPUTE_DATA);
    uint8_t i_re = msg->data.compute_data.i_re;
    uint8_t i_im = msg->data.compute_data.i_im;
    int x_im = (msg->data.compute_data.cid % N_RE)*SIZE_C_W;  // starting pos for redraw - one chunk
    int y_im = (msg->data.compute_data.cid / N_RE)*SIZE_C_H;
    data->cid = msg->data.compute_data.cid;
    int x = x_im + i_re;  // x coordinate of the pixel in the image
    int y = y_im + i_im;  // y coordinate of the pixel in the image
    int idx = (y * W + x) * 3;  // index of the pixel in the 1D array
    double t = (double)msg->data.compute_data.iter / data->n; // t is in [0, 1]
    img[idx] = RED(t);; // red component
    img[idx + 1] = GREEN(t); // green component
    img[idx + 2] = BLUE(t); // blue component
    if(data->cid != data->prev_cid){ // if the chunk is done (cid changed
        xwin_redraw(W, H, img);
    }
    else if(data->cid == NUM_CHUNKS-1 && idx == 3*W*H){ // if the last chunk is done
        xwin_redraw(W, H, img);
    }
    data->prev_cid = data->cid;
    free(msg);
    *c = '\0';
}


void exit_output_thread(data_t* data, unsigned char* img) {
    if (io_putc(data->fd, 'q') != 1) { // sends exit byte
        fprintf(stderr, "\033[1;31mERROR\033[0m: Unable to send the end byte\r\n");
        exit(1);
    }
    io_close(data->fd);
    io_close(data->rd);
    fprintf(stderr, "\033[1;35mTHREAD\033[0m: Exit output thread %lu\r\n", (unsigned long)pthread_self());
    xwin_close();
    free(img);
}
/* end of threads.c */
