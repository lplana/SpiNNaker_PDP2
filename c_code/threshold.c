// SpiNNaker API
#include "spin1_api.h"

// front-end-common
#include "common-typedefs.h"
#include <data_specification.h>
#include <simulation.h>

// mlp
#include "mlp_params.h"
#include "mlp_types.h"
#include "mlp_macros.h"
#include "mlp_externs.h"  // allows compiler to check extern types!

#include "init_t.h"
#include "comms_t.h"
#include "process_t.h"


// ------------------------------------------------------------------------
// threshold core main routines
// ------------------------------------------------------------------------
// ------------------------------------------------------------------------
// threshold core constants
// ------------------------------------------------------------------------
// list of procedures for the FORWARD phase in the output pipeline. The order is
// relevant, as the index is defined in mlp_params.h
out_proc_t const
  t_out_procs[SPINN_NUM_OUT_PROCS] =
  {
    out_logistic, out_integr, out_hard_clamp, out_weak_clamp, out_bias
  };

// list of procedures for the BACKPROP phase in the output pipeline. The order
// is relevant, as the index needs to be the same as in the FORWARD phase. In
// case one routine is not intended to be available in lens, then a NULL should
// replace the call
out_proc_back_t const
  t_out_back_procs[SPINN_NUM_OUT_PROCS] =
  {
    out_logistic_back, out_integr_back, out_hard_clamp_back, out_weak_clamp_back, out_bias_back
  };

// list of procedures for the initialisation of the output pipeline. The order
// is relevant, as the index needs to be the same as in the FORWARD phase. In
// case one routine is not intended to be available because no initialisation
// is required, then a NULL should replace the call
out_proc_init_t const
  t_init_out_procs[SPINN_NUM_OUT_PROCS] =
  {
      NULL, init_out_integr, init_out_hard_clamp, init_out_weak_clamp, NULL
  };

// list of procedures for the evaluation of the convergence (and stopping)
// criteria. The order is relevant, as the indices are specified in mlp_params.h
// A NULL routine does not evaluate any convergence criterion and therefore
// computation will continue for the defined maximum number of ticks
stop_crit_t const
  t_stop_procs[SPINN_NUM_STOP_PROCS] =
  {
    NULL, std_stop_crit, max_stop_crit
  };

// list of procedures for the evaluation of the errors between the output and
// the target values of the output groups. The order is relevant, as the indices
// are specified in mlp_params.h. A NULL routine does not evaluate any error and
// therefore the weight update will always be 0
out_error_t const
  t_out_error[SPINN_NUM_ERROR_PROCS] =
  {
    NULL, error_cross_entropy, error_squared
  };
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// global variables
// ------------------------------------------------------------------------
uint chipID;               // 16-bit (x, y) chip ID
uint coreID;               // 5-bit virtual core ID

uint fwdKey;               // 32-bit packet ID for FORWARD phase
uint bkpKey;               // 32-bit packet ID for BACKPROP phase

uint32_t stage_step;       // current stage step
uint32_t stage_num_steps;  // current stage number of steps

uint         epoch;        // current training iteration
uint         example_cnt;  // example count in epoch
uint         example_inx;  // current example index
uint         evt;          // current event in example
uint         max_evt;      // the last event reached in the current example
uint         num_events;   // number of events in current example
uint         event_idx;    // index into current event
proc_phase_t phase;        // FORWARD or BACKPROP
uint         num_ticks;    // number of ticks in current event
uint         max_ticks;    // maximum number of ticks in current event
uint         min_ticks;    // minimum number of ticks in current event
uint         tick;         // current tick in phase
uint         ev_tick;      // current tick in event
uchar        tick_stop;    // current tick stop decision

uint         to_epoch   = 0;
uint         to_example = 0;
uint         to_tick    = 0;
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// data structures in regions of SDRAM
// ------------------------------------------------------------------------
mlp_set_t        * es;     // example set data
mlp_example_t    * ex;     // example data
mlp_event_t      * ev;     // event data
activation_t     * it;     // example inputs
activation_t     * tt;     // example targets
uint             * rt;     // multicast routing keys data
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// network, core and stage configurations (DTCM)
// ------------------------------------------------------------------------
network_conf_t ncfg;           // network-wide configuration parameters
t_conf_t       tcfg;           // threshold core configuration parameters
stage_conf_t   xcfg;           // stage configuration parameters
address_t      xadr;           // stage configuration SDRAM address
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// threshold core variables
// ------------------------------------------------------------------------
// threshold cores compute unit outputs and error deltas.
// ------------------------------------------------------------------------
activation_t   * t_outputs;         // current tick unit outputs
net_t          * t_nets;            // nets received from input cores
error_t        * t_errors[2];       // error banks: current and next tick
activation_t   * t_last_integr_output;  //last INTEGRATOR output value
long_deriv_t   * t_last_integr_output_deriv; //last INTEGRATOR output deriv value
activation_t   * t_instant_outputs; // current output value stored for the backward pass
short_activ_t  * t_out_hard_clamp_data; //values injected by hard clamps
short_activ_t  * t_out_weak_clamp_data; //values injected by weak clamps
uint             t_it_idx;          // index into current inputs/targets
uint             t_tot_ticks;       // total ticks on current example
pkt_queue_t      t_net_pkt_q;       // queue to hold received nets
uchar            t_active;          // processing nets/errors from queue?
scoreboard_t     t_sync_arrived;    // keep track of expected sync packets
uchar            t_sync_rdy;        // have expected sync packets arrived?
sdp_msg_t        t_sdp_msg;         // SDP message buffer for host comms.

// FORWARD phase specific
// (output computation)
scoreboard_t     tf_arrived;        // keep track of expected nets
uint             tf_thrds_pend;     // sync. semaphore: proc & stop
uchar            tf_chain_prev;     // previous daisy chain (DC) value
uchar            tf_initChain;      // previous DC received init value
uchar            tf_chain_rdy;      // local DC value can be forwarded
uchar            tf_stop_crit;      // stop criterion met?
uchar            tf_group_crit;     // stop criterion met for all groups?
uchar            tf_event_crit;     // stop criterion met for all events?
uchar            tf_example_crit;   // stop criterion met for all examples?
error_t          t_group_criterion; // convergence criterion value
stop_crit_t      tf_stop_func;      // stop evaluation function
uint             tf_stop_key;       // stop criterion packet key
uint             tf_stpn_key;       // stop network packet key

// BACKPROP phase specific
// (error delta computation)
uint             tb_procs;          // pointer to processing errors
uint             tb_comms;          // pointer to receiving errors
scoreboard_t     tb_arrived;        // keep track of expected errors
uint             tb_thrds_pend;     // sync. semaphore: proc & stop

int              t_max_output_unit; // unit with highest output
int              t_max_target_unit; // unit with highest target
activation_t     t_max_output;      // highest output value
activation_t     t_max_target;      // highest target value

long_deriv_t   * t_output_deriv;    // derivative of the output value
delta_t        * t_deltas;

uint           * t_fwdKey;          // t cores have one fwdKey per partition

// history arrays
net_t          * t_net_history;
activation_t   * t_output_history;
activation_t   * t_target_history;
long_deriv_t   * t_output_deriv_history;
// ------------------------------------------------------------------------


#ifdef DEBUG
// ------------------------------------------------------------------------
// DEBUG variables
// ------------------------------------------------------------------------
uint pkt_sent;  // total packets sent
uint sent_fwd;  // packets sent in FORWARD phase
uint sent_bkp;  // packets sent in BACKPROP phase
uint pkt_recv;  // total packets received
uint recv_fwd;  // packets received in FORWARD phase
uint recv_bkp;  // packets received in BACKPROP phase
uint spk_sent;  // sync packets sent
uint spk_recv;  // sync packets received
uint chn_sent;  // chain packets sent
uint chn_recv;  // chain packets received
uint stp_sent;  // stop packets sent
uint stp_recv;  // stop packets received
uint stn_sent;  // network_stop packets sent
uint stn_recv;  // network_stop packets received
uint wrng_phs;  // packets received in wrong phase
uint wrng_tck;  // FORWARD packets received in wrong tick
uint wrng_btk;  // BACKPROP packets received in wrong tick
uint tot_tick;  // total number of ticks executed
// ------------------------------------------------------------------------
#endif


// ------------------------------------------------------------------------
// timer callback: check that there has been progress in execution.
// If no progress has been made terminate with SPINN_TIMEOUT_EXIT code.
// ------------------------------------------------------------------------
void timeout (uint ticks, uint unused)
{
  (void) ticks;
  (void) unused;

  // check if progress has been made
  if ((to_epoch == epoch) && (to_example == example_cnt) && (to_tick == tick))
  {
    // report timeout error
    stage_done (SPINN_TIMEOUT_EXIT);
  }
  else
  {
    // update checked variables
    to_epoch   = epoch;
    to_example = example_cnt;
    to_tick    = tick;
  }
}
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// kick start simulation
//NOTE: workaround for an FEC bug
// ------------------------------------------------------------------------
void get_started (void)
{
  // start timer,
  vic[VIC_ENABLE] = (1 << TIMER1_INT);
  tc[T1_CONTROL] = 0xe2;

  // redefine start function,
  simulation_set_start_function (stage_start);

  // and run new start function
  stage_start ();
}
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// main: register callbacks and initialise basic system variables
// ------------------------------------------------------------------------
void c_main ()
{
  // say hello,
  io_printf (IO_BUF, ">> mlp\n");

  // get core IDs,
  chipID = spin1_get_chip_id ();
  coreID = spin1_get_core_id ();

  // initialise configurations from SDRAM,
  uint exit_code = cfg_init ();
  if (exit_code != SPINN_NO_ERROR)
  {
    // report results and abort
    stage_done (exit_code);
  }

  // allocate memory in DTCM and SDRAM,
  exit_code = mem_init ();
  if (exit_code != SPINN_NO_ERROR)
  {
    // report results and abort
    stage_done (exit_code);
  }

  // initialise variables,
  var_init ();

  // set up timer1 (used for background deadlock check),
  spin1_set_timer_tick (SPINN_TIMER_TICK_PERIOD);
  spin1_callback_on (TIMER_TICK, timeout, SPINN_TIMER_P);

  // set up packet received callbacks,
  spin1_callback_on (MC_PACKET_RECEIVED, t_receivePacket, SPINN_PACKET_P);
  spin1_callback_on (MCPL_PACKET_RECEIVED, t_receivePacket, SPINN_PACKET_P);

  // setup simulation,
  simulation_set_start_function (get_started);

  // start execution,
  simulation_run ();

  // and say goodbye
  io_printf (IO_BUF, "<< mlp\n");
}
// ------------------------------------------------------------------------
