# Copyright 2017 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import random
import shutil
import glob

from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.compiler.plugin.poplar.tests import test_utils as tu
from tensorflow.python import ipu
from tensorflow.python.platform import googletest
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.estimator import estimator
from tensorflow.python.estimator import model_fn as model_fn_lib
from tensorflow.python.estimator import run_config
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.layers import layers
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn
from tensorflow.python.ops import variable_scope
from tensorflow.python.training import gradient_descent
from tensorflow.python.summary import summary_iterator
from tensorflow.python.training import training_util

def model_fn(features, labels, mode):
  with ops.device("/device:IPU:0"):
    with variable_scope.variable_scope("ascope", use_resource=True):
      x = array_ops.reshape(features, [-1, 4])
      x = layers.dense(inputs=x, units=10)
      x = layers.dense(inputs=x, units=3)

      if (mode == model_fn_lib.ModeKeys.TRAIN
          or mode == model_fn_lib.ModeKeys.EVAL):
        labels = array_ops.stop_gradient(labels)
        loss = math_ops.reduce_mean(
            nn.softmax_cross_entropy_with_logits_v2(logits=x, labels=labels))
      else:
        loss = None

      if mode == model_fn_lib.ModeKeys.TRAIN:
        opt = gradient_descent.GradientDescentOptimizer(0.01)
        train = opt.minimize(loss, training_util.get_global_step())
      else:
        train = None

  ipu.summary_ops.ipu_compile_summary("compile_summary", [train, loss])

  return model_fn_lib.EstimatorSpec(
      mode=mode, predictions=x, loss=loss, train_op=train)


def input_fn():

  t_data = []
  v_data = []
  for _ in range(16 * 4):
    type = random.randint(0, 2)
    t = [random.random(), random.random(), random.random(), random.random()]
    t[type] += random.uniform(1.0, 3.0)
    v = [0, 0, 0]
    v[type] = 1.0
    t_data.append(t)
    v_data.append(v)

  dataset = dataset_ops.Dataset.from_tensor_slices((t_data, v_data))
  dataset = dataset.batch(4)
  return dataset


class IpuEstimatorTest(test_util.TensorFlowTestCase):
  def testTrain(self):

    shutil.rmtree("testlogs", True)

    tu.configure_ipu_system()

    run_cfg = run_config.RunConfig()

    classifier = estimator.Estimator(
        model_fn=model_fn, config=run_cfg, model_dir="testlogs")

    classifier.train(input_fn=input_fn, steps=16)

    event_file = glob.glob("testlogs/event*")

    self.assertTrue(len(event_file) == 1)

    compile_for_ipu_count = 0
    for summary in summary_iterator.summary_iterator(event_file[0]):
      for val in summary.summary.value:
        if val.tag == "ipu_trace":
          for evt_str in val.tensor.string_val:
            evt = IpuTraceEvent.FromString(evt_str)
            if (evt.type == IpuTraceEvent.COMPILE_END
                and len(evt.compile_end.compilation_report)) > 0:
              compile_for_ipu_count += 1

    # Initialization graph and main graph
    self.assertEqual(compile_for_ipu_count, 2)


if __name__ == "__main__":
  googletest.main()