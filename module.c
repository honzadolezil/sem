#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <termios.h>
#include <threads.h>
#include <unistd.h> // for STDIN_FILENO

#include <pthread.h>
#include "messages.h"
#include "prg_io_nonblock.h" // send and recieves bites through pipe
#define MY_DEVICE_OUT "/tmp/pipe.out"
#define MY_DEVICE_IN "/tmp/pipe.in"


void call_termios(int reset);


typedef struct { // shared date structure;
    int alarm_period;
    int alarm_counter;
    bool quit;
    int fd; //forwarding
    int rd;// recieving
    bool is_serial_open; // if comunication established
    bool abort;
    pthread_mutex_t *mtx;
    pthread_cond_t *cond;
}   data_t;

void* input_thread(void*);
void* calculation_thread(void*);

message *buffer_parse(data_t *data, int message_type);
bool send_message(data_t *data, message *msg);

int main(int argc, char *argv[])
{
   data_t data = { .alarm_period = 0, .alarm_counter = 0, .quit = false, .fd = EOF, .is_serial_open = false, .abort = false,};

   enum { INPUT, CALCULATION, NUM_THREADS };
   const char *threads_names[] = { "Input", "Calculation",};

   void* (*thr_functions[])(void*) = { input_thread, calculation_thread};

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
      printf("\033[1;35mTHREAD\033[0m: Create thread '%s' %s\r\n", threads_names[i], ( r == 0 ? "OK" : "FAIL") );
   }

   int *ex;
   for (int i = 0; i < NUM_THREADS; ++i) { // join threads so main doesnt end before threads
      printf("\033[1;35mTHREAD\033[0m: Call join to the thread %s\r\n", threads_names[i]);
      int r = pthread_join(threads[i], (void*)&ex);
      printf("\033[1;35mTHREAD\033[0m: Joining the thread %s has been %s - exit value %i\r\n", threads_names[i], (r == 0 ? "OK" : "FAIL"), *ex);
   }

   call_termios(1); // restore terminal settings
   return EXIT_SUCCESS;
}



void* input_thread(void* d)
{
    data_t *data = (data_t*)d;
    static int r = 0;
    // open comunication pipes
    data->fd = io_open_read(MY_DEVICE_OUT); // opens a named pipe
    if (data->fd == EOF){
        fprintf(stderr, "Error: Unable to open the file %s\r\n", MY_DEVICE_OUT);
        exit(1); // not coding style but whatever
    }
    data->rd= io_open_write(MY_DEVICE_IN);
    if (data->rd == EOF) {
      fprintf(stderr, "Error: Unable to open the file %s\r\n", MY_DEVICE_IN);
      exit(1);
    }

    // wait for recieving startup message
    while(!data->quit){ 
        uint8_t c;
        io_getc_timeout(data->fd, 0,&c); 
        if (c == MSG_STARTUP){
            message *msg = buffer_parse(data, MSG_STARTUP);
            printf("INFO: Startup: %s\r\n", msg->data.startup.message);
            free(msg);
            c = '\0';
            break;
        }
        else if (c == 'q'){
            data->quit = true;
            break;
        }
    }
    printf("INFO: Startup message recieved\r\n");
    pthread_mutex_lock(data->mtx);
    while (!data->quit) {
        
        uint8_t c;
        io_getc_timeout(data->fd, 0,&c); 
        if (c == 'q'){
            break;
        }    
        else if (c == MSG_GET_VERSION){//sends firmware info
            printf("INFO: sending version\r\n");
            pthread_mutex_unlock(data->mtx);
            message msg  = {.type = MSG_VERSION, .data.version = {'1','2','2'}};
            if(!send_message(data,&msg))
                exit(1);
            fsync(data->rd);
            pthread_mutex_lock(data->mtx);
        }
        else if (c == MSG_STARTUP){
            pthread_mutex_unlock(data->mtx);
            message *msg = buffer_parse(data, MSG_STARTUP);
            printf("INFO: Startup: %s\r\n", msg->data.startup.message);
            free(msg);
            c = '\0';
            pthread_mutex_lock(data->mtx);
        }
        else if (c == MSG_SET_COMPUTE){
            pthread_mutex_unlock(data->mtx);
            printf("INFO: recieved set compute\r\n");
            message *msg = buffer_parse(data, MSG_SET_COMPUTE);
            double c_re = msg->data.set_compute.c_re;
            double c_im = msg->data.set_compute.c_im;
            double d_re = msg->data.set_compute.d_re;
            double d_im = msg->data.set_compute.d_im;
            int n = msg->data.set_compute.n;
            printf("c_re = %lf, c_im = %lf, d_re = %lf, d_im = %lf, n = %d\r\n", c_re, c_im, d_re, d_im, n);
            c = '\0';
            free(msg);   
            pthread_mutex_lock(data->mtx);
        }
        else if (c == MSG_COMPUTE){
            pthread_mutex_unlock(data->mtx);
            printf("INFO: recieved computation\r\n");
            message *msg = buffer_parse(data, MSG_COMPUTE);
            uint8_t cid = msg->data.compute.cid;
            double re  = msg->data.compute.re;
            double im  = msg->data.compute.im;
            uint8_t n_re = msg->data.compute.n_re;
            uint8_t n_im = msg->data.compute.n_im;
            printf("cid = %d, re = %lf, im = %lf, n_re = %d, n_im = %d\r\n", cid, re, im, n_re, n_im);
            c = '\0';  
            free(msg);
            pthread_mutex_lock(data->mtx);
        }
            
    }
      
    pthread_mutex_unlock(data->mtx);
    data->quit = true;
    r = 1;
    pthread_mutex_lock(data->mtx);
    pthread_cond_broadcast(data->cond);
    pthread_mutex_unlock(data->mtx);
    fprintf(stderr, "\033[1;35mTHREAD\033[0m: Exit input thread %lu\r\n", (unsigned long)pthread_self());
    return &r;
}

void* calculation_thread(void*){
    static int r = 1;
    return &r;
}

bool send_message(data_t *data, message *msg){
   uint8_t msg_buf[sizeof(message)];
   int size;
   fill_message_buf(msg, msg_buf,sizeof(message), &size);
   pthread_mutex_lock(data->mtx);
   int ret = write(data->rd, msg_buf, size);
   pthread_mutex_unlock(data->mtx);
   if(size != ret){
        exit(1);
   }
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
        io_getc_timeout(data->fd, 0, &c);
        msg_buf[i++] = c;
    }
    message *msg = malloc(sizeof(message));
    if(msg == NULL){
        fprintf(stderr, "ERROR: Unable to allocate memory\r\n");
        exit(1);
    }
    msg->type = message_type;
    get_message_size(message_type, &len);
    if(!parse_message_buf(msg_buf, len, msg)){
        fprintf(stderr, "ERROR: Unable to parse the message\r\n");
        message msg2  = {.type = MSG_ERROR};
        send_message(data,&msg2);
        fsync(data->rd);
        free(msg);  
        exit(1);
    } 
    return msg;
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