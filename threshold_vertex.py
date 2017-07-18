import numpy as np
import os

from data_specification.enums.data_type import DataType

from pacman.executor.injection_decorator import inject_items

from pacman.model.graphs.machine.machine_vertex import MachineVertex
from pacman.model.decorators.overrides import overrides
from pacman.model.resources.resource_container import ResourceContainer
from pacman.model.resources.sdram_resource import SDRAMResource
from pacman.model.resources.iptag_resource import IPtagResource

from spinn_front_end_common.utilities.utility_objs.executable_start_type \
    import ExecutableStartType
from spinn_front_end_common.abstract_models.abstract_has_associated_binary \
    import AbstractHasAssociatedBinary
from spinn_front_end_common.abstract_models\
    .abstract_generates_data_specification \
    import AbstractGeneratesDataSpecification
from spinn_front_end_common.abstract_models\
    .abstract_provides_n_keys_for_partition \
    import AbstractProvidesNKeysForPartition

from mlp_regions import MLPRegions


class ThresholdVertex(
        MachineVertex,
        AbstractHasAssociatedBinary,
        AbstractProvidesNKeysForPartition,
        AbstractGeneratesDataSpecification):
    """ A vertex to implement an MLP threshold core
    """

    def __init__(self, network=None, group=None,
                 file_x=None, file_y=None, file_c=None):
        """
        """

        MachineVertex.__init__(self, label =\
                               "t{} core".format (group))

        # MLP network
        self._network = network

        # forward and backprop link partition names
        self._fwd_link = "fwd_s{}".format (group)
        self._bkp_link = "bkp_s{}".format (group)
        self._stp_link = "stp_s{}".format (group)

        self._n_keys = 65536

        # binary, configuration and data files
        self._aplxFile = "binaries/threshold.aplx"
        self._coreFile = "data/t_conf_{}_{}_{}.dat".format (file_x, file_y, file_c)
        self._inputsFile = "data/inputs_{}.dat".format (group)
        self._targetsFile = "data/targets_{}.dat".format (group)
        self._exSetFile = "data/example_set.dat"
        self._examplesFile = "data/examples.dat"
        self._eventsFile = "data/events.dat"

        self._N_NETWORK_CONFIGURATION_BYTES = \
            len ((self._network).config)

        self._N_CORE_CONFIGURATION_BYTES = \
            os.path.getsize (self._coreFile) \
            if os.path.isfile (self._coreFile) \
            else 0

        self._N_INPUTS_CONFIGURATION_BYTES = \
            os.path.getsize (self._inputsFile) \
            if os.path.isfile (self._inputsFile) \
            else 0

        self._N_TARGETS_CONFIGURATION_BYTES = \
            os.path.getsize (self._targetsFile) \
            if os.path.isfile (self._targetsFile) \
            else 0

        self._N_EXAMPLE_SET_BYTES = \
            os.path.getsize (self._exSetFile) \
            if os.path.isfile (self._exSetFile) \
            else 0

        self._N_EXAMPLES_BYTES = \
            os.path.getsize (self._examplesFile) \
            if os.path.isfile (self._examplesFile) \
            else 0

        self._N_EVENTS_BYTES = \
            os.path.getsize (self._eventsFile) \
            if os.path.isfile (self._eventsFile) \
            else 0

        self._N_KEY_BYTES = 16

        self._sdram_usage = (
            self._N_NETWORK_CONFIGURATION_BYTES + \
            self._N_CORE_CONFIGURATION_BYTES + \
            self._N_INPUTS_CONFIGURATION_BYTES + \
            self._N_TARGETS_CONFIGURATION_BYTES + \
            self._N_EXAMPLE_SET_BYTES + \
            self._N_EXAMPLES_BYTES + \
            self._N_EVENTS_BYTES + \
            self._N_KEY_BYTES
        )

    @property
    def fwd_link (self):
        return self._fwd_link

    @property
    def bkp_link (self):
        return self._bkp_link

    @property
    def stp_link (self):
        return self._stp_link

    @property
    @overrides (MachineVertex.resources_required)
    def resources_required (self):

        resources = ResourceContainer (
            sdram = SDRAMResource (self._sdram_usage),
            iptags = [IPtagResource (ip_address = "localhost",
                                    port = 17896,
                                    strip_sdp = False)]
            )
        return resources

    @overrides (AbstractHasAssociatedBinary.get_binary_file_name)
    def get_binary_file_name (self):
        return self._aplxFile

    @overrides (AbstractHasAssociatedBinary.get_binary_start_type)
    def get_binary_start_type (self):
        return ExecutableStartType.SYNC

    @overrides (AbstractProvidesNKeysForPartition.get_n_keys_for_partition)
    def get_n_keys_for_partition(self, partition, graph_mapper):
        return self._n_keys

    @inject_items ({
        "routing_info": "MemoryRoutingInfos"})
    @overrides (
        AbstractGeneratesDataSpecification.generate_data_specification,
        additional_arguments=["routing_info"])
    def generate_data_specification (
            self, spec, placement, routing_info):

        # Reserve and write the network configuration region
        spec.reserve_memory_region (
            MLPRegions.NETWORK.value,
            self._N_NETWORK_CONFIGURATION_BYTES)

        spec.switch_write_focus (MLPRegions.NETWORK.value)

        # write the network configuration into spec
        for c in (self._network).config:
            spec.write_value (ord (c), data_type=DataType.UINT8)

        # Reserve and write the core configuration region
        if os.path.isfile (self._coreFile):
            spec.reserve_memory_region (
                MLPRegions.CORE.value, self._N_CORE_CONFIGURATION_BYTES)

            spec.switch_write_focus (MLPRegions.CORE.value)

            # open the core configuration file
            core_file = open (self._coreFile, "rb")

            # read the data into a numpy array and put in spec
            pc = np.fromfile (core_file, np.uint8)
            for byte in pc:
                spec.write_value (byte, data_type=DataType.UINT8)

        # Reserve and write the input data region
        if os.path.isfile (self._inputsFile):
            spec.reserve_memory_region (
                MLPRegions.INPUTS.value,
                self._N_INPUTS_CONFIGURATION_BYTES)

            spec.switch_write_focus (MLPRegions.INPUTS.value)

            # open input data file
            inputs_file = open (self._inputsFile, "rb")

            # read the data into a numpy array and put in spec
            ic = np.fromfile (inputs_file, np.uint8)
            for byte in ic:
                spec.write_value (byte, data_type=DataType.UINT8)

        # Reserve and write the target data region
        if os.path.isfile (self._targetsFile):
            spec.reserve_memory_region (
                MLPRegions.TARGETS.value,
                self._N_TARGETS_CONFIGURATION_BYTES)

            spec.switch_write_focus (MLPRegions.TARGETS.value)

            # open target data file
            targets_file = open (self._targetsFile, "rb")

            # read the data into a numpy array and put in spec
            tc = np.fromfile (targets_file, np.uint8)
            for byte in tc:
                spec.write_value (byte, data_type=DataType.UINT8)

        # Reserve and write the example set region
        if os.path.isfile (self._exSetFile):
            spec.reserve_memory_region (
                MLPRegions.EXAMPLE_SET.value,
                self._N_EXAMPLE_SET_BYTES)

            spec.switch_write_focus (MLPRegions.EXAMPLE_SET.value)

            # open the example set file
            ex_set_file = open (self._exSetFile, "rb")

            # read the data into a numpy array and put in spec
            es = np.fromfile (ex_set_file, np.uint8)
            for byte in es:
                spec.write_value (byte, data_type=DataType.UINT8)

        # Reserve and write the examples region
        if os.path.isfile (self._examplesFile):
            spec.reserve_memory_region (
                MLPRegions.EXAMPLES.value,
                self._N_EXAMPLES_BYTES)

            spec.switch_write_focus (MLPRegions.EXAMPLES.value)

            # open the examples file
            examples_file = open (self._examplesFile, "rb")

            # read the data into a numpy array and put in spec
            ex = np.fromfile (examples_file, np.uint8)
            for byte in ex:
                spec.write_value (byte, data_type=DataType.UINT8)

        # Reserve and write the events region
        if os.path.isfile (self._eventsFile):
            spec.reserve_memory_region (
                MLPRegions.EVENTS.value,
                self._N_EVENTS_BYTES)

            spec.switch_write_focus (MLPRegions.EVENTS.value)

            # open the events file
            ev_file = open (self._eventsFile, "rb")

            # read the data into a numpy array and put in spec
            ev = np.fromfile (ev_file, np.uint8)
            for byte in ev:
                spec.write_value (byte, data_type=DataType.UINT8)

        # Reserve and write the routing region
        spec.reserve_memory_region (
            MLPRegions.ROUTING.value, self._N_KEY_BYTES)

        spec.switch_write_focus (MLPRegions.ROUTING.value)

        # write link keys (fwd, bkp, fds, stp)
        spec.write_value (routing_info.get_first_key_from_pre_vertex (
            self, self._fwd_link), data_type = DataType.UINT32)
        spec.write_value (routing_info.get_first_key_from_pre_vertex (
            self, self._bkp_link), data_type = DataType.UINT32)
        spec.write_value (0, data_type = DataType.UINT32)
        spec.write_value (routing_info.get_first_key_from_pre_vertex (
            self, self._stp_link), data_type = DataType.UINT32)

        # End the specification
        spec.end_specification ()
