#ifndef __INIT_T_H__
#define __INIT_T_H__

uint cfg_init (void);
uint mem_init (void);
uint prc_init (void);
void var_init (void);

uint init_out_integr     (void);
uint init_out_hard_clamp (void);
uint init_out_weak_clamp (void);

void stage_init  (void);
void stage_start (void);
void stage_done  (uint ec);

#endif
 
