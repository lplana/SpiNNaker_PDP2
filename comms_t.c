// SpiNNaker API
#include "spin1_api.h"

// mlp
#include "mlp_params.h"
#include "mlp_types.h"
#include "sdram.h"

#include "comms_t.h"
#include "process_t.h" 

// this files contains the communication routines used by T cores

// ------------------------------------------------------------------------
// global variables
// ------------------------------------------------------------------------
extern uint coreID;               // 5-bit virtual core ID
extern uint coreKey;              // 21-bit core packet ID
extern uint bkpKey;               // 32-bit packet ID for backprop passes
extern uint stpKey;               // 32-bit packet ID for stop criterion

extern uint         epoch;        // current training iteration
extern uint         example;      // current example in epoch
extern uint         evt;          // current event in example
extern uint         num_ticks;    // number of ticks in current event
extern proc_phase_t phase;        // FORWARD or BACKPROP
extern uint         tick;         // current tick in phase
extern uchar        tick_stop;    // current tick stop decision

// ------------------------------------------------------------------------
// configuration structures (SDRAM)
// ------------------------------------------------------------------------
extern chip_struct_t        *ct; // chip-specific data
extern uint                 *cm; // simulation core map
extern uchar                *dt; // core-specific data
extern mc_table_entry_t     *rt; // multicast routing table data
extern weight_t             *wt; //# initial connection weights
extern mlp_set_t            *es; // example set data
extern mlp_example_t        *ex; // example data
extern mlp_event_t          *ev; // event data
extern activation_t         *it; // example inputs
extern activation_t         *tt; // example targets
// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
// network and core configurations
// ------------------------------------------------------------------------
extern global_conf_t  mlpc;       // network-wide configuration parameters
extern chip_struct_t  ccfg;       // chip configuration parameters
extern t_conf_t       tcfg;       // threshold core configuration parameters
// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
// threshold core variables
// ------------------------------------------------------------------------
extern activation_t   * t_outputs;     // current tick unit outputs
extern net_t          * t_nets;        // nets received from sum cores
extern error_t        * t_errors;      // current tick errors
extern uint             t_it_idx;      // index into current inputs/targets
extern uint             t_tot_ticks;   // total ticks on current example
extern pkt_queue_t      t_net_pkt_q;   // queue to hold received nets
extern pkt_queue_t      t_err_pkt_q;   // queue to hold received errors
extern uchar            t_active;      // processing nets/errors from queue?
extern scoreboard_t     t_sync_arr;    // keep track of expected sync packets
extern uchar            t_sync_done;   // have expected sync packets arrived?
extern sdp_msg_t        t_sdp_msg;     // SDP message buffer for host comms.
extern uint             tf_thrds_init; // sync. semaphore initial value
extern uint             tf_thrds_done; // sync. semaphore: proc & stop
extern uchar            tf_stop_prev;  // previous group stop criterion met?
extern uchar            tf_stop_crit;  // stop criterion met?
extern uchar            tf_stop_init;  // sync. semaphore: stop daisy chain
extern uchar            tf_stop_done;  // sync. semaphore: stop daisy chain
extern stop_crit_t      tf_stop_func;  // stop evaluation function
extern uint             tf_stop_key;   // stop criterion packet key
//#extern uint             tb_thrds_done; // sync. semaphore: proc & stop
// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
// DEBUG variables
// ------------------------------------------------------------------------
#ifdef DEBUG
  extern uint pkt_sent;  // total packets sent
  extern uint sent_bkp;  // packets sent in BACKPROP phase
  extern uint pkt_recv;  // total packets received
  extern uint recv_fwd;  // packets received in FORWARD phase
  extern uint recv_bkp;  // packets received in BACKPROP phase
  extern uint spk_sent;  // sync packets sent
  extern uint spk_recv;  // sync packets received
  extern uint stp_sent;  // stop packets sent
  extern uint stp_recv;  // stop packets received
  extern uint wrng_phs;  // packets received in wrong phase
  extern uint wrng_tck;  // FORWARD packets received in wrong tick
  extern uint wrng_btk;  // BACKPROP packets received in wrong tick
#endif

// ------------------------------------------------------------------------
// code
// ------------------------------------------------------------------------

// callback routine for a multicast packet received
void t_receivePacket (uint key, uint payload)
{
  // get packet phase
  uint ph = (key & SPINN_PHASE_MASK) >> SPINN_PHASE_SHIFT;

  // check packet type
  if ((key & SPINN_STOP_MASK) == SPINN_STPR_KEY)
  {
    #ifdef DEBUG
      stp_recv++;
    #endif

    // STOP decision arrived
    tick_stop = (key & SPINN_STPD_MASK) >> SPINN_STPD_SHIFT;

    #ifdef DEBUG_VRB
      io_printf (IO_BUF, "sc:%x\n", tick_stop);
    #endif

    // check if all threads done
    if (tf_thrds_done == 0)
    {
      // initialize semaphore
      tf_thrds_done = tf_thrds_init;

      // and advance tick
      spin1_schedule_callback (tf_advance_tick, NULL, NULL, SPINN_T_TICK_P);
    }
    else
    {
      // if not done report processing thread done
      tf_thrds_done -= 1;
    }
  }
  else if ((key & SPINN_STOP_MASK) == SPINN_STPF_KEY)
  {
    #ifdef DEBUG
      stp_recv++;
    #endif

    // STOP daisy chain partial decision arrived
    if (tf_stop_done == 0)
    {
      // initialize semaphore,
      tf_stop_done = tf_stop_init;

      // send stop criterion packet,
      spin1_schedule_callback (tf_send_stop, NULL, NULL, SPINN_SEND_STOP_P);

      // and check if all threads done -- last group does not get a decision!
      if (tcfg.is_last_output_group)
      {
        if (tf_thrds_done == 0)
        {
          // initialize semaphore,
          tf_thrds_done = tf_thrds_init;
    
          // and advance tick
          spin1_schedule_callback (tf_advance_tick, NULL, NULL, SPINN_T_TICK_P);
        }
        else
        {
          // if not done report stop thread done
          tf_thrds_done -= 1;
        }
      }
    }
    else
    {
      // if not done report processing thread done
      tf_stop_done -= 1;
    }
  }
  else if (key & SPINN_SYNC_MASK)
  {
    // sync packet received
    #ifdef DEBUG
      spk_recv++;
    #endif

    t_syncPacket (key, ph);
  }
  else if (ph == SPINN_FORWARD)
  {
    // FORWARD phase packet
    t_forwardPacket (key, payload);
  }
  else
  {
    // BACKPROP phase packet
    t_backpropPacket (key, payload);
  }
}

// routine processing a forward multicast packet
void t_forwardPacket (uint key, uint payload)
{
  #ifdef DEBUG
    pkt_recv++;
  #endif

  // check if space in FORWARD packet queue,
  uint new_tail = (t_net_pkt_q.tail + 1) % SPINN_PKT_QUEUE_LEN;

  if (new_tail == t_net_pkt_q.head)
  {
    // if queue full exit and report failure
    spin1_kill (SPINN_QUEUE_FULL);
  }
  else
  {
    // if not full queue packet,
    t_net_pkt_q.queue[t_net_pkt_q.tail].key = key;
    t_net_pkt_q.queue[t_net_pkt_q.tail].payload = payload;
    t_net_pkt_q.tail = new_tail;
  
    // and schedule processing thread 
    // if in FORWARD phase and not active already
    if ((phase == SPINN_FORWARD) && (!t_active))
    {
      t_active = TRUE;
      spin1_schedule_callback (tf_process, NULL, NULL, SPINN_TF_PROCESS_P);
    }
  }
}

// routine processing a backward multicast packet
void t_backpropPacket (uint key, uint payload)
{
  #ifdef DEBUG
    pkt_recv++;
  #endif

  // check if space in BACKPROP packet queue,
  uint new_tail = (t_err_pkt_q.tail + 1) % SPINN_PKT_QUEUE_LEN;

  if (new_tail == t_err_pkt_q.head)
  {
    // if queue full exit and report failure
    spin1_kill (SPINN_QUEUE_FULL);
  }
  else
  {
    // if not full queue packet,
    t_err_pkt_q.queue[t_err_pkt_q.tail].key = key;
    t_err_pkt_q.queue[t_err_pkt_q.tail].payload = payload;
    t_err_pkt_q.tail = new_tail;
  
    // and schedule processing thread 
    // if in BACKPROP phase and not active already
    if ((phase == SPINN_BACKPROP) && (!t_active))
    {
      t_active = TRUE;
      spin1_schedule_callback (tb_process, NULL, NULL, SPINN_TB_PROCESS_P);
    }
  }
}

// routine processing a sync multicast packet
void t_syncPacket (uint key, uint ph)
{
  if (ph == SPINN_FORWARD)
  {
    // keep track of arrived blocks,
    #if SPINN_USE_COUNTER_SB == FALSE
      // get sync block
      uint blk = (key & SPINN_BLK_C_MASK) >> SPINN_BLK_C_SHIFT;
    
      t_sync_arr |= (1 << blk);
    #else
      t_sync_arr++;
    #endif
    
    // and check if all expected packets arrived
    if (t_sync_arr == tcfg.f_s_all_arr)
    {
      // initialize for next synchronization,
      t_sync_arr = 0;
    
      // and check if can trigger sending data
      if (phase == SPINN_FORWARD)
      {
        // schedule sending of unit outputs
        spin1_schedule_callback (t_init_outputs, 0, 0, SPINN_T_INIT_OUT_P);

        // and, if required, send outputs to host
        if (tcfg.write_out)
        {
          spin1_schedule_callback (send_outputs_to_host,
                                    SPINN_HOST_NORMAL, 0, SPINN_SEND_OUTS_P
                                  );
        }
      }
      else 
      { 
        // if not ready flag sync done
        t_sync_done = TRUE;
      }
    }
  }
  else
  {
    // keep track of arrived blocks,
    #if SPINN_USE_COUNTER_SB == FALSE
      // get sync block
      uint blk = (key & SPINN_BLK_R_MASK) >> SPINN_BLK_R_SHIFT;
    
      t_sync_arr |= (1 << blk);
    #else
      t_sync_arr++;
    #endif
    
    // and check if all expected packets arrived,
    if (t_sync_arr == tcfg.b_s_all_arr)
    {
      // initialize for next synchronization,
      t_sync_arr = 0;
    
      // check if can trigger sending data
      if (phase == SPINN_BACKPROP)
      {
        // schedule sending of deltas
        spin1_schedule_callback (t_init_deltas, 0, 0, SPINN_SEND_DELTAS_P);
      }
      else 
      { 
        // if not ready flag sync done
        t_sync_done = TRUE;
      }
    }
  }
}

// this routine sends the relevant data to the host through an SDP message
// TODO: all outputs may not fit in one SDP message!
void send_outputs_to_host (uint cmd, uint tick)
{
  int le;
  le = (tick == 0) ? -1 : (int) evt;

  // report epoch, example and tick,
  t_sdp_msg.cmd_rc = cmd;
  t_sdp_msg.seq    = tcfg.write_blk;
  t_sdp_msg.arg1   = epoch;
  t_sdp_msg.arg2   = (le << 16) | example;
  t_sdp_msg.arg3   = tick;

  // copy outputs and targets into msg buffer,
  activation_t * my_data = (activation_t *) t_sdp_msg.data;
  for (uint i = 0; i < tcfg.num_outputs; i++)
  {
    my_data[2 * i]     = t_outputs[i];
    my_data[2 * i + 1] = tt[t_it_idx + i];
  }

  // set message length,
  uint len = 2 * tcfg.num_outputs * sizeof(activation_t);
  t_sdp_msg.length = sizeof (sdp_hdr_t) + sizeof (cmd_hdr_t) + len;

  // and send message
  while (!spin1_send_sdp_msg (&t_sdp_msg, SPINN_SDP_TMOUT))
    io_printf (IO_STD, "sdp!\n");
}

// send an sdp packet to the host with information related to
// various parameters of the simulation: id of the output group sending the
// data, number of output units, number of units writing outputs an dnumber of
// ticks of simulation
void send_info_to_host (uint null0, uint null1)
{
  // send initial info to host
  // report epoch, example and tick,
  t_sdp_msg.cmd_rc = SPINN_HOST_INFO;
  t_sdp_msg.seq    = tcfg.write_blk;
  t_sdp_msg.arg1   = tcfg.num_outputs;
  t_sdp_msg.arg2   = ccfg.num_write_blks;
  t_sdp_msg.arg3   = t_tot_ticks + 1;

  // set message length,
  t_sdp_msg.length = sizeof (sdp_hdr_t) + sizeof (cmd_hdr_t);

  // and send message
  while (!spin1_send_sdp_msg (&t_sdp_msg, SPINN_SDP_TMOUT));

  #ifdef DEBUG_VRB
    io_printf (IO_BUF, "sent info to host: nb:%d wb:%d no:%d tt:%d\n",
                ccfg.num_write_blks, tcfg.write_blk,
                tcfg.num_outputs, t_tot_ticks
              );
  #endif
}
