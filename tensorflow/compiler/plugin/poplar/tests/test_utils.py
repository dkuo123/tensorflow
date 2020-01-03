# Copyright 2017 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
import contextlib
import fnmatch
import json as js
import numpy as np

from tensorflow.compiler.plugin.poplar.driver.config_pb2 import IpuOptions
from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.core.framework import attr_value_pb2
from tensorflow.python.data.ops.dataset_ops import Dataset
from tensorflow.python.client import session as session_lib
from tensorflow.python.framework import ops
from tensorflow.python.ops import gen_array_ops
from tensorflow.python.ops import math_ops


def configure_ipu_system(compilation_trace=True,
                         io_trace=False,
                         execution_trace=True,
                         report_every_nth_execution=0,
                         text_report=True,
                         cbor_report=False,
                         sharded=False,
                         pipelining=False,
                         replicated=False,
                         compile_ipu_code=False,
                         enable_ipu_events=False,
                         prefetch_data_streams=True,
                         engine_opts=None,
                         device_count_override=None,
                         max_cross_replica_sum_buffer_size=0,
                         max_inter_ipu_copies_buffer_size=0,
                         merge_infeed_io_copies=False,
                         always_rearrange_copies_on_the_host=False,
                         serialization_folder=""):
  opts = IpuOptions()
  opts.profiling.enable_ipu_trace_events = (compilation_trace or io_trace
                                            or execution_trace
                                            or enable_ipu_events)
  opts.profiling.enable_compilation_trace = compilation_trace
  opts.profiling.enable_io_trace = io_trace
  opts.profiling.enable_execution_trace = execution_trace
  opts.profiling.enable_poplar_reports_text = text_report
  opts.profiling.enable_poplar_reports_cbor = cbor_report
  opts.profiling.report_every_nth_execution = report_every_nth_execution
  opts.profiling.max_report_size = 0x10000000  # 256MB
  opts.ipu_model_config.enable_ipu_model = True
  opts.ipu_model_config.compile_ipu_code = compile_ipu_code
  opts.prefetch_data_streams = prefetch_data_streams
  opts.max_cross_replica_sum_buffer_size = max_cross_replica_sum_buffer_size
  opts.max_inter_ipu_copies_buffer_size = max_inter_ipu_copies_buffer_size
  opts.speed_size_config.merge_infeed_io_copies = merge_infeed_io_copies
  opts.speed_size_config.always_rearrange_copies_on_the_host = \
      always_rearrange_copies_on_the_host
  opts.serialization_folder = serialization_folder

  # yapf: disable
  assert not (pipelining and device_count_override
             ), "Can't have both pipelining enabled and device_count_override"
  assert not (sharded and device_count_override
             ), "Can't have both sharded enabled and device_count_override"
  # yapf: enable

  if engine_opts:
    for o in engine_opts.items():
      opt = opts.compilation_options.add()
      opt.option = o[0]
      opt.value = o[1]

  # When sharded or pipelining we use two devices.
  if device_count_override:
    device_count = device_count_override
  elif sharded:
    device_count = 2 * (2 if replicated else 1)
  elif pipelining:
    device_count = 4 * (2 if replicated else 1)
  else:
    device_count = 2 if replicated else 0

  if device_count:
    dev = opts.device_config.add()
    dev.auto_count = device_count

  g = ops.Graph()
  with g.as_default():
    cfg_op = gen_ipu_ops.ipu_configure_hardware(opts.SerializeToString())

  with session_lib.Session(graph=g) as sess:
    sess.run(cfg_op)


@contextlib.contextmanager
def ipu_session():
  with session_lib.Session() as sess:
    yield sess


def items_matching_at_least_one_pattern(items, patterns):
  matches = []
  patterns = [x + '*' for x in patterns]
  for item in items:
    if [p for p in patterns if fnmatch.fnmatch(item, p)]:
      matches.append(item)
  return matches


def names_in_blacklist(names, blacklist):
  return items_matching_at_least_one_pattern(names, blacklist)


def missing_names_in_whitelist_entries(names, whitelist):
  fail_list = []
  wl = [x + '*' for x in whitelist]
  for name in names:
    if name and not [x for x in wl if fnmatch.fnmatch(name, x)]:
      fail_list += [name]
  return fail_list


def missing_whitelist_entries_in_names(names, whitelist):
  fail_list = []
  wl = [x + '*' for x in whitelist]
  for x in wl:
    if not [name for name in names if fnmatch.fnmatch(name, x)]:
      fail_list += [x]
  return fail_list


def count_matches_in_list(input_list, to_match):
  return len([s for s in input_list if fnmatch.fnmatch(s, to_match)])


class TensorMap(object):
  class Tile(object):
    def __init__(self, tile, num_elements):
      self.tile = tile
      self.num_elements = num_elements

    def __eq__(self, other):
      return self.tile == other.tile and self.num_elements == other.num_elements

  class Tensor(object):
    def __init__(self, inst, index, shape, dtype, has_constant, has_aliases,
                 num_elements, tiles):
      self.inst = inst
      self.index = index
      self.shape = shape
      self.dtype = dtype
      self.has_constant = has_constant
      self.has_aliases = has_aliases
      self.num_elements = num_elements
      self.tiles = tiles

    def tile_ids(self):
      return list({t.tile for t in self.tiles})

  def __init__(self, tensor_map, num_tiles_per_ipu):
    self.num_tiles_per_ipu = num_tiles_per_ipu
    self.mappings = {}
    for comp, js_tensors in tensor_map["mappings"].items():
      tensors = []
      for js_tensor in js_tensors:
        tiles = []
        for tile in js_tensor[7]:
          assert len(tile) == 2
          tiles.append(TensorMap.Tile(tile[0], tile[1]))
        tensors.append(
            TensorMap.Tensor(inst=js_tensor[0],
                             index=js_tensor[1],
                             shape=js_tensor[2],
                             dtype=js_tensor[3],
                             has_constant=bool(js_tensor[4]),
                             has_aliases=bool(js_tensor[5]),
                             num_elements=js_tensor[6],
                             tiles=tiles))
      self.mappings[comp] = tensors

  def all_tensors(self):
    for _, tensors in self.mappings.items():
      for tensor in tensors:
        yield tensor

  def tile_ids(self, computation=None):
    if isinstance(computation, list):
      computations = computation
    else:
      computations = [computation] if computation else self.mappings.keys()
    ids = set()
    for c in computations:
      for tensor in self.mappings[c]:
        ids.update(tensor.tile_ids())
    return ids

  def ipu_ids(self, computation=None):
    tile_ids = self.tile_ids(computation)
    return {int(tile_id / self.num_tiles_per_ipu) for tile_id in tile_ids}

  def computation_names(self):
    return list(self.mappings.keys())


class ReportJSON(object):
  def __init__(self,
               test,
               sess=None,
               io_trace=True,
               compile_ipu_code=False,
               device_count_override=None,
               execution_trace=True,
               sharded=False,
               compilation_trace=True,
               pipelining=False,
               configure_device=True,
               replicated=False,
               max_cross_replica_sum_buffer_size=0,
               max_inter_ipu_copies_buffer_size=0,
               merge_infeed_io_copies=False,
               always_rearrange_copies_on_the_host=False,
               serialization_folder=""):
    self.test = test
    self.sess = sess
    # If no session is passed to the constructor then assume
    # the events will be provided by the user.
    if sess:
      with ops.device('cpu'):
        self.report = gen_ipu_ops.ipu_event_trace()
      if configure_device:
        configure_ipu_system(
            compilation_trace,
            io_trace,
            execution_trace=execution_trace,
            text_report=False,
            compile_ipu_code=compile_ipu_code,
            device_count_override=device_count_override,
            sharded=sharded,
            pipelining=pipelining,
            replicated=replicated,
            max_cross_replica_sum_buffer_size=max_cross_replica_sum_buffer_size,
            max_inter_ipu_copies_buffer_size=max_inter_ipu_copies_buffer_size,
            merge_infeed_io_copies=merge_infeed_io_copies,
            always_rearrange_copies_on_the_host=
            always_rearrange_copies_on_the_host,
            serialization_folder=serialization_folder)

  def reset(self):
    assert self.sess, "A valid session must be passed to the constructor" \
    " to use this method"
    self.sess.run(self.report)

  def parse_log(self, assert_len=None, assert_msg=""):
    assert self.sess, "A valid session must be passed to the constructor" \
    " to use this method"
    events = self.sess.run(self.report)
    return self.parse_events(events, assert_len, assert_msg)

  def parse_events(self, events, assert_len=None, assert_msg=""):
    if assert_len:
      self.test.assertEqual(assert_len, len(events), assert_msg)
    self.events = {}
    self.tensor_map = None
    self.instruction_info = {}
    events_types = collections.defaultdict(int)
    for e in events:
      evt = IpuTraceEvent.FromString(e)
      events_types[evt.type] += 1
      try:
        if evt.type == IpuTraceEvent.COMPILE_BEGIN:
          pass
        if evt.type == IpuTraceEvent.COMPILE_END:
          if evt.compile_end.compilation_report:
            assert IpuTraceEvent.COMPILE_END not in self.events
            self.events[IpuTraceEvent.COMPILE_END] = js.loads(
                evt.compile_end.compilation_report, encoding="utf-8")
            self.tensor_map = TensorMap(
                js.loads(evt.compile_end.tensor_map, encoding="utf-8"),
                self.get_num_tiles_per_ipu())
            self.instruction_info = js.loads(evt.compile_end.instruction_info,
                                             encoding="utf-8")
        if evt.type == IpuTraceEvent.HOST_TO_DEVICE_TRANSFER:
          if evt.data_transfer.data_transfer:
            assert IpuTraceEvent.HOST_TO_DEVICE_TRANSFER not in self.events
            self.events[IpuTraceEvent.HOST_TO_DEVICE_TRANSFER] = js.loads(
                evt.data_transfer.data_transfer, encoding="utf-8")
        if evt.type == IpuTraceEvent.DEVICE_TO_HOST_TRANSFER:
          if evt.data_transfer.data_transfer:
            assert IpuTraceEvent.DEVICE_TO_HOST_TRANSFER not in self.events
            self.events[IpuTraceEvent.DEVICE_TO_HOST_TRANSFER] = js.loads(
                evt.data_transfer.data_transfer, encoding="utf-8")
        if evt.type == IpuTraceEvent.LOAD_ENGINE:
          pass
        if evt.type == IpuTraceEvent.EXECUTE:
          if evt.execute.execution_report:
            self.events[IpuTraceEvent.EXECUTE] = self.events.get(
                IpuTraceEvent.EXECUTE, []) + [
                    js.loads(evt.execute.execution_report, encoding="utf-8")
                ]
      except UnicodeDecodeError:
        pass
    return events_types

  def get_host_to_device_event_names(self):
    return [
        t["name"]
        for t in self.events[IpuTraceEvent.HOST_TO_DEVICE_TRANSFER]["tensors"]
    ]

  def get_device_to_host_event_names(self):
    return [
        t["name"]
        for t in self.events[IpuTraceEvent.DEVICE_TO_HOST_TRANSFER]["tensors"]
    ]

  def assert_host_to_device_event_names(self, names, msg=None):
    self.test.assertEqual(
        len(names),
        len(
            self.events.get(IpuTraceEvent.HOST_TO_DEVICE_TRANSFER,
                            {}).get("tensors", [])), msg)
    for name in names:
      self.test.assertEqual(
          count_matches_in_list(self.get_host_to_device_event_names(), name),
          1, msg)

  def assert_device_to_host_event_names(self, names, msg=None):
    self.test.assertEqual(
        len(names),
        len(
            self.events.get(IpuTraceEvent.DEVICE_TO_HOST_TRANSFER,
                            {}).get("tensors", [])), msg)
    for name in names:
      self.test.assertEqual(
          count_matches_in_list(self.get_device_to_host_event_names(), name),
          1, msg)

  def get_each_tile_memory(self):
    return self.events[IpuTraceEvent.COMPILE_END]["memory"]["byTile"]["total"]

  # Excluding gaps
  def get_max_tile_memory(self):
    return max(self.get_each_tile_memory())

  def get_always_live_memory(self):
    return sum(self.events[IpuTraceEvent.COMPILE_END]["memory"]["liveness"]
               ["alwaysLive"]["bytesByTile"])

  def get_total_tile_memory(self):
    return sum(self.get_each_tile_memory())

  def get_vertices(self):
    return self.events[IpuTraceEvent.COMPILE_END]["vertexTypes"]["names"]

  def get_compute_sets(self):
    return self.events[IpuTraceEvent.COMPILE_END]["computeSets"]["names"]

  def get_execution_reports(self):
    return self.events[IpuTraceEvent.EXECUTE]

  def get_instruction_info(self):
    return self.instruction_info

  def get_ml_type_counts(self):
    res = [0, 0, 0, 0]
    for i in self.instruction_info['ml_types'].values():
      ml_type = i - 1
      res[ml_type] = res[ml_type] + 1
    return res

  def assert_no_compute_set(self):
    self.test.assertFalse(
        self.events.get(IpuTraceEvent.COMPILE_END,
                        {}).get("computeSets", {}).get("names", {}))

  def assert_contains_one_compile_event(self):
    self.test.assertTrue(IpuTraceEvent.COMPILE_END in self.events)

  def get_tensor_map(self):
    return self.tensor_map

  def get_num_ipus(self):
    return self.events[IpuTraceEvent.COMPILE_END]["target"]["numIPUs"]

  def get_num_tiles(self):
    return self.events[IpuTraceEvent.COMPILE_END]["target"]["numTiles"]

  def get_num_tiles_per_ipu(self):
    return self.get_num_tiles() / self.get_num_ipus()

  def get_first_program_of_type(self, program_type):
    for p in self.events[IpuTraceEvent.COMPILE_END]["programs"]:
      if program_type == p['type']:
        return p
    return None

  def get_program_names_of_type(self, program_type):
    return [
        p['name'] for p in self.events[IpuTraceEvent.COMPILE_END]["programs"]
        if p['type'] == program_type
    ]

  def get_program(self, index=0):
    return self.events[IpuTraceEvent.COMPILE_END]["programs"][index]

  def assert_pipeline_stages_on_expected_ipu(self, expected_ipus):
    self.test.assertFalse(
        items_matching_at_least_one_pattern(
            self.tensor_map.computation_names(),
            ["*_stage_%d_" % (len(expected_ipus) + 1)]),
        "The number of expected_ipus does not match the number of stages")
    for i, expected_ipu in enumerate(expected_ipus):
      stage = items_matching_at_least_one_pattern(
          self.tensor_map.computation_names(), ["*_stage_%d_" % i])
      self.test.assertTrue(stage, "No stage %d found" % i)
      ipus = self.tensor_map.ipu_ids(stage)
      self.test.assertEqual(
          len(ipus), 1,
          "Stage %d was mapped to more than one ipu: %s" % (i + 1, ipus))
      self.test.assertEqual(
          ipus.pop(), expected_ipu,
          "Stage %d did not run on the expected IPU" % (i + 1))

  def assert_each_tile_memory_is_less_than(self, expected, tolerance=0.01):
    low = 0
    high = int(expected * (1.0 + tolerance))
    self.test.assertAllInRange(self.get_each_tile_memory(), low, high)

  def assert_total_tile_memory(self, expected, tolerance=0.01):
    low = int(expected * (1.0 - tolerance))
    high = int(expected * (1.0 + tolerance))
    self.test.assertAllInRange([self.get_total_tile_memory()], low, high)

  def assert_max_tile_memory(self, expected, tolerance=0.01):
    low = int(expected * (1.0 - tolerance))
    high = int(expected * (1.0 + tolerance))
    self.test.assertAllInRange([self.get_max_tile_memory()], low, high)

  def assert_always_live_memory(self, expected, tolerance=0.01):
    low = int(expected * (1.0 - tolerance))
    high = int(expected * (1.0 + tolerance))
    self.test.assertAllInRange([self.get_always_live_memory()], low, high)

  # Asserts all the compute sets match a pattern in the whitelist and also asserts that all the whitelist patterns match at least one compute set
  def assert_all_compute_sets_and_list(self, ok):
    self.test.assertFalse(
        missing_whitelist_entries_in_names(self.get_compute_sets(), ok),
        "Whitelist items not found in compute sets:\n\t%s" %
        "\n\t".join(self.get_compute_sets()))
    self.test.assertFalse(
        missing_names_in_whitelist_entries(self.get_compute_sets(), ok),
        "Compute sets item not found in whitelist:\n\t%s" % "\n\t".join(ok))

  # Asserts all the global exchanges match a pattern in the whitelist and also asserts that all the whitelist patterns match at least one global exchange
  def assert_all_global_exchanges_and_list(self, ok):
    self.test.assertFalse(
        missing_whitelist_entries_in_names(
            self.get_program_names_of_type('GlobalExchange'),
            ok), "Whitelist items not found in global exchanges:\n\t%s" %
        "\n\t".join(self.get_compute_sets()))
    self.test.assertFalse(
        missing_names_in_whitelist_entries(
            self.get_program_names_of_type('GlobalExchange'),
            ok), "Global exchanges item not found in whitelist:\n\t%s" %
        "\n\t".join(ok))

  # Asserts that all the whitelist patterns match at least one compute set
  def assert_compute_sets_contain_list(self, ok):
    self.test.assertFalse(
        missing_whitelist_entries_in_names(self.get_compute_sets(), ok),
        "Whitelist items not found in compute sets:\n\t%s" %
        "\n\t".join(self.get_compute_sets()))

  # Asserts that none of the compute sets match any of the blacklist items
  def assert_compute_sets_not_in_blacklist(self, blacklist):
    self.test.assertFalse(
        names_in_blacklist(self.get_compute_sets(), blacklist),
        "Compute sets items found in blacklist:\n\t%s" %
        "\n\t".join(blacklist))

  # Asserts that all the whitelist patterns match at least one vertex
  def assert_vertices_contain_list(self, ok):
    self.test.assertFalse(
        missing_whitelist_entries_in_names(self.get_vertices(), ok),
        "Whitelist items not found in vertices:\n\t%s" %
        "\n\t".join(self.get_vertices()))

  def assert_compute_sets_matches(self, expr, num_matches, msg=None):
    self.test.assertEqual(count_matches_in_list(self.get_compute_sets(), expr),
                          num_matches, msg)


def extract_all_compile_end_events(events):
  result = []
  for e in events:
    evt = IpuTraceEvent.FromString(e)
    if evt.type == IpuTraceEvent.COMPILE_END:
      if evt.compile_end.compilation_report:
        result += [evt]
  return result


def extract_all_execute_events(events):
  result = []
  for e in events:
    evt = IpuTraceEvent.FromString(e)
    if evt.type == IpuTraceEvent.EXECUTE:
      result += [evt]
  return result


def create_multi_increasing_dataset(value,
                                    shapes=None,
                                    dtypes=None,
                                    repeat=True):
  # Default values:
  shapes = shapes if shapes else [[1, 32, 32, 4], [1, 8]]
  dtypes = dtypes if dtypes else [np.float32, np.float32]

  def _get_one_input(data):
    result = []
    for i, shape in enumerate(shapes):
      result.append(
          math_ops.cast(gen_array_ops.broadcast_to(data, shape=shape),
                        dtype=dtypes[i]))
    return result

  dataset = Dataset.range(value).map(_get_one_input)
  if repeat:
    dataset = dataset.repeat()
  return dataset


def create_dual_increasing_dataset(value,
                                   data_shape=None,
                                   label_shape=None,
                                   dtype=np.float32,
                                   repeat=True):
  data_shape = data_shape if data_shape else [1, 32, 32, 4]
  label_shape = label_shape if label_shape else [1, 8]
  return create_multi_increasing_dataset(value,
                                         shapes=[data_shape, label_shape],
                                         dtypes=[dtype, dtype],
                                         repeat=repeat)


def create_single_increasing_dataset(value,
                                     shape=None,
                                     dtype=np.float32,
                                     repeat=True):
  shape = shape if shape is not None else [1, 32, 32, 4]
  return create_multi_increasing_dataset(value,
                                         shapes=[shape],
                                         dtypes=[dtype],
                                         repeat=repeat)


def move_variable_initialization_to_cpu():
  graph = ops.get_default_graph()

  init_ops = []
  dep_ops = [
      x.initializer.inputs[1].op for x in graph.get_collection('variables')
  ]
  visited = set()

  while dep_ops:
    op = dep_ops.pop()
    if not op in visited:
      visited.add(op)
      init_ops += [op]
      dep_ops += [x.op for x in op.inputs]

  # pylint: disable=protected-access
  for op in init_ops:
    op._set_device('/device:CPU:0')
    op._set_attr(
        '_class',
        attr_value_pb2.AttrValue(list=attr_value_pb2.AttrValue.ListValue(
            s=[b'loc:@cpu'])))
    op._set_attr('_XlaCompile', attr_value_pb2.AttrValue(b=False))
    op._set_attr('_XlaScope', attr_value_pb2.AttrValue(s=b''))
  # pylint: enable=protected-access
