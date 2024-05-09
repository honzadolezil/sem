#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <termios.h>
#include <threads.h>
#include <unistd.h> // for STDIN_FILENO
#include <complex.h> // for julia set complex numbers

#include <pthread.h>
#include "messages.h"
#include "prg_io_nonblock.h" // send and recieves bites through pipe
#define MY_DEVICE_OUT "/tmp/pipe.out"
#define MY_DEVICE_IN "/tmp/pipe.in"


void call_termios(int reset);

#define SIZE_C_W 64
#define SIZE_C_H 48
#define NUM_CHUNKS 100

typedef struct { // shared date structure;
    int alarm_period;
    int alarm_counter;
    bool quit;
    int fd; //forwarding
    int rd;// recieving
    bool is_serial_open; // if comunication established
    bool abort;
    bool is_cond_signaled;
    bool is_message_recieved;
    pthread_mutex_t *mtx;
    pthread_cond_t *cond;


    //set compute data
    double c_re;
    double c_im;
    double d_re;
    double d_im;
    int n;


    //computation data
    uint8_t cid;
    double re;
    double im;
    uint8_t n_re;
    uint8_t n_im;



}   data_t;

void* input_thread(void*);
void* calculation_thread(void*);

message *buffer_parse(data_t *data, int message_type);
bool send_message(data_t *data, message *msg);

void compute_julia_set(data_t *data);

#define CHUNK_SIZE_W 64
#define CHUNK_SIZE_H 48

int main(int argc, char *argv[])
{
   data_t data = { .alarm_period = 0, .alarm_counter = 0, .quit = false, .fd = EOF, .is_serial_open = false, .abort = false, .is_cond_signaled = false, .cid = 0, .re = 0, .im = 0, .n_re = 0, .n_im = 0, .is_message_recieved = false, .mtx = NULL, .cond = NULL, .c_re = 0, .c_im = 0, .d_re = 0, .d_im = 0, .n = 0};

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
    //pthread_mutex_lock(data->mtx);
    while (!data->quit) {
        
        uint8_t c = '\0';
        io_getc_timeout(data->fd, 0,&c); 
        if (c == 'q'){
            data->quit = true;
            break;
        }    
        else if (c == MSG_GET_VERSION){//sends firmware info
            printf("INFO: sending version\r\n");
            //pthread_mutex_unlock(data->mtx);
            message msg  = {.type = MSG_VERSION, .data.version = {'1','2','2'}};
            if(!send_message(data,&msg))
                exit(1);
            fsync(data->rd);
           // pthread_mutex_lock(data->mtx);
        }
        else if (c == MSG_STARTUP){
            //pthread_mutex_unlock(data->mtx);
            message *msg = buffer_parse(data, MSG_STARTUP);
            printf("INFO: Startup: %s\r\n", msg->data.startup.message);
            free(msg);
            c = '\0';
            //pthread_mutex_lock(data->mtx);
        }
        else if (c == MSG_SET_COMPUTE){
            //pthread_mutex_unlock(data->mtx);
            printf("INFO: recieved set compute\r\n");
            message *msg = buffer_parse(data, MSG_SET_COMPUTE);
            data->c_re = msg->data.set_compute.c_re;
            data->c_im = msg->data.set_compute.c_im;
            data->d_re = msg->data.set_compute.d_re;
            data->d_im = msg->data.set_compute.d_im;
            data->n = msg->data.set_compute.n;


            printf("c_re = %lf, c_im = %lf, d_re = %lf, d_im = %lf, n = %d\r\n", data->c_re, data->c_im, data->d_re, data->d_im, data->n);
            c = '\0';
            free(msg);   
            //pthread_mutex_lock(data->mtx);
        }
        else if (c == MSG_COMPUTE){
            //pthread_mutex_unlock(data->mtx);
            printf("INFO: recieved compute\r\n");
            message *msg = buffer_parse(data, MSG_COMPUTE);
            data->cid = msg->data.compute.cid;
            data->re = msg->data.compute.re;
            data->im = msg->data.compute.im;
            data->n_re = msg->data.compute.n_re;
            data->n_im = msg->data.compute.n_im;         
            data->is_cond_signaled = true;
            pthread_cond_broadcast(data->cond);

            //pthread_mutex_lock(data->mtx);

            c = '\0';
            free(msg);

            
        }
        else if (c == MSG_ABORT){
            //printf("recieved end of computation\r\n");
            data->abort = true;
            fsync(data->abort);
            pthread_mutex_unlock(data->mtx);
            message *msg = buffer_parse(data, MSG_ABORT);
            free(msg);
            //pthread_mutex_lock(data->mtx);
            c = '\0';

        }
            
    }
      
    //pthread_mutex_unlock(data->mtx);
    data->quit = true;
    r = 1;
    pthread_cond_broadcast(data->cond);
    
    //pthread_mutex_unlock(data->mtx);
    fprintf(stderr, "\033[1;35mTHREAD\033[0m: Exit input thread %lu\r\n", (unsigned long)pthread_self());
    return &r;
}

void* calculation_thread(void*d){
    data_t *data = (data_t*)d;
    static int r = 1;

    bool q = false;
    pthread_mutex_lock(data->mtx);
    while(!q){
        //printf("INFO: Calculation thread is waiting\r\n");
        while (!q && !data->is_cond_signaled) {
            pthread_cond_wait(data->cond, data->mtx);
            q = data->quit;
        }
        //printf("INFO: Calculation thread is running\r\n");
        q = data->quit;
        if(data->quit){
            break;
        }

        if (!data->abort && !q) {
            // compute julia set for each chunk (64x48 pixels on 640 x 480 screen)
            // send the result back to the input thread
            double start_re = data->re;
            double start_im = data->im;
            while(!q){
                
                if(data->cid == 100){
                    printf("INFO: Calculation thread is done\r\n");
                    pthread_mutex_unlock(data->mtx);
                    message msg = {.type = MSG_DONE};
                    send_message(data, &msg);
                    fsync(data->rd);
                    pthread_mutex_lock(data->mtx);
                    break;
                }

                int chunk_width = 64;
                int chunk_height = 48;
                int chunks_per_row = 640 / chunk_width;

                int x_im = (data->cid % chunks_per_row) * chunk_width; //first chunk (real)
                int y_im = (data->cid / chunks_per_row) * chunk_height; // forst chunk (imaginary)

                printf("INFO: Chunk %d: x_im = %d, y_im = %d\r\n", data->cid, x_im, y_im);
                data->re =start_re + x_im * data->d_re;
                data->im = start_im + y_im * data->d_im;
                printf("INFO: Chunk %d: re = %lf, im = %lf\r\n", data->cid, data->re, data->im);                

                compute_julia_set(data);
                data->cid++;
                q = data->quit;
            }
        }

        data->is_cond_signaled = false;
        q = data->quit;
    }
    pthread_mutex_unlock(data->mtx);

    printf("INFO: Calculation thread is exiting\r\n");
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
    uint8_t c = 0;
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



void compute_julia_set(data_t *data) {
    
    uint8_t iter;
    double complex Z;
    double complex C = data->c_re + data->c_im * I;
    for (uint8_t x = 0; x <= CHUNK_SIZE_W; x++) { // for size of chunk 
        for (uint8_t y = 0; y <= CHUNK_SIZE_H; y++) { // for size of chunk
            Z = (data->re + x * data->d_re) + (data->im + y * data->d_im) * I; 
            iter = 0;
            while (cabs(Z) < 2 && iter < data->n) {
                Z = Z * Z + C;
                iter++;

            }
            //printf("INFO: Chunk %d: x = %d, y = %d, iter = %d\r\n", data->cid, x, y, iter);
            pthread_mutex_unlock(data->mtx);
            message msg = {.type = MSG_COMPUTE_DATA, .data.compute_data = {data->cid, x, y, iter}}; // for each pixel = x, y in given chunk
            send_message(data, &msg);
            fsync(data->rd);
            //printf("INFO: sent compute data\r\n");
            pthread_mutex_lock(data->mtx);


        }
        
    }
    printf("INFO: Chunk %d is done\r\n", data->cid);


    
}