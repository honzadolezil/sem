#include "computation.h"
#include "utils.h"

struct{
    double c_re;
    double c_im;
    int n;
    double range_re_min;
    double range_re_max;
    double range_im_min;
    double range_im_max;
    int grid_w;
    int grid_h;
    int cur_x;
    int cur_y;
    double d_re;
    double d_im;
    int nbr_chunks;
    int cid;
    double chunk_re;
    double chunk_im;
    uint8_t chunk_n_re;
    uint8_t chunk_n_im;
    uint8_t *grid;
    bool computing;
    bool done;
} comp = {
    .c_re = -0.4,
    .c_im = 0.6,
    .n = 60,
    .range_re_min = -1.6,
    .range_re_max = 1.6,
    .range_im_min = -1.1,
    .range_im_max = 1.1,
    .grid = NULL,
    .grid_w = 640,
    .grid_h = 480,
    .chunk_n_re = 64,
    .chunk_n_im = 48,
    .computing = false,
    .done = false
};

void computation_init(void){
    comp.grid = my_alloc(comp.grid_w * comp.grid_h);
    comp.d_re =  (comp.range_re_max - comp.range_re_min) / (1. * comp.grid_w);
    comp.d_im =  -(comp.range_im_max - comp.range_im_min) / (1. * comp.grid_h);
    comp.nbr_chunks = (comp.grid_h * comp.grid_w) / (comp.chunk_n_re * comp.chunk_n_im);
}
void computation_cleanup(void){
    if (comp.grid){
        free(comp.grid);
    }
    comp.grid = NULL;
}

void get_grid_size(int *w, int *h){
    *w = comp.grid_w;
    *h = comp.grid_h;
}

bool is_computing(void){
    return comp.computing;
}

bool is_done(void){
    return comp.done;
}

void abort_comp(void){
    comp.computing = false;
}

bool set_compute(message *msg){
    my_assert(msg != NULL, __func__, __LINE__, __FILE__);
    bool ret = !is_computing();
    if (ret){
        msg->type = MSG_SET_COMPUTE;
        msg->data.set_compute.c_re = comp.c_re;
        msg->data.set_compute.c_im = comp.c_im;
        msg->data.set_compute.d_re = comp.d_re;
        msg->data.set_compute.d_im = comp.d_im;
        msg->data.set_compute.n = comp.n;
        comp.done = false;
    }
    return ret;
}

bool compute(message *msg){
    my_assert(msg != NULL, __func__, __LINE__, __FILE__);
    if (!is_computing()){
        comp.cid = 0;
        comp.computing = true;
        comp.cur_x = comp.cur_y = 0;
        comp.chunk_re = comp.range_re_min;
        comp.chunk_im = comp.range_im_max;
        msg->type = MSG_COMPUTE;
    }
    if (comp.computing && msg->type == MSG_COMPUTE){
       msg->data.compute.cid = comp.cid;
       msg->data.compute.re = comp.chunk_re;
       msg->data.compute.im = comp.chunk_im;
       msg->data.compute.n_re = comp.chunk_n_re;
       msg->data.compute.n_im = comp.chunk_n_im;
    }
    return is_computing();
} 

void update_image(int w, int h, unsigned char *img){
    my_assert(img && comp.grid && w == comp.grid_w && h == comp.grid_h, __func__, __LINE__, __FILE__);
    /////////to do/////////////////
}

void update_data(const msg_compute_data *compute_data){
    my_assert(compute_data != NULL, __func__, __LINE__, __FILE__);
    if (compute_data->cid == comp.cid){
        const int idx = comp.cur_x + compute_data->i_re + (comp.cur_y + compute_data->i_im) * comp.grid_w;
        if (idx >= 0 && idx < (comp.grid_w * comp.grid_h)){
            comp.grid[idx] = compute_data->iter;
        }
    }
    else{
        warn("Received chunk with unexpected chunk id (cid)");
    }
}