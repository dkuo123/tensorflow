# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# =============================================================================
"""
Infeed queue
~~~~~~~~~~~~
"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.compiler.plugin.poplar.ops import gen_pop_datastream_ops
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.data.util import structure
from tensorflow.python.framework import ops


class IPUInfeedQueue:
  """Wraps a tf.Dataset object with infeed operations specific to the IPU.

  This class, along with `tensorflow.python.ipu.loops` is used to create a data
  pipeline from a `dataset` into a training/inference loop on the IPU inside a
  single `session.run` which reduces the overheads of calling `session.run` for
  each iteration of the loop.

  You should pass the infeed queue as an argument to a loop from
  `tensorflow.python.ipu.loops`. These loops will then handle the dequeuing of
  the data to the device automatically.

  The feed_name allows individual feeds to be named.  When including more than
  one feed in the same graph, each should be independently named.

  The following skeleton shows how to use this method when building a training
  loop. Note how the body signature contains variables which correspond to the
  nested structure of `tf.Tensor` objects representing the next element in the
  infeed queue:

  .. code-block:: python

    # Create an example dataset.
    dataset = ...  # A `tf.data.Dataset` object.

    def dataset_parser(value):
      features, labels = parse_record(value)
      return {"features": features,
              "labels": labels}
    # The resulting dataset has a nested structure of: {features, labels}.
    dataset = dataset.map(dataset_parser)

    infeed_queue = ipu.ipu_infeed_queue.IPUInfeedQueue(dataset, feed_name="training_infeed")

    # dataset can no longer be used beyond this point.

    def my_net():
      # Note how the nested structure forms part of the loop body signature.
      def body(loss, features, labels):
        with variable_scope.variable_scope("vs", use_resource=True):
          y = tf.conv2d(features, .....)
          ...
          ...
          logits = tf.nn.xw_plus_b(....)
        loss = tf.reduce_mean(tf.nn.softmax_cross_entropy_with_logits_v2(logits=logits, labels=labels))
        optimizer = gradient_descent.GradientDescentOptimizer(0.000001)
        train = optimizer.minimize(loss)
        with ops.control_dependencies([train]):
          return array_ops.identity(loss)

      loss = 0.0
      return = tf.conrib.ipu.loops.repeat(10000, body, [loss], infeed_queue)

    with ipu.ops.ipu_scope("/device:IPU:0"):
      res = ipu_compiler.compile(my_net, inputs=[])

    with tf.Session() as sess:
      sess.run(infeed_queue.initializer)
      sess.run(variables.global_variables_initializer())
      result = sess.run(res)

  """

  def __init__(self,
               dataset,
               feed_name,
               device_ordinal=0,
               replication_factor=1,
               data_to_prefetch=1):
    """Creates an IPUInfeedQueue object.

    Args:
       dataset: a tf.data.Dataset object, all transformations e.g. `shuffle`,
         `repeat`, `batch` must be applied prior to passing in to this function.
         This dataset can no longer be used after creating this queue.
       feed_name: the name of the infeed queue.  This must be unique between
         all IPUInfeedQueues and IPUOutfeedQueues.
       device_ordinal: ordinal of the device on which this queue will be used.
       replication_factor: the number of replicated graphs this infeed will be
         used in.
       data_to_prefetch: The amount of data to prefetch. Defaults to 1, no prefetch.
        If set to non-1 (and non-0) each time we sync with the CPU we will return
        this number of dataset values rather than 1. This must not go over the size
        of the dataset if it is not repeating, and will increment the infeed by this
        amount each time so using the infeed in multiple programs or loops should take
        into account that if data_to_prefetch is not a factor of the previous iterations
        then the next loop/program will not be starting at the iteration it otherwise
        would be.

        This will obviously increase the memory usage from having more batches live at a
        given point but should give a speed up by having to make fewer round trips to host
        memory. It may be that larger number of batches should be prefetched at once in
        order to see any benefit as the lookup itself has some overhead from internal copies.

    Raises:
      ValueError: if all dimensions of shapes of dataset.output_shapes are not
        fully defined. tf.data.batch function must be called with
        `drop_remainder=True` to ensure that batch size is constant.

    """

    for output_shape in dataset._flat_structure["output_shapes"]:
      if isinstance(output_shape, list) or isinstance(output_shape, tuple):
        raise ValueError("Nested list/tuple input shapes are not supported")
      if not output_shape.is_fully_defined():
        raise ValueError("""Output shape {} is not fully defined. If using \
tf.Dataset.batch, set `drop_remainder=True`.""".format(output_shape))
    with ops.device('/device:CPU:0'):
      self._replication_factor = replication_factor
      self._dataset = dataset
      self._structure = dataset_ops.get_structure(self._dataset)
      self._flat_structure = dataset._flat_structure

      # We use max to clamp 0/1 to the same value.
      self._data_to_prefetch = max(1, data_to_prefetch)

      # Batch the dataset to take replication and prefetch into account.

      if self._data_to_prefetch != 1:
        self._dataset = self._dataset.batch(
            self._data_to_prefetch, drop_remainder=True)

      if self._replication_factor != 1:
        self._dataset = self._dataset.batch(
            self._replication_factor, drop_remainder=True)

      # Apply the dataset and take ownership.
      self._dataset = self._dataset._apply_options()

      try:
        ds_variant = self._dataset._variant_tensor
      except TypeError:
        ds_variant = self._dataset._as_variant_tensor
      # ID used for differentiating between datasets.
      self._id = str(feed_name)

      # Dataset iterator creator.
      self._initializer = gen_pop_datastream_ops.ipu_consume_dataset(
          input_dataset=ds_variant,
          feed_id=self._id,
          replication_factor=self._replication_factor,
          device_ordinal=device_ordinal,
          **self._dataset._flat_structure)

    self._dequeued = False
    self._initialized = False

  def _dequeue(self):
    """Returns a nested structure of `tf.Tensor`s representing the next element
    in the infeed queue.

    This function should not be called directly, instead the infeed should be
    passed to a loop from `tensorflow.python.ipu.loops`.

    Returns:
      A nested structure of `tf.Tensor` objects.
    """
    flat_ret = gen_pop_datastream_ops.pop_datastream_infeed_dequeue(
        feed_id=self._id,
        replication_factor=self._replication_factor,
        data_to_prefetch=self._data_to_prefetch,
        **self._flat_structure)
    self._dequeued = True
    return structure.from_tensor_list(self._structure, flat_ret)

  @property
  def dequeued(self):
    """Returns whether this queue has been dequeued.

    Returns:
      A nested structure of `tf.Tensor` objects.
    """
    return self._dequeued

  @property
  def number_of_tuple_elements(self):
    """Returns the number of IPUInfeedQueue tuple elements."""
    return len(structure.get_flat_tensor_specs(self._structure))

  @property
  def initializer(self):
    """A `tf.Operation` that should be run to initialize this IPUInfeedQueue.

    Returns:
      A `tf.Operation` that should be run to initialize this IPUInfeedQueue

    Raises:
      ValueError: if the function `initializer` has already been called.
    """
    if self._initialized:
      raise ValueError(
          """The IPUInfeedQueue `initializer` function can only be accessed once."""
      )
    self._initialized = True
    return self._initializer

  def get_next(self):
    """Obsolete function."""
    raise ValueError("""`get_next()` is now obsolete as the IPUInfeedQueue is \
now automatically dequeued by the loop.""")
