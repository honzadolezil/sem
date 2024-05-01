

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <fcntl.h>


#include <string.h>
#include <termios.h>
#include <unistd.h> // for STDIN_FILENO

#include <pthread.h>

#include "prg_io_nonblock.h"

#define MY_DEVICE_OUT "/tmp/pipe.out"
#define MY_DEVICE_IN "/tmp/pipe.in"

#define PERIOD_MIN 10
#define PERIOD_MAX 2000
#define PERIOD_STEP 10
#include "messages.h"

typedef struct { // shared date structure
   int alarm_period;
   int alarm_counter;
   bool quit;
   int fd; //forwarding
   int rd;// recieving
   bool is_serial_open; // if comunication established
   pthread_mutex_t *mtx;
   pthread_cond_t *cond;
} data_t;

void call_termios(int reset); // raw mode terminal

void* input_thread(void*);
void* output_thread(void*);
void* alarm_thread(void*);
bool send_message(data_t *data, message *msg);
message *buffer_parse(data_t *data, int message_type);

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
   message msg2;
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
         case 'g':
            {
               pthread_mutex_unlock(data->mtx);
               msg2 = (message){.type = MSG_GET_VERSION,};
               send_message(data, &msg2);
               fsync(data->fd); // sync the data
               pthread_mutex_lock(data->mtx);
            }
            break;
         case 's':
         {
            pthread_mutex_unlock(data->mtx);
            msg2 = (message){.type = MSG_SET_COMPUTE, .data.set_compute = { .c_re = 0.1, .c_im = 0.2, .d_re = 0.3, .d_im = 0.4, .n = 100}};
            send_message(data, &msg2);
            fsync(data->fd); // sync the data
            pthread_mutex_lock(data->mtx);
            printf("computation message sent\r\n");
         
         }
         break;
      }
      if (data->alarm_period != period) {
         pthread_cond_signal(data->cond); // signal the output thread to refresh 
      }
      data->alarm_period = period;
      pthread_mutex_unlock(data->mtx);
   }

   pthread_mutex_unlock(data->mtx);
   data->quit = true;
   r = 1;
   pthread_mutex_lock(data->mtx);
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
   data->rd = io_open_read(MY_DEVICE_IN);
   if (data->rd == EOF){
        fprintf(stderr, "Error: Unable to open the file %s\n", MY_DEVICE_IN);
        exit(1); // not coding style but whatever
    }
   message msg  = {.type = MSG_STARTUP, .data.startup = { .message = "Henlo"}};
   send_message(data, &msg);
   
   if (io_putc(data->fd, 'i') != 1) { // sends init byte
      fprintf(stderr, "Error: Unable to send the init byte\n");
      exit(1);
   }
   fsync(data->fd); // sync the data
   pthread_mutex_lock(data->mtx);
   data->is_serial_open = true;
   while (!q) { // main loop for data output
      pthread_cond_wait(data->cond, data->mtx); // wait for next event
      // here i will print output to the console
      uint8_t c;
      //int idx = 0, len = 0;
      //uint8_t msg_buf[sizeof(message)];
      
      io_getc_timeout(data->rd, 10,&c); 
      if(c == MSG_VERSION){
         //printf("Version message recieved:");
         message *msg = buffer_parse(data, MSG_VERSION);
         printf("Version: %c. %c. %c\r\n", msg->data.version.major, msg->data.version.minor, msg->data.version.patch);
         free(msg);
         c = '\0';
      }
      if(c == MSG_ERROR){
         printf("Module sent error\r\n");
      }
   q = data->quit;
   fflush(stdout);
   }
   pthread_mutex_unlock(data->mtx);

   if (io_putc(data->fd, 'q') != 1) { // sends exit byte
      fprintf(stderr, "Error: Unable to send the end byte\r\n");
      exit(1);
   }
   fsync(data->fd); // sync the data
   io_close(data->fd);
   io_close(data->rd);
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
   printf("pipe unlocked\r\n");
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
   //printf("sending\n");
   fill_message_buf(msg, msg_buf,sizeof(message), &size);
   //printf("filled");
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
    msg_buf[i++] = message_type; // add the first byte 
    while((io_getc_timeout(data->rd, 10,&c) == 1)){
        msg_buf[i++] = c;
    }
    message *msg = malloc(sizeof(message));
    msg->type = message_type;
    get_message_size(message_type, &len);
    if(!parse_message_buf(msg_buf, len, msg)){
        fprintf(stderr, "Error: Unable to parse the message\n");
        free(msg);
        exit(1);
    } 
    return msg;

}

/* end of threads.c */

