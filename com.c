#include "com.h"




/*parse and buffer message - alocate and create pointer to the message*/
message *buffer_parse(data_t *data, int message_type){
    printf("henlo bitch\n");
    uint8_t c;
    int len = 0;
    uint8_t msg_buf[sizeof(message)];
    int i = 0;
    msg_buf[i++] = message_type; // add the first byte 
    while((io_getc_timeout(data->fd, 10,&c) == 1)){
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


bool send_message(data_t *data, message *msg, int pipe){
   uint8_t msg_buf[sizeof(message)];
   int size;
   int ret;
   fill_message_buf(msg, msg_buf,sizeof(message), &size);
   pthread_mutex_lock(data->mtx);
   if (pipe == RD){ret = write(data->rd, msg_buf, size);}
   else if (pipe == FD){ret = write(data->rd, msg_buf, size);}
   pthread_mutex_unlock(data->mtx);
   return size == ret;
}