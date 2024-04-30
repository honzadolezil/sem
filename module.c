#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <string.h>
#include <termios.h>
#include <unistd.h> // for STDIN_FILENO

#include <pthread.h>
#include "messages.h"
#include "prg_io_nonblock.h" // send and recieves bites through pipe
#include "messages.h"
#define MY_DEVICE_OUT "/tmp/pipe.out"
#define MY_DEVICE_IN "/tmp/pipe.in"

typedef struct { // shared date structure;
    int alarm_period;
    int alarm_counter;
    bool quit;
    int fd; //forwarding
    int rd;// recieving
    bool is_serial_open; // if comunication established
    pthread_mutex_t *mtx;
    pthread_cond_t *cond;
}   data_t;

bool send_message(data_t *data, message *msg);

int main(){
    data_t *data = (data_t*)malloc(sizeof(data_t));
    if (data == NULL) {
        fprintf(stderr, "Error: Unable to allocate memory\n");
        exit(1);
    }
    pthread_mutex_t mtx;
    pthread_mutex_init(&mtx, NULL); // initialize mutex with default attributes
    data->mtx = &mtx;                // make the mutex accessible from the shared data structure
      
    
    data->fd = io_open_read(MY_DEVICE_OUT); // opens a named pipe
    if (data->fd == EOF){
        fprintf(stderr, "Error: Unable to open the file %s\n", MY_DEVICE_OUT);
        exit(1); // not coding style but whatever
    }
    data->rd= io_open_write(MY_DEVICE_IN);
    if (data->rd == EOF) {
      fprintf(stderr, "Error: Unable to open the file %s\n", MY_DEVICE_IN);
      exit(1);
   }
    // here comes comunications
    while(!data->quit){
        uint8_t c;
        int idx = 0, len = 0;
        uint8_t msg_buf[sizeof(message)];
        int r = io_getc_timeout(data->fd, 0,&c); 
        if (c == MSG_GET_VERSION){//sends firmware info
            printf("sending version\n");
            message msg  = {.type = MSG_VERSION, .data.version = {'1','p','3'}};
            send_message(data,&msg);
            fsync(data->rd);
        }
        if (c == MSG_SET_COMPUTE){
            printf("recieved set compute\n");
            //float c_re, c_im, d_re, d_im, n;
            int i = 0;
            msg_buf[i++] = MSG_SET_COMPUTE;
            while((io_getc_timeout(data->fd, 100,&c) == 1)){
                msg_buf[i++] = c;
            }
            message *msg = malloc(sizeof(message));
            msg->type = MSG_SET_COMPUTE;
            get_message_size(MSG_SET_COMPUTE, &len);
            if(!parse_message_buf(msg_buf, len, msg)){
                fprintf(stderr, "Error: Unable to parse the message\n");
                free(msg);
                exit(1);
            } // TODO
            printf("c_re = %lf, c_im = %lf, d_re = %lf, d_im = %lf, n = %d\n", msg->data.set_compute.c_re, msg->data.set_compute.c_im, msg->data.set_compute.d_re, msg->data.set_compute.d_im, msg->data.set_compute.n);
            printf("message size = %d\r\n", len); 
        
            free(msg);
        }
        if (c == 'q'){
            data->quit = false;
            break;
        }
        else
        if (r == 1){
            if(idx == 0){
                if (get_message_size(data->fd, &len)){
                    
                    msg_buf[idx++] = c; // first byte and check if it is a valid message
                    
                    //fprintf(stdout, "message is %c\n", c);
                    

                }
                else{
                    fprintf(stderr, "Error: Unknown message type %c\n", c);
                }
            }else{
                msg_buf[idx++] = c;
            }
            
            if(len != 0 && idx == len){
                message *msg = malloc(sizeof(message));
                if (msg == NULL){
                    fprintf(stderr, "Error: Unable to allocate memory\n");
                    exit(1);
                }
                if(parse_message_buf(msg_buf, len, msg)){
                    
                    // do something - save messages into buffer
                    for (size_t i = 0; i < len; i++){
                        printf("%c", msg_buf[i]);
                    }
                    printf("\n");
                    fflush(stdout);
                }
                else{
                    fprintf(stderr, "Error: Unable to parse the message\n");
                    free(msg);
                }
            idx = len = 0;
            data->quit = true;
            }
        }
        else{
         // do nothing   
        }
    }





    io_close(data->fd); // closes the file named pipe 
    io_close(data->rd);
    return 0;
}

bool send_message(data_t *data, message *msg){
   uint8_t msg_buf[sizeof(message)];
   int size;
   fill_message_buf(msg, msg_buf,sizeof(message), &size);
   pthread_mutex_lock(data->mtx);
   int ret = write(data->rd, msg_buf, size);
   pthread_mutex_unlock(data->mtx);
   return size == ret;
}