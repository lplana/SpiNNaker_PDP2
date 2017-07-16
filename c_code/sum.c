// SpiNNaker API
#include "spin1_api.h"

#include <data_specification.h>

// mlp
#include "mlp_params.h"
#include "mlp_types.h"
#include "mlp_externs.h"

#include "init_s.h"
#include "comms_s.h"

#define SPINN_EXEC_TYPE 'S'

// main methods for the S core

// ------------------------------------------------------------------------
// global variables
// ------------------------------------------------------------------------
uint chipID;               // 16-bit (x, y) chip ID
uint coreID;               // 5-bit virtual core ID
uint coreIndex;            // coreID - 1 (convenient for array indexing)

uint coreType;             // weight, sum, input or threshold

uint fwdKey;               // 32-bit packet ID for FORWARD phase
uint bkpKey;               // 32-bit packet ID for BACKPROP phase

uint         epoch;        // current training iteration
uint         example;      // current example in epoch
uint         evt;          // current event in example
uint         num_events;   // number of events in current example
uint         event_idx;    // index into current event
proc_phase_t phase;        // FORWARD or BACKPROP
uint         num_ticks;    // number of ticks in current event
uint         max_ticks;    // maximum number of ticks in current event
uint         min_ticks;    // minimum number of ticks in current event
uint         tick;         // current tick in phase
uchar        tick_stop;    // current tick stop decision

// ------------------------------------------------------------------------
// configuration structures (SDRAM)
// ------------------------------------------------------------------------
global_conf_t    *gt; // global configuration data
chip_struct_t    *ct; // chip-specific data
uchar            *dt; // core-specific data
uint             *rt; // multicast routing keys data
weight_t         *wt; // initial connection weights
mlp_set_t        *es; // example set data
mlp_example_t    *ex; // example data
mlp_event_t      *ev; // event data
activation_t     *it; // example inputs
activation_t     *tt; // example targets

// ------------------------------------------------------------------------
// network and core configurations (DTCM)
// ------------------------------------------------------------------------
global_conf_t mlpc;           // network-wide configuration parameters
chip_struct_t ccfg;           // chip configuration parameters
s_conf_t      scfg;           // sum core configuration parameters
// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
// sum core variables
// ------------------------------------------------------------------------
// sum cores compute unit nets and errors (acummulate b-d-ps).
// ------------------------------------------------------------------------
long_net_t     * s_nets[2];         // unit nets computed in current tick
long_error_t   * s_errors[2];       // errors computed in current tick
long_error_t   * s_init_err[2];     // errors computed in initial tick
pkt_queue_t      s_pkt_queue;       // queue to hold received b-d-ps
uchar            s_active;          // processing b-d-ps from queue?

// FORWARD phase specific
// (net computation)
scoreboard_t   * sf_arrived[2];     // keep track of expected net b-d-p
scoreboard_t     sf_done;           // current tick net computation done
uint             sf_thrds_done;     // sync. semaphore: proc & stop

// BACKPROP phase specific
// (error computation)
long_error_t   * sb_init_error;     // initial error value for every tick
scoreboard_t     sb_all_arrived;    // all deltas have arrived in tick
scoreboard_t   * sb_arrived[2];     // keep track of expected error b-d-p
scoreboard_t     sb_done;           // current tick error computation done
//#uint             sb_thrds_done;     // sync. semaphore: proc & stop
// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
// DEBUG variables
// ------------------------------------------------------------------------
#ifdef DEBUG
  uint pkt_sent = 0;  // total packets sent
  uint sent_fwd = 0;  // packets sent in FORWARD phase
  uint sent_bkp = 0;  // packets sent in BACKPROP phase
  uint pkt_recv = 0;  // total packets received
  uint recv_fwd = 0;  // packets received in FORWARD phase
  uint recv_bkp = 0;  // packets received in BACKPROP phase
  uint spk_sent = 0;  // sync packets sent
  uint spk_recv = 0;  // sync packets received
  uint stp_sent = 0;  // stop packets sent
  uint stp_recv = 0;  // stop packets received
  uint wrng_phs = 0;  // packets received in wrong phase
  uint wrng_tck = 0;  // FORWARD packets received in wrong tick
  uint wrng_btk = 0;  // BACKPROP packets received in wrong tick
  uint wght_ups = 0;  // number of weight updates done
  uint tot_tick = 0;  // total number of ticks executed
#endif
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// load configuration from SDRAM and initialize variables
// ------------------------------------------------------------------------
uint init ()
{
  io_printf (IO_BUF, "sum\n");

  // read the data specification header
  address_t data_address = data_specification_get_data_address ();
  if (!data_specification_read_header (data_address)) {
	  rt_error (RTE_SWERR);
  }

  // get addresses of all SDRAM regions
  // global configuration
  gt = (global_conf_t *) data_specification_get_region
		  (GLOBAL, data_address);

  // initialize network configuration from SDRAM
  spin1_memcpy (&mlpc, gt, sizeof(global_conf_t));

  // chip configuration
  ct = (chip_struct_t *) data_specification_get_region
		  (CHIP, data_address);

  // initialize chip-specific configuration from SDRAM
  spin1_memcpy(&ccfg, ct, sizeof(chip_struct_t));

  // initialize core configuration according to core function
  coreType = ccfg.core_type[coreIndex];

  // fail if wrong type of core
  if (coreType != SPINN_SUM_PROC)
    return (SPINN_CORE_TYPE_ERROR);

  // core configuration
  dt = (uchar *) data_specification_get_region
		  (CORE, data_address);

  // initialize core-specific configuration from SDRAM
  spin1_memcpy (&scfg, dt, sizeof(s_conf_t));

  // example inputs
  it = (activation_t *) data_specification_get_region
		  (INPUTS, data_address);

  // targets are not used by input cores
  tt = NULL;

  // example set
  es = (struct mlp_set *) data_specification_get_region
		  (EXAMPLE_SET, data_address);

  // examples
  ex = (struct mlp_example *) data_specification_get_region
		  (EXAMPLES, data_address);

  // events
  ev = (struct mlp_event *) data_specification_get_region
		  (EVENTS, data_address);

  // routing keys
  rt = (uint *) data_specification_get_region
		  (ROUTING, data_address);

  // initialize epoch, example and event counters
  //TODO: alternative algorithms for chosing example order!
  epoch   = 0;
  example = 0;
  evt     = 0;

  // initialize phase
  phase = SPINN_FORWARD;

  // initialize number of events and event index
  num_events = ex[example].num_events;
  event_idx  = ex[example].ev_idx;

  // allocate memory and initialize variables
  uint rcode = s_init ();

  return (rcode);
}
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// check exit code and print details of the state
// ------------------------------------------------------------------------
void done (uint ec)
{
  // skew execution to avoid tubotron congestion
  spin1_delay_us (SPINN_SKEW_DELAY);  //@delay

  // report problems -- if any
  switch (ec)
  {
    case SPINN_NO_ERROR:
      io_printf (IO_BUF, "simulation OK\n");

      break;

    case SPINN_UKNOWN_TYPE:
      io_printf (IO_BUF, "unknown core type\n");

      break;

    case SPINN_QUEUE_FULL:
      io_printf (IO_BUF, "packet queue full\n");

      break;

    case SPINN_MEM_UNAVAIL:
      io_printf (IO_BUF, "malloc failed\n");

      break;

    case SPINN_UNXPD_PKT:
      io_printf (IO_BUF, "unexpected packet received - abort!\n");

      break;

    case SPINN_TIMEOUT_EXIT:
      io_printf (IO_BUF, "timeout (h:%u e:%u p:%u t:%u) - abort!\n",
                  epoch, example, phase, tick
                );

      #ifdef DEBUG_VRB
        io_printf (IO_BUF, "(fd:%08x bd:%08x)\n", sf_done, sb_done);

        for (uint i = 0; i < scfg.num_nets; i++)
        {
          io_printf (IO_BUF, "(fa:%08x/%08x ba:%08x/%08x)\n",
                      sf_arrived[0][i], sf_arrived[1][i],
                      sb_arrived[0][i], sb_arrived[1][i]
                    );
        }
      #endif

      break;

    // in case the chip configuration data structure defines the core to be of a
    // different type than this executable, throw an error
    case SPINN_CORE_TYPE_ERROR:

      switch (coreType)
      {
        case SPINN_WEIGHT_PROC:
          io_printf (IO_BUF, "error in the core type - executable: %c core, structure: W type\n", SPINN_EXEC_TYPE);
          break;

        case SPINN_SUM_PROC:
          io_printf (IO_BUF, "error in the core type - executable: %c core, structure: S type\n", SPINN_EXEC_TYPE);
          break;

        case SPINN_INPUT_PROC:
          io_printf (IO_BUF, "error in the core type - executable: %c core, structure: I type\n", SPINN_EXEC_TYPE);
          break;

        case SPINN_THRESHOLD_PROC:
          io_printf (IO_BUF, "error in the core type - executable: %c core, structure: T type\n", SPINN_EXEC_TYPE);
          break;

        case SPINN_UNUSED_PROC:
          io_printf (IO_BUF, "error in the core type - executable: %c core, but the core should be unused\n", SPINN_EXEC_TYPE);
          break;

        default:
          io_printf (IO_BUF, "error in the core type - executable: %c core, but chip structure has an invalid entry: %d\n", SPINN_EXEC_TYPE, coreType);
          break;
      }

      break;
  }

  // report diagnostics
  #ifdef DEBUG
    io_printf (IO_BUF, "total ticks:%d\n", tot_tick);
    io_printf (IO_BUF, "recv:%d fwd:%d bkp:%d\n", pkt_recv, recv_fwd, recv_bkp);
    io_printf (IO_BUF, "sent:%d fwd:%d bkp:%d\n", pkt_sent, sent_fwd, sent_bkp);
    io_printf (IO_BUF, "wrong phase:%d\n", wrng_phs);
    io_printf (IO_BUF, "wrong tick:%d\n", wrng_tck);
    io_printf (IO_BUF, "wrong btick:%d\n", wrng_btk);
    io_printf (IO_BUF, "sync recv:%d\n", spk_recv);
    io_printf (IO_BUF, "sync sent:%d\n", spk_sent);
    io_printf (IO_BUF, "stop recv:%d\n", stp_recv);
    io_printf (IO_BUF, "stop sent:%d\n", stp_sent);
  #endif
}
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// timer callback: if the execution takes too long it probably deadlocked.
// Therefore the execution is terminated with SPINN_TIMEOUT_EXIT exit code.
// ------------------------------------------------------------------------
void timeout (uint ticks, uint null)
{
  if (ticks == mlpc.timeout)
  {
    // exit and report timeout
    spin1_exit (SPINN_TIMEOUT_EXIT);
  }
}
// ------------------------------------------------------------------------


// ------------------------------------------------------------------------
// main: register callbacks and initialize basic system variables
// ------------------------------------------------------------------------
void c_main ()
{
  // say hello,
  io_printf (IO_BUF, ">> mlp\n");

  // get this core's IDs,
  chipID = spin1_get_chip_id();
  coreID = spin1_get_core_id();
  coreIndex = coreID - 1; // used to access arrays!

  // initialize application,
  uint exit_code = init ();

  // check if init completed successfully,
  if (exit_code != SPINN_NO_ERROR)
  {

    // if init failed report results,
    done (exit_code);

    // and abort simulation
    return;
  }

  // set timer tick value (in microseconds),
  spin1_set_timer_tick (SPINN_TIMER_TICK_PERIOD);

  #ifdef PROFILE
    // configure timer 2 for profiling
    // enabled, 32 bit, free running, 16x pre-scaler
    tc[T2_CONTROL] = SPINN_TIMER2_CONF;
    tc[T2_LOAD] = SPINN_TIMER2_LOAD;
  #endif

  // register callbacks,
  // timeout escape -- in case something went wrong!
  spin1_callback_on (TIMER_TICK, timeout, SPINN_TIMER_P);

  // packet received callback depends on core function
  spin1_callback_on (MC_PACKET_RECEIVED, s_receivePacket, SPINN_PACKET_P);
  spin1_callback_on (MCPL_PACKET_RECEIVED, s_receivePacket, SPINN_PACKET_P);

  // go,
  io_printf (IO_BUF, "-----------------------\n");
  io_printf (IO_BUF, "starting simulation\n");

  #ifdef PROFILE
    uint start_time = tc[T2_COUNT];
    io_printf (IO_BUF, "start count: %u\n", start_time);
  #endif

  // start execution and get exit code,
  exit_code = spin1_start (SYNC_WAIT);

  #ifdef PROFILE
    uint final_time = tc[T2_COUNT];
    io_printf (IO_BUF, "final count: %u\n", final_time);
    io_printf (IO_BUF, "execution time: %u us\n",
                  (start_time - final_time) / SPINN_TIMER2_DIV);
  #endif

  // report results,
  done (exit_code);

  io_printf (IO_BUF, "stopping simulation\n");
  io_printf (IO_BUF, "-----------------------\n");

  // and say goodbye
  io_printf (IO_BUF, "<< mlp\n");
}
// ------------------------------------------------------------------------
