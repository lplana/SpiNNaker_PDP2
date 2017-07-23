import struct

import spinnaker_graph_front_end as g

from pacman.model.graphs.machine import MachineEdge

from input_vertex     import InputVertex
from sum_vertex       import SumVertex
from threshold_vertex import ThresholdVertex
from weight_vertex    import WeightVertex

from mlp_group import MLPGroup
from mlp_link  import MLPLink
from mlp_types import MLPGroupTypes


class MLPNetwork():
    """ top-level MLP network object.
            contains groups and links
            and top-level properties.
    """

    def __init__(self,
                net_type,
                intervals = 1,
                ticks_per_interval = None,
                ):
        """
        """
        # assign network parameter initial values
        self._net_type           = net_type.value
        self._ticks_per_interval = ticks_per_interval
        self._global_max_ticks   = (intervals * ticks_per_interval) + 1
        self._timeout            = 100

        # initialise lists of groups and links
        self._groups = []
        self._links  = []

        # OUTPUT groups are connected in a daisy chain for convergence
        self._output_chain = []

        # create single-unit Bias group by default
        self._bias_group = self.group (units        = 1,
                                       group_type   = MLPGroupTypes.BIAS,
                                       label        = "Bias"
                                       )


    @property
    def net_type (self):
        return self._net_type

    @property
    def training (self):
        return self._training

    @property
    def num_epochs (self):
        return self._num_epochs

    @property
    def num_examples (self):
        return self._num_examples

    @property
    def ticks_per_int (self):
        return self._ticks_per_int

    @property
    def global_max_ticks (self):
        return self._global_max_ticks

    @property
    def num_write_blocks (self):
        return self._num_write_blks

    @property
    def timeout (self):
        return self._timeout

    @property
    def groups (self):
        return self._groups

    @property
    def output_chain (self):
        return self._output_chain

    @property
    def bias_group (self):
        return self._bias_group

    @property
    def config (self):
        """ returns a packed string that corresponds to
            (C struct) network_conf in mlp_types.h:

            typedef struct network_conf
            {
              uchar net_type;
              uchar training;
              uint  num_epochs;
              uint  num_examples;
              uint  ticks_per_int;
              uint  global_max_ticks;
              uint  num_write_blks;
              uint  timeout;
            } network_conf_t;

            pack: standard sizes, little-endian byte-order,
            explicit padding
        """
        return struct.pack("<2B2x6I",
                           self._net_type,
                           self._training,
                           self._num_epochs,
                           self._num_examples,
                           self._ticks_per_interval,
                           self._global_max_ticks,
                           self._num_write_blks,
                           self._timeout
                           )


    def group (self,
               units        = None,
               group_type   = MLPGroupTypes.HIDDEN,
               input_funcs  = None,
               output_funcs = None,
               write_blk    = None,
               label        = None
               ):
        """ add a group to the network

        :param units:
        :param gtype:
        :param label:

        :return: a new group object
        """
        _id = len (self.groups)

        if (group_type == MLPGroupTypes.OUTPUT):
            _write_blk = len (self.output_chain)
            if len (self.output_chain):
                _is_first_out = 0
            else:
                _is_first_out = 1
        else:
            _write_blk    = 0
            _is_first_out = 0

        _group = MLPGroup (_id,
                           units        = units,
                           gtype        = group_type,
                           input_funcs  = input_funcs,
                           output_funcs = output_funcs,
                           write_blk    = _write_blk,
                           is_first_out = _is_first_out,
                           label        = label
                           )

        print "adding group {} [total: {}]".format (
                label,
                len (self.groups) + 1
                )

        self.groups.append (_group)

        if (group_type == MLPGroupTypes.OUTPUT):
            self.output_chain.append (_group)

        # OUTPUT and HIDDEN groups instantiate BIAS links by default
        if (group_type == MLPGroupTypes.OUTPUT or\
            group_type == MLPGroupTypes.HIDDEN):
            self.link (self.bias_group, _group)

        return _group


    def link (self,
              pre_link_group = None,
              post_link_group = None,
              label = None
              ):
        """ add a link to the network

        :param pre_link_group: link source group
        :param post_link_group: link destination group

        :return: a new link object
        """
        if label is None:
            _label = "{}-{}".format (pre_link_group.label,
                                     post_link_group.label
                                     )
        else:
            _label = label

        _link = MLPLink (pre_link_group  = pre_link_group,
                         post_link_group = post_link_group,
                         label           = _label
                         )

        print "adding link from {} to {} [total: {}]".format (\
            pre_link_group.label,
            post_link_group.label,
            len (self._links) + 1
            )

        self._links.append (_link)

        return _link


    def generate_machine_graph (self,
                                ):
        """ generates a machine graph for simulation
        """
        print "generating machine graph"

        # setup the machine graph
        g.setup ()

        # set the number of write blocks before generating vertices
        self._num_write_blks = len (self.output_chain)

        # create associated weight, sum, input and threshold
        # machine vertices for every network group
        _num_groups = len (self.groups)

        for grp in self.groups:
            # create one weight core per network group, including this one
            for i in range (_num_groups):
                wv = WeightVertex (self,
                                   grp,
                                   from_group = self.groups[i]
                                   )
                g.add_machine_vertex_instance (wv)
                # list w_vertices is ordered by from_group
                grp.w_vertices.append (wv)

            sv = SumVertex (self,
                            grp,
                            fwd_expect = _num_groups,
                            bkp_expect = _num_groups
                            )
            g.add_machine_vertex_instance (sv)
            grp.s_vertex = sv

            iv = InputVertex (self,
                              grp
                              )
            g.add_machine_vertex_instance (iv)
            grp.i_vertex = iv

            # check if last output group in daisy chain
            if (grp.type == MLPGroupTypes.OUTPUT):
                if grp == self.output_chain[-1]:
                    _is_last_out = 1
                else:
                    _is_last_out = 0
            else:
                _is_last_out = 0

            tv = ThresholdVertex (self,
                                  grp,
                                  fwd_sync_expect = _num_groups,
                                  is_last_out     = _is_last_out
                                  )
            g.add_machine_vertex_instance (tv)
            grp.t_vertex = tv

        # create associated forward, backprop, sync and stop
        # machine edges for every network group
        for gn, grp in enumerate (self.groups):
            for w in grp.w_vertices:
                # create forward w to s links
                g.add_machine_edge_instance (MachineEdge (w, grp.s_vertex),
                                             w.fwd_link)
                # create backprop w to s links
                _frmg = w.from_group
                g.add_machine_edge_instance (MachineEdge (w, _frmg.s_vertex),
                                             w.bkp_link)
                # create forward synchronisation w to t links
                g.add_machine_edge_instance (MachineEdge (w, _frmg.t_vertex),
                                             w.fds_link)

            # create forward s to i link
            g.add_machine_edge_instance (MachineEdge (grp.s_vertex,
                                                      grp.i_vertex),
                                         grp.s_vertex.fwd_link)
            # create backprop s to t link
            g.add_machine_edge_instance (MachineEdge (grp.s_vertex,
                                                      grp.t_vertex),
                                         grp.s_vertex.bkp_link)

            # create forward i to t link
            g.add_machine_edge_instance (MachineEdge (grp.i_vertex,
                                                      grp.t_vertex),
                                         grp.i_vertex.fwd_link)
            # create backprop i to w (multicast) links
            for w in grp.w_vertices:
                g.add_machine_edge_instance (MachineEdge (grp.i_vertex, w),
                                             grp.i_vertex.bkp_link)

            # create forward t to w (multicast) links
            for fg in self.groups:
                g.add_machine_edge_instance (MachineEdge (grp.t_vertex,
                                                          fg.w_vertices[gn]),
                                             grp.t_vertex.fwd_link)
            # create backprop t to i link
            g.add_machine_edge_instance (MachineEdge (grp.t_vertex,
                                                      grp.i_vertex),
                                         grp.t_vertex.bkp_link)

            # create stop links, if OUTPUT group
            if grp in self.output_chain:
                # if last OUTPUT group broadcast stop decision
                if grp == self.output_chain[-1]:
                    for stpg in self.groups:
                        for w in stpg.w_vertices:
                            g.add_machine_edge_instance\
                              (MachineEdge (grp.t_vertex, w),
                               grp.t_vertex.stp_link)

                        g.add_machine_edge_instance\
                         (MachineEdge (grp.t_vertex, stpg.s_vertex),\
                          grp.t_vertex.stp_link)

                        g.add_machine_edge_instance\
                         (MachineEdge (grp.t_vertex, stpg.i_vertex),\
                          grp.t_vertex.stp_link)

                        # no link to itself!
                        if stpg != grp:
                            g.add_machine_edge_instance\
                             (MachineEdge (grp.t_vertex, stpg.t_vertex),\
                              grp.t_vertex.stp_link)
                else:
                    # create stop link to next OUTPUT group in chain
                    _inx  = self.output_chain.index (grp)
                    _stpg = self.output_chain[_inx + 1]
                    g.add_machine_edge_instance (MachineEdge (grp.t_vertex,
                                                              _stpg.t_vertex),
                                                 grp.t_vertex.stp_link)


    def train (self,
               num_epochs   = None,
               num_examples = None
               ):
        """ train the application graph for a number of epochs

        :param num_epochs:
        :param num_examples:

        :type  num_epochs: int
        :type  num_examples: int
        """
        print "g.run ()"

        self._training     = 1
        self._num_epochs   = num_epochs
        self._num_examples = num_examples

        # generate machine graph
        self.generate_machine_graph ()

        # run simulation of the machine graph
        g.run (None)


    def test (self,
               num_examples = None
              ):
        """ test the application graph without training

        :param num_examples:

        :type  num_examples: int
        """
        print "g.run ()"

        self._training     = 0
        self._num_examples = num_examples

        # generate machine graph
        self.generate_machine_graph ()

        # run simulation of the machine graph
        g.run (None)


    def end (self):
        """ clean up before exiting
        """
        print "g.stop ()"
        #g.stop()
