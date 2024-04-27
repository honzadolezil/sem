

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <termios.h>
#include <unistd.h> // for STDIN_FILENO

#include <pthread.h>

#include "prg_io_nonblock.h"

#define MY_DEVICE_OUT "/tmp/pipe.out"
#define PERIOD_MIN 10
#define PERIOD_MAX 2000
#define PERIOD_STEP 10
#include "messages.h"

typedef struct { // shared date structure
   int alarm_period;
   int alarm_counter;
   bool quit;
   int fd;
   bool is_serial_open; // if comunication established
   pthread_mutex_t *mtx;
   pthread_cond_t *cond;
} data_t;

void call_termios(int reset); // raw mode terminal

void* input_thread(void*);
void* output_thread(void*);
void* alarm_thread(void*);
bool send_message(data_t *data, message *msg);

// - main function -----------------------------------------------------------
int main(int argc, char *argv[])
{
   data_t data = { .alarm_period = 100, .alarm_counter = 0, .quit = false, .fd = EOF, .is_serial_open = false};

   enum { INPUT, OUTPUT, ALARM, NUM_THREADS };
   const char *threads_names[] = { "Input", "Output", "Alarm" };

   void* (*thr_functions[])(void*) = { input_thread, output_thread, alarm_thread };

   pthread_t threads[NUM_THREADS];
   pthread_mutex_t mtx;
   pthread_cond_t cond;
   pthread_mutex_init(&mtx, NULL); // initialize mutex with default attributes
   pthread_cond_init(&cond, NULL); // initialize condition variable with default attributes
   data.mtx = &mtx;                // make the mutex accessible from the shared data structure
   data.cond = &cond;              // make the cond accessible from the shared data structure

   call_termios(0);

   for (int i = 0; i < NUM_THREADS; ++i) { // create threads 
      int r = pthread_create(&threads[i], NULL, thr_functions[i], &data);
      printf("Create thread '%s' %s\r\n", threads_names[i], ( r == 0 ? "OK" : "FAIL") );
   }

   int *ex;
   for (int i = 0; i < NUM_THREADS; ++i) { // join threads so main doesnt end before threads
      printf("Call join to the thread %s\r\n", threads_names[i]);
      int r = pthread_join(threads[i], (void*)&ex);
      printf("Joining the thread %s has been %s - exit value %i\r\n", threads_names[i], (r == 0 ? "OK" : "FAIL"), *ex);
   }

   call_termios(1); // restore terminal settings
   return EXIT_SUCCESS;
}

// - function -----------------------------------------------------------------
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

// - function -----------------------------------------------------------------
void* input_thread(void* d)
{
   data_t *data = (data_t*)d;
   static int r = 0;
   int c;
   bool qq = false;
   while((!qq)){ // until pipe isnt open - dont do anything
      pthread_mutex_lock(data->mtx); 
      qq = data->is_serial_open;
      pthread_mutex_unlock(data->mtx);
   }
   while ((c = getchar()) != 'q') {
      pthread_mutex_lock(data->mtx);
      int period = data->alarm_period;
      switch (c) {
         case 'r':
            period -= PERIOD_STEP;
            if (period < PERIOD_MIN) {
               period = PERIOD_MIN;
            }
            break;
         case 'p':
            period += PERIOD_STEP;
            if (period > PERIOD_MAX) {
               period = PERIOD_MAX;
            }
            break;
      }
      if (data->alarm_period != period) {
         pthread_cond_signal(data->cond); // signal the output thread to refresh 
      }
      data->alarm_period = period;
      pthread_mutex_unlock(data->mtx);
   }
   r = 1;
   pthread_mutex_lock(data->mtx);
   data->quit = true;
   pthread_cond_broadcast(data->cond);
   pthread_mutex_unlock(data->mtx);
   fprintf(stderr, "Exit input thread %lu\r\n", (unsigned long)pthread_self());
   return &r;
}

// - function -----------------------------------------------------------------
void* output_thread(void* d)
{
   data_t *data = (data_t*)d;
   static int r = 0;
   bool q = false;
   data->fd = io_open_write(MY_DEVICE_OUT);
   if (data->fd == EOF) {
      fprintf(stderr, "Error: Unable to open the file %s\n", MY_DEVICE_OUT);
      exit(1);
   }
   message msg  = {.type = MSG_STARTUP, .data.startup = { .message = "Hello"}};
   send_message(data, &msg);

   if (io_putc(data->fd, 'i') != 1) { // sends init byte
      fprintf(stderr, "Error: Unable to send the init byte\n");
      exit(1);
   }
   fsync(data->fd); // sync the data
   pthread_mutex_lock(data->mtx);
   data->is_serial_open = true;

   while (!q) {
      pthread_cond_wait(data->cond, data->mtx); // wait for next event
      if (io_putc(data->fd, 'a') != 1) { // sends init byte
         fprintf(stderr, "Error: unable to send a\n");
         data->quit = true;
      }
      fsync(data->fd); // sync the data
      q = data->quit;
      printf("\rAlarm time: %10i   Alarm counter: %10i", data->alarm_period, data->alarm_counter);
      fflush(stdout);
   }
   pthread_mutex_unlock(data->mtx);

   if (io_putc(data->fd, 'q') != 1) { // sends init byte
      fprintf(stderr, "Error: Unable to send the end byte\n");
      exit(1);
   }
   fsync(data->fd); // sync the data
   io_close(data->fd);
   fprintf(stderr, "Exit output thread %lu\r\n", (unsigned long)pthread_self());
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
   static int r = 0;
   pthread_mutex_lock(data->mtx);
   bool q = data->quit;
   useconds_t period = data->alarm_period * 1000; // alarm_period is in ms
   pthread_mutex_unlock(data->mtx);

   while (!q) {
      usleep(period);
      pthread_mutex_lock(data->mtx);
      q = data->quit;
      data->alarm_counter += 1;
      period = data->alarm_period * 1000; // update the period is it has been changed
      pthread_cond_broadcast(data->cond);
      pthread_mutex_unlock(data->mtx);
   }
   fprintf(stderr, "Exit alarm thread %lu\r\n", (unsigned long)pthread_self());
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

/* end of threads.c */
