#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <termios.h>
#include <unistd.h> // for STDIN_FILENO

#include <pthread.h>
#include "messages.h"
#include "prg_io_nonblock.h" // send and recieves bites through pipe
#include "messages.h"
#define MY_DEVICE_OUT "/tmp/pipe.out"
#define MY_DEVICE_IN "/tmp/pipe.in"

typedef struct { // shared date structure;
   int alarm_counter;       
   int fd; // named pipe mkfifo /tmp/pipe.out
   int rd;
   bool quit;
} data_t;

int main(){
    data_t data = { .alarm_counter = 0, .fd = EOF ,.quit = false };
    data.fd = io_open_read(MY_DEVICE_OUT); // opens a named pipe
    if (data.fd == EOF){
        fprintf(stderr, "Error: Unable to open the file %s\n", MY_DEVICE_OUT);
        exit(1); // not coding style but whatever
    }
    data.rd= io_open_write(MY_DEVICE_IN);
    if (data.rd == EOF) {
      fprintf(stderr, "Error: Unable to open the file %s\n", MY_DEVICE_IN);
      exit(1);
   }
    // here comes comunications
    while(!data.quit){
        uint8_t c;
        int idx = 0, len = 0;
        uint8_t msg_buf[sizeof(message)];
        int r = io_getc_timeout(data.fd, 200,&c); 
        if (c == MSG_GET_VERSION){
            printf("version nextbro\n");
        }
        if (c == 'q'){
            data.quit = false;
            break;
        }
        else
        if (r == 1){
            if(idx == 0){
                if (get_message_size(data.fd, &len)){
                    
                    msg_buf[idx++] = c; // first byte and check if it is a valid message
                    
                    fprintf(stdout, "message is %c\n", c);
                    

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
            data.quit = true;
            }
        }
        else{
         // do nothing   
        }
    }





    io_close(data.fd); // closes the file named pipe 
    io_close(data.rd);
    return 0;
}