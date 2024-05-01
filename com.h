#include "messages.h"
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


#define RD 1
#define FD 0

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


message *buffer_parse(data_t *data, int message_type);
bool send_message(data_t *data, message *msg, int type);
