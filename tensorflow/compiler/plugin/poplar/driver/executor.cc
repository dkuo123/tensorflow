/* Copyright 2017 Graphcore Ltd
 */

/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "include/json/json.h"

#include "tensorflow/compiler/plugin/poplar/driver/conversions.h"
#include "tensorflow/compiler/plugin/poplar/driver/executable.h"
#include "tensorflow/compiler/plugin/poplar/driver/executor.h"
#include "tensorflow/compiler/plugin/poplar/driver/hlo_hash.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/platform.h"
#include "tensorflow/compiler/plugin/poplar/driver/platform_id.h"
#include "tensorflow/compiler/plugin/poplar/driver/util.h"

#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/status_macros.h"

#include "tensorflow/core/lib/strings/stringprintf.h"

#include "google/protobuf/util/message_differencer.h"

#include <fstream>

#include <string.h>

#include <poplar/DeviceManager.hpp>
#include <poplar/IPUModel.hpp>
#include <poplar/Tensor.hpp>

/*
 * TensorControl is a structure that maintains state about the location
 * of a tensor - either on the device or cached on the host.
 *
 * Tensorflow/XLA assumes that a tensor is on the device when the device
 * allocator is called (PoplarExecutor::Allocate).  However, Poplar cannot
 * allocate tensors independently of the compiled Engine.  The TensorControl
 * structure tracks where the tensors are.
 *
 * TensorControl has three pieces of interacting state:
 *   on_device: This says whether the data is on the device (in one of the
 *              tensors belonging to the currently loaded engine).  When this
 *              is false, it means the data is being held in the host side
 *              buffer.
 *
 *   input_handle: If the tensor is on_device, and this is not -1, then it
 *                 indicates which of the input tensors of the current engine
 *                 contains the data.
 *
 *   output_handle: If the tensor is on_device, and this is not empty, then it
 *                  indicates which of the output tensors of the current
 *                  engine contains the data.
 *
 *   The states are:
 *     on_device=false :
 *       The data is in the host buffer.  If this buffer is passed as an
 *       argument when an engine is executed then it must be copied to the
 *       device.
 *
 *     on_device=true, input_handle not empty, output_handle is empty :
 *       During the previous engine execution, the data was copied to the
 *       device as one of the arguments.  On the next execution, if the engine
 *       does not change, and the argument index is the same, then the data
 *       does not need to be recopied to the device.  I suspect that this case
 *       is rare.
 *
 *     on_device=true, input_handle is empty, output_handle not empty :
 *       During the last execution, the buffer was allocated to represent one
 *       of the outputs of the engine.  If the host wants to read the data back
 *       then it will have to be retrieved from the device.  If the next
 *       execution changes the engine, then the data will have to be read back.
 *
 *     on_device=true, input_handle not empty, output_handle not empty :
 *       During the last execution, the buffer was an argument to the execution
 *       and was also one of the output parameters.  This typically indicates
 *       that it is a variable (weights/biases) that has been updated in place.
 *       If the next execution doesn't change the engine, and the data is not
 *       read back to the host in between executions, and the data remains as
 *       an argument to the same input number, then the data does not need to be
 *       copied back to the host.  This is the ideal situation when executing an
 *       engine repeatedly with the same set of weights/biases.
 *
 */
namespace se = ::stream_executor;

namespace xla {
namespace poplarplugin {

std::string GetInputCopyHandle(int64 parameter, int64 index) {
  return tensorflow::strings::Printf("%lld.%lld", parameter, index);
}

std::string GetOutputCopyHandle(int64 output_index, int64 flat_tensor_index) {
  return tensorflow::strings::Printf("out_%lld.%lld", output_index,
                                     flat_tensor_index);
}

se::host::HostStream* AsPoplarStream(se::Stream* stream) {
  DCHECK(stream != nullptr);
  return dynamic_cast<se::host::HostStream*>(stream->implementation());
}

PoplarExecutor::PoplarExecutor()
    : ordinal_(0),
      device_open_(false),
      poplar_device_(poplar::Device::createCPUDevice()),
      poplar_device_hash_(0) {
  ConfigurePoplarDevice(current_config_);
}

PoplarExecutor::~PoplarExecutor() {}

void* PoplarExecutor::Allocate(uint64 size) {
  void* raw_buf = new char[size + sizeof(TensorControl)];
  TensorControl* allocated = new (raw_buf) TensorControl();
  allocated->size = size;
  allocated->ref_count = 1;
  allocated->on_device = false;
  allocated->input_handle.clear();
  allocated->output_handle.clear();
  allocated->output_convertor = nullptr;
  allocated->converted_data.clear();
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    allocations_.push_back(allocated);
  }
  return allocated;
}

void* PoplarExecutor::AllocateSubBuffer(se::DeviceMemoryBase* parent,
                                        uint64 offset_bytes,
                                        uint64 size_bytes) {
  TensorControl* tc = reinterpret_cast<TensorControl*>(parent->opaque());
  return tc->data + offset_bytes;
}

void PoplarExecutor::Deallocate(se::DeviceMemoryBase* mem) {
  if (!mem->is_sub_buffer()) {
    bool free = false;
    TensorControl* tc = reinterpret_cast<TensorControl*>(mem->opaque());
    {
      std::lock_guard<std::recursive_mutex> g(mutex_);
      if (--tc->ref_count == 0) {
        allocations_.remove(tc);
        free = true;
      }
    }
    if (free) {
      tc->~TensorControl();
      delete[] static_cast<char*>(mem->opaque());
    }
  }
}

bool PoplarExecutor::Memcpy(se::Stream* stream, void* host_dst,
                            const se::DeviceMemoryBase& pop_src, uint64 size) {
  AsPoplarStream(stream)->EnqueueTask([this, host_dst, pop_src, size]() {
    Status ok = SynchronousMemcpy(host_dst, pop_src, size);
  });
  return true;
}

bool PoplarExecutor::Memcpy(se::Stream* stream, se::DeviceMemoryBase* pop_dst,
                            const void* host_src, uint64 size) {
  se::DeviceMemoryBase dst = *pop_dst;
  AsPoplarStream(stream)->EnqueueTask([this, dst, host_src, size]() mutable {
    Status ok = SynchronousMemcpy(&dst, host_src, size);
  });
  return true;
}

Status PoplarExecutor::SynchronousMemcpy(se::DeviceMemoryBase* pop_dst,
                                         const void* host_src, uint64 size) {
  TensorControl* tc = reinterpret_cast<TensorControl*>(pop_dst->opaque());
  memcpy(tc->data, host_src, size);
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    tc->on_device = false;
    tc->input_handle.clear();
  }
  return Status::OK();
}

Status PoplarExecutor::SynchronousMemcpy(void* host_dst,
                                         const se::DeviceMemoryBase& pop_src,
                                         uint64 size) {
  const TensorControl* tc =
      reinterpret_cast<const TensorControl*>(pop_src.opaque());
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (tc->on_device == true && !tc->output_handle.empty()) {
      TF_RETURN_IF_ERROR(MoveDeviceToHost());
    }
  }
  memcpy(host_dst, tc->data, size);
  return Status::OK();
}

bool PoplarExecutor::HostCallback(se::Stream* stream,
                                  std::function<void()> callback) {
  AsPoplarStream(stream)->EnqueueTask(callback);
  return true;
}

bool PoplarExecutor::CreateStreamDependency(se::Stream* dependent,
                                            se::Stream* other) {
  AsPoplarStream(dependent)->EnqueueTask(
      [other]() { auto ok = other->BlockHostUntilDone(); });
  AsPoplarStream(dependent)->BlockUntilDone();
  return true;
}

bool PoplarExecutor::StartTimer(se::Stream* stream, se::Timer* timer) {
  dynamic_cast<se::host::HostTimer*>(timer->implementation())->Start(stream);
  return true;
}

bool PoplarExecutor::StopTimer(se::Stream* stream, se::Timer* timer) {
  dynamic_cast<se::host::HostTimer*>(timer->implementation())->Stop(stream);
  return true;
}

Status PoplarExecutor::BlockHostUntilDone(se::Stream* stream) {
  AsPoplarStream(stream)->BlockUntilDone();
  return Status::OK();
}

bool PoplarExecutor::SynchronizeAllActivity() {
  // TODO actually ensure that all execution has finished
  return true;
}

se::DeviceDescription* PoplarExecutor::PopulateDeviceDescription() const {
  se::internal::DeviceDescriptionBuilder builder;

  builder.set_name("Poplar");
  const auto version =
      poplar::versionString() + " (" + poplar::packageHash() + ")";
  builder.set_platform_version(version);

  auto built = builder.Build();
  return built.release();
}

std::string PoplarExecutor::GetDeviceTargetName() const {
  return poplar::toString(poplar_device_.getTarget().getTargetType());
}

static bool DeviceConfigurationsEqual(
    const tensorflow::IPUOptions::DeviceConfig& a,
    const tensorflow::IPUOptions::DeviceConfig& b) {
  return google::protobuf::util::MessageDifferencer::Equivalent(a, b);
}

Status PoplarExecutor::ConfigurePoplarDevice(
    const tensorflow::IPUOptions::DeviceConfig& cfg) {
  if (!DeviceConfigurationsEqual(cfg, current_config_) || !device_open_) {
    current_config_ = cfg;

    tensorflow::IPUOptions::DeviceConfig::Type type = cfg.type();

    static poplar::DeviceManager device_mgr =
        poplar::DeviceManager::getDeviceManager();

    int num_ipus = cfg.ipu_model_config().num_ipus();
    int tiles_per_ipu = cfg.ipu_model_config().tiles_per_ipu();

    if (num_ipus == 0) {
      num_ipus = 1;
    }

    try {
      // Only log the device_id when device type has been specified
      bool log_device_id =
          type != tensorflow::IPUOptions::DeviceConfig::DEFAULT;
      auto device_list =
          device_mgr.getDevices(poplar::TargetType::IPU, num_ipus);

      if (type == tensorflow::IPUOptions::DeviceConfig::DEFAULT) {
        if (device_list.size() > 0 && ordinal_ == 0) {
          type = tensorflow::IPUOptions::DeviceConfig::IPU;
        } else {
          type = tensorflow::IPUOptions::DeviceConfig::CPU;
        }
      }

      if (device_open_) {
        VLOG(1) << "Detaching poplar device type " << GetDeviceTargetName();
        poplar_device_.detach();
        device_open_ = false;
      }

      auto statusor_device_config_index = GetDeviceConfigIndex();
      bool opened = false;
      switch (type) {
        case tensorflow::IPUOptions::DeviceConfig::IPU: {
          // If a specific device has been requested, then attach to it,
          // otherwise attach to the first device available.
          if (statusor_device_config_index.ok()) {
            const auto device_config_index =
                statusor_device_config_index.ValueOrDie();
            if (device_config_index >= device_list.size()) {
              return InvalidArgument(
                  "Requested device configuration index %d, but %d "
                  "configurations were available.",
                  device_config_index, device_list.size());
            }
            poplar_device_ = std::move(device_list.at(device_config_index));
            if (poplar_device_.attach()) {
              opened = true;
            } else {
              return InternalError(
                  "Could not attach to the device configuration index "
                  "requested.");
            }
          } else {
            for (auto& d : device_list) {
              if (d.getTarget().getTargetType() == poplar::TargetType::IPU) {
                if (d.attach()) {
                  poplar_device_ = std::move(d);
                  opened = true;
                  break;
                }
              }
            }
          }
          if (opened) {
            unsigned mj, mn, pt;
            poplar_device_.getDriverVersion(mj, mn, pt);
            VLOG(1) << "Poplar driver: " << mj << "." << mn << "." << pt;

            if (tiles_per_ipu > 0) {
              poplar_device_ =
                  poplar_device_.createVirtualDevice(tiles_per_ipu);
            }
            if (log_device_id) {
              // Log the device ID's in the current config
              std::stringstream ss;
              ss << "Attached to IPU";
              if (poplar_device_.getDriverIDs().size() > 1) {
                ss << "s";
              }
              ss << ": ";
              auto first_pass = true;
              for (const auto& id : poplar_device_.getDriverIDs()) {
                if (first_pass) {
                  first_pass = false;
                } else {
                  ss << ", ";
                }
                ss << id;
              }
              LOG(INFO) << ss.str();
            }
          }
          break;
        }
        case tensorflow::IPUOptions::DeviceConfig::IPU_MODEL: {
          if (statusor_device_config_index.ok()) {
            const auto device_config_index =
                statusor_device_config_index.ValueOrDie();
            // We only allow one configutation for IPU_MODEL
            if (device_config_index != 0) {
              return InvalidArgument(
                  "Requested device configuration index %d, but 1 "
                  "configuration was available.",
                  device_config_index);
            }
          }
          poplar::IPUModel model;
          if (num_ipus != 0) {
            model.numIPUs = num_ipus;
          }
          if (tiles_per_ipu != 0) {
            model.tilesPerIPU = tiles_per_ipu;
          }
          poplar_device_ = model.createDevice();
          if (poplar_device_.attach()) {
            opened = true;
          }
          break;
        }
        case tensorflow::IPUOptions::DeviceConfig::CPU: {
          if (statusor_device_config_index.ok()) {
            const auto device_config_index =
                statusor_device_config_index.ValueOrDie();
            // We only allow one configutation for CPU
            if (device_config_index != 0) {
              return InvalidArgument(
                  "Requested device configuration index %d, but 1 "
                  "configuration was available.",
                  device_config_index);
            }
          }
          poplar_device_ = poplar::Device::createCPUDevice();
          if (poplar_device_.attach()) {
            opened = true;
          }
          break;
        }
        default:
          return xla::InternalError(
              "Unrecognized poplar device type for ordinal %d: %d", ordinal_,
              type);
      }

      if (!opened) {
        return xla::ResourceExhausted(
            "Unable to acquire poplar device type for ordinal %d", ordinal_);
      }
    } catch (poplar::poplar_error e) {
      return xla::InternalError(
          "Unable to open poplar device for ordinal %d: %s", ordinal_,
          e.what());
    }

    VLOG(1) << "Attached poplar device type " << GetDeviceTargetName();
    device_open_ = true;

    option_flags_ = poplar::OptionFlags();
    option_flags_.set("target.workerStackSizeInBytes", "0x200");

    // Device specific options
    switch (type) {
      case tensorflow::IPUOptions::DeviceConfig::IPU: {
        if (current_config_.profiling().enable_execution_trace()) {
          // Enable getting the cycle counts for each compute set on hardware
          // when asking for an execution trace
          option_flags_.set("debug.executionProfile", "compute_sets");
        }
        break;
      }
      default: { break; }
    }

    for (const auto& opt : cfg.compilation_options()) {
      option_flags_.set(opt.option(), opt.value());
    }

    // Cache Target hash
    std::vector<int64> poplar_target;
    const auto& target = poplar_device_.getTarget();
    poplar_target.push_back(target.getNumTiles());
    poplar_target.push_back(target.getDataPathWidth());
    poplar_target.push_back(target.getBytesPerTile());
    poplar_target.push_back(target.getNumWorkerContexts());
    poplar_target.push_back(target.getTilesPerIPU());
    poplar_target.push_back(target.getNumIPUs());
    poplar_target.push_back((unsigned)target.getTargetType());

    for (int64 h : poplar_target) {
      poplar_device_hash_ = tensorflow::Hash64Combine(poplar_device_hash_, h);
    }
  }

  return Status::OK();
}

bool PoplarExecutor::HaveExecutableCache() const {
  return !current_config_.engine_cache_directory().empty();
}

std::string PoplarExecutor::CachedExecutableFilename(
    const HloModule& module) const {
  HloHash module_hash(&module);
  uint64 hash = module_hash.GetHash();
  hash = tensorflow::Hash64Combine(hash, poplar_device_hash_);

  std::string filename = tensorflow::strings::Printf("%0llx.xla_engine", hash);

  const auto& dir = current_config_.engine_cache_directory();

  return tensorflow::io::JoinPath(dir, filename);
}

bool PoplarExecutor::HaveCachedExecutable(const std::string& filename) const {
  return false;
}

tensorflow::IpuTraceEvent PoplarExecutor::NewTraceEvent() {
  uint64 now = tensorflow::Env::Default()->NowMicros();
  tensorflow::IpuTraceEvent evt;
  evt.set_timestamp(static_cast<double>(now) / 1000000.0);
  evt.set_ordinal(ordinal_);
  return evt;
}

void PoplarExecutor::AddCompileBeginEventRecord(const std::string& module_name,
                                                const std::string& xla_graph) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::COMPILE_BEGIN);
  evt.mutable_compile_begin()->set_module_name(std::move(module_name));
  evt.mutable_compile_begin()->set_xla_graph(std::move(xla_graph));

  reports_.push_back(evt);
};

void PoplarExecutor::AddCompileEndEventRecord(const std::string& module_name,
                                              const std::string& report,
                                              const std::string& tensor_map,
                                              int64 duration) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::COMPILE_END);
  evt.mutable_compile_end()->set_module_name(std::move(module_name));
  evt.mutable_compile_end()->set_compilation_report(std::move(report));
  evt.mutable_compile_end()->set_duration(duration);
  evt.mutable_compile_end()->set_tensor_map(tensor_map);

  reports_.push_back(evt);
}

void PoplarExecutor::AddHostToDeviceEventRecord(const std::string& json) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::HOST_TO_DEVICE_TRANSFER);
  evt.mutable_data_transfer()->set_data_transfer(std::move(json));

  reports_.push_back(evt);
}

void PoplarExecutor::AddDeviceToHostEventRecord(const std::string& json) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::DEVICE_TO_HOST_TRANSFER);
  evt.mutable_data_transfer()->set_data_transfer(std::move(json));

  reports_.push_back(evt);
}

void PoplarExecutor::AddLoadEngineEventRecord(const std::string& module_name) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::LOAD_ENGINE);
  evt.mutable_load_engine()->set_module_name(std::move(module_name));

  reports_.push_back(evt);
}

void PoplarExecutor::AddExecuteEventRecord(const std::string& module_name,
                                           const std::string& report,
                                           const std::string& trace) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::EXECUTE);
  evt.mutable_execute()->set_module_name(std::move(module_name));
  evt.mutable_execute()->set_execution_report(std::move(report));
  evt.mutable_execute()->set_activity_trace(std::move(trace));

  reports_.push_back(evt);
}

const poprand::RandomGenMode PoplarExecutor::GetRandomGenMode() const {
  switch (current_config_.random_type()) {
    case tensorflow::IPUOptions::DeviceConfig::NOT_REPEATABLE:
      return poprand::NOT_REPEATABLE;
    case tensorflow::IPUOptions::DeviceConfig::SYSTEM_REPEATABLE:
      return poprand::SYSTEM_REPEATABLE;
    case tensorflow::IPUOptions::DeviceConfig::ALWAYS_REPEATABLE:
      return poprand::ALWAYS_REPEATABLE;
    default:
      return poprand::NOT_REPEATABLE;
  }
}

Status PoplarExecutor::GetCompilerEvents(
    std::list<tensorflow::IpuTraceEvent>& out) {
  std::lock_guard<std::recursive_mutex> g(mutex_);
  out.splice(out.end(), std::move(reports_));
  reports_.clear();
  return Status::OK();
}

void PoplarExecutor::FlattenedDeviceMemoryList(
    InputPairList& list, const xla::Shape& shape, void* base,
    const InputOutputAliasingMap::InputInfo& input_info) {
  TensorControl* tc = static_cast<TensorControl*>(base);
  if (xla::ShapeUtil::IsTuple(shape)) {
    void** ptrs = reinterpret_cast<void**>(tc->data);
    for (unsigned int t = 0; t < xla::ShapeUtil::TupleElementCount(shape);
         t++) {
      void* ptr = ptrs[t];
      FlattenedDeviceMemoryList(list,
                                xla::ShapeUtil::GetTupleElementShape(shape, t),
                                ptr, input_info);
    }
  } else {
    list.push_back(InputDef(tc, GetInputConversionFunction(shape),
                            input_info.IsStreaming()));
  }
}

void PoplarExecutor::UpdateArgsHandleMap(
    const Args& args, const xla::poplarplugin::PoplarExecutable& executable) {
  args_map_.clear();

  const auto* comp = executable.module().entry_computation();
  std::vector<xla::Shape> shapes(comp->num_parameters());
  for (const auto& inst : comp->parameter_instructions()) {
    shapes[inst->parameter_number()] = inst->shape();
  }

  const auto& inputs_info =
      executable.GetInputOutputAliasingMap().GetEntryInputInfos();
  CHECK_EQ(inputs_info.size(), args.size());
  CHECK_EQ(shapes.size(), args.size());
  for (unsigned int a = 0; a < inputs_info.size(); a++) {
    const auto& input_info = inputs_info[a];
    InputPairList bufs;
    FlattenedDeviceMemoryList(bufs, shapes[a],
                              const_cast<void*>(args[a].opaque()), input_info);
    for (unsigned i = 0; i < bufs.size(); i++) {
      args_map_[GetInputCopyHandle(a, i)] = bufs[i];
    }
  }
}

void PoplarExecutor::FlattenedOutputDeviceMemoryList(
    OutputPairList& list, const xla::Shape& shape, void* base,
    const InputOutputAliasingMap::OutputInfo& output_info) {
  TensorControl* tc = static_cast<TensorControl*>(base);
  if (xla::ShapeUtil::IsTuple(shape)) {
    void** ptrs = reinterpret_cast<void**>(tc->data);
    for (unsigned int t = 0; t < xla::ShapeUtil::TupleElementCount(shape);
         t++) {
      void* ptr = ptrs[t];
      FlattenedOutputDeviceMemoryList(
          list, xla::ShapeUtil::GetTupleElementShape(shape, t), ptr,
          output_info);
    }
  } else {
    list.push_back(OutputDef(tc, output_info.IsStreaming()));
  }
}

void PoplarExecutor::UpdateOutputsHandleMap(
    const xla::poplarplugin::PoplarExecutable& executable,
    const xla::Shape& shape, se::DeviceMemoryBase retbuf) {
  outputs_map_.clear();

  // Get all output pointers and their shapes
  std::vector<void*> outputs;
  std::vector<xla::Shape> shapes;

  if (ShapeUtil::IsTuple(shape)) {
    TensorControl* tc = static_cast<TensorControl*>(retbuf.opaque());
    void** ptrs = reinterpret_cast<void**>(tc->data);
    for (int64 i = 0; i < ShapeUtil::TupleElementCount(shape); i++) {
      shapes.push_back(ShapeUtil::GetTupleElementShape(shape, i));
      outputs.push_back(ptrs[i]);
    }
  } else {
    shapes.push_back(shape);
    outputs.push_back(retbuf.opaque());
  }

  // For all outputs
  const auto& outputs_info =
      executable.GetInputOutputAliasingMap().GetEntryOutputInfos();
  CHECK_EQ(outputs_info.size(), shapes.size());
  CHECK_EQ(outputs.size(), shapes.size());
  for (unsigned int a = 0; a < outputs_info.size(); a++) {
    const auto& output_info = outputs_info[a];
    OutputPairList bufs;
    FlattenedOutputDeviceMemoryList(bufs, shapes[a], outputs[a], output_info);
    for (unsigned i = 0; i < bufs.size(); i++) {
      outputs_map_[bufs[i].tc->output_handle] = bufs[i];
    }
  }
}

se::DeviceMemoryBase PoplarExecutor::ConstantOutputAllocation::GetAllocation(
    xla::DeviceMemoryAllocator* allocator, const xla::Shape& shape,
    const int64 output_index, int64& flat_tensor_index, const Args&,
    const InputOutputAliasingMap::OutputInfo&, const ArgsHandleMap&,
    const int) const {
  const auto& constant = constants_[output_index][flat_tensor_index];
  const int64 size(xla::ShapeUtil::ByteSizeOf(shape));
  se::DeviceMemoryBase allocated =
      allocator->Allocate(0, size, false).ConsumeValueOrDie().Forget();
  TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());
  tc->size = size;
  tc->on_device = false;
  tc->output_handle = std::string();
  tc->output_convertor = nullptr;

  void* buf(static_cast<void*>(tc->data));
  memcpy(buf, constant.untyped_data(), constant.size_bytes());
  return allocated;
}

se::DeviceMemoryBase PoplarExecutor::RemapOutputAllocation::GetAllocation(
    xla::DeviceMemoryAllocator*, const xla::Shape&, const int64 output_index,
    int64& flat_tensor_index, const Args& args,
    const InputOutputAliasingMap::OutputInfo&, const ArgsHandleMap& args_map,
    const int) const {
  const auto& remap_idx = remap_map_[output_index];
  auto it = args_map.find(GetInputCopyHandle(remap_idx, flat_tensor_index));
  if (it == args_map.end()) {
    LOG(FATAL) << "Could not remap an output to input tensor.";
  }
  TensorControl* tc = it->second.tc;
  tc->ref_count++;
  return se::DeviceMemoryBase(tc);
}

se::DeviceMemoryBase PoplarExecutor::BufferOutputAllocation::GetAllocation(
    xla::DeviceMemoryAllocator* allocator, const xla::Shape& shape,
    const int64 output_index, int64& flat_tensor_index, const Args& args,
    const InputOutputAliasingMap::OutputInfo& output_info,
    const ArgsHandleMap& args_map, const int ordinal) const {
  int64 size(xla::ShapeUtil::ByteSizeOf(shape));
  if (output_info.IsResourceModified()) {
    // The output is an in-place update of one of the inputs
    // TODO: is this a multi-threading bug?
    auto it = args_map.find(
        GetInputCopyHandle(output_info.GetInputIndex(), flat_tensor_index));
    if (it == args_map.end()) {
      LOG(FATAL) << "Could not find matching input resource tensor.";
    }
    TensorControl* tc = it->second.tc;
    tc->size = size;
    tc->on_device = output_info.IsStreaming() ? false : true;
    tc->ref_count++;
    tc->output_handle = GetOutputCopyHandle(output_index, flat_tensor_index);
    tc->output_convertor = GetOutputConversionFunction(shape);
    return se::DeviceMemoryBase(tc);
  } else {
    // The output is not one of the inputs
    se::DeviceMemoryBase allocated =
        allocator->Allocate(ordinal, size, false).ConsumeValueOrDie().Forget();
    TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());
    tc->size = size;
    tc->on_device = output_info.IsStreaming() ? false : true;
    tc->output_handle = GetOutputCopyHandle(output_index, flat_tensor_index);
    tc->output_convertor = GetOutputConversionFunction(shape);
    return allocated;
  }
}

se::DeviceMemoryBase PoplarExecutor::HandleOutputBuffer(
    xla::DeviceMemoryAllocator* allocator,
    const PoplarExecutor::OutputAllocation& allocation_info,
    const xla::Shape& shape, const int64 output_index, int64& flat_tensor_index,
    const Args& args, const InputOutputAliasingMap::OutputInfo& output_info) {
  if (!ShapeUtil::IsTuple(shape)) {
    se::DeviceMemoryBase buf = allocation_info.GetAllocation(
        allocator, shape, output_index, flat_tensor_index, args, output_info,
        args_map_, ordinal_);
    flat_tensor_index++;
    return buf;
  } else {
    int64 size(xla::ShapeUtil::ByteSizeOf(shape, sizeof(void*)));
    se::DeviceMemoryBase allocated =
        allocator->Allocate(0, size, false).ConsumeValueOrDie().Forget();
    TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());

    void** buf = reinterpret_cast<void**>(tc->data);
    for (int64 i = 0; i < xla::ShapeUtil::TupleElementCount(shape); i++) {
      se::DeviceMemoryBase out = HandleOutputBuffer(
          allocator, allocation_info, shape.tuple_shapes(i), output_index,
          flat_tensor_index, args, output_info);
      *buf++ = out.opaque();
    }
    return se::DeviceMemoryBase(tc, size);
  }
}

se::DeviceMemoryBase PoplarExecutor::GetOutputBuffer(
    const xla::poplarplugin::PoplarExecutable& executable,
    xla::DeviceMemoryAllocator* allocator,
    const PoplarExecutor::OutputAllocation& allocation_info,
    const xla::Shape& shape, const Args& args,
    const InputOutputAliasingMap& input_output_aliasing_map) {
  // Get all output shapes
  std::vector<xla::Shape> shapes;
  const int64 size = ShapeUtil::IsTuple(shape)
                         ? xla::ShapeUtil::ByteSizeOf(shape, sizeof(void*))
                         : xla::ShapeUtil::ByteSizeOf(shape);

  if (ShapeUtil::IsTuple(shape)) {
    for (int64 i = 0; i < ShapeUtil::TupleElementCount(shape); i++) {
      shapes.push_back(ShapeUtil::GetTupleElementShape(shape, i));
    }
  } else {
    shapes.push_back(shape);
  }

  std::vector<void*> ptrs;
  // For all outputs
  // Call a recursive function HandleOutputBuffer for each output instruction
  const auto& outputs_info =
      executable.GetInputOutputAliasingMap().GetEntryOutputInfos();
  CHECK_EQ(outputs_info.size(), shapes.size());
  for (unsigned int idx = 0; idx < shapes.size(); idx++) {
    const auto& output_info =
        input_output_aliasing_map.GetEntryOutputInfos()[idx];
    int64 start_flat_tensor_index = 0;
    se::DeviceMemoryBase out =
        HandleOutputBuffer(allocator, allocation_info, shapes[idx], idx,
                           start_flat_tensor_index, args, output_info);
    ptrs.push_back(out.opaque());
  }
  if (ShapeUtil::IsTuple(shape)) {
    se::DeviceMemoryBase allocated =
        allocator->Allocate(0, size, false).ConsumeValueOrDie().Forget();
    TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());
    void** buf = reinterpret_cast<void**>(tc->data);
    for (void* ptr : ptrs) {
      *buf++ = ptr;
    }
    return se::DeviceMemoryBase(tc, size);
  } else {
    CHECK_EQ(ptrs.size(), 1);
    return se::DeviceMemoryBase(ptrs[0]);
  }
}

// Takes a tensor and returns a pointer to a buffer with the data in the right
// format
void* PoplarExecutor::PreProcessBuffer(InputDef& id) {
  TensorControl* tc = id.tc;
  void* buf(static_cast<void*>(tc->data));
  if (id.fn != nullptr) {
    tc->converted_data = id.fn(buf, tc->size, 0);
    buf = tc->converted_data.data();
  }
  return buf;
}

// Convers the data into the right host format
void PoplarExecutor::PostProcessBuffer(TensorControl* tc) {
  if (tc->output_convertor) {
    void* buf(static_cast<void*>(tc->data));
    std::vector<char> converted = tc->output_convertor(buf, 0, tc->size);
    memcpy(buf, converted.data(), converted.size());
  }
}

StatusOr<bool> PoplarExecutor::CheckMoveDeviceToHostRequired(
    const bool engine_changed) {
  // Pull previous execution outputs back from device if:
  // a) one is on the device _and_
  // b)   the engine is changing _or_
  // c)   output buffer isn't an input to the current engine _or_
  // d)   output buffer isn't currently in the right place for the new input
  bool do_device_to_host = false;
  for (const auto& tc : allocations_) {
    if (tc->on_device == true && !tc->output_handle.empty()) {
      if (engine_changed || args_map_.count(tc->input_handle) == 0 ||
          tc != args_map_.at(tc->input_handle).tc) {
        do_device_to_host = true;
      }
    }
  }
  return do_device_to_host;
}

StatusOr<bool> PoplarExecutor::CheckMoveHostToDeviceRequired(
    const bool engine_changed) {
  // Put resources on the device if:
  // a) the engine has changed
  // b) resource is not on the device
  // c) resource is on the device, but in the wrong place
  bool do_host_to_device = false;
  for (const auto& arg : args_map_) {
    if (!arg.second.streamed) {
      auto it =
          std::find(allocations_.begin(), allocations_.end(), arg.second.tc);
      if (it == allocations_.end()) {
        return tensorflow::errors::InvalidArgument(
            "Argument isn't allocated on device: ", (void*)arg.second.tc);
      }
      if (engine_changed || arg.second.tc->on_device == false ||
          arg.second.tc->input_handle != arg.first) {
        do_host_to_device = true;
      }
    }
  }
  return do_host_to_device;
}

Status PoplarExecutor::MoveDeviceToHost() {
  if (UseSyntheticData()) {
    return Status::OK();
  }

  Json::Value root;
  root["tensors"] = Json::Value(Json::arrayValue);
  uint64 total_size = 0;

  for (const auto& tc : allocations_) {
    // Set up streams
    if (tc->on_device == true && !tc->output_handle.empty()) {
      void* buf(static_cast<void*>(tc->data));
      current_engine_->connectStream(tc->output_handle, buf);

      Json::Value tensor;
      tensor["name"] = Json::Value(tc->output_handle);
      tensor["size"] = Json::Value::UInt64(tc->size);
      root["tensors"].append(tensor);
      total_size += tc->size;
    }
  }
  root["total_size"] = Json::Value::UInt64(total_size);
  Json::StreamWriterBuilder json_builder;
  std::string json_msg = Json::writeString(json_builder, root);

  // perform device -> host read
  try {
    current_engine_->run(PoplarProgramType::DEVICE_TO_HOST);
  } catch (const std::logic_error& e) {
    return PoplarExceptionToTensorflowStatus("[Device to host] ", e);
  }

  if (current_config_.profiling().enable_io_trace()) {
    AddDeviceToHostEventRecord(json_msg);
  }

  // Post process upload
  for (const auto& tc : allocations_) {
    if (tc->on_device == true && !tc->output_handle.empty()) {
      PostProcessBuffer(tc);
    }

    tc->on_device = false;
    tc->output_handle.clear();
    tc->input_handle.clear();
  }

  return Status::OK();
}

Status PoplarExecutor::MoveHostToDevice() {
  if (UseSyntheticData()) {
    return Status::OK();
  }
  try {
    Json::Value root;
    root["tensors"] = Json::Value(Json::arrayValue);
    uint64 total_size = 0;

    for (auto arg : args_map_) {
      TensorControl* tc = arg.second.tc;
      std::vector<std::pair<std::string, int64>> stream_list;
      void* buf(static_cast<void*>(tc->data));
      if (!arg.second.streamed) {
        buf = PreProcessBuffer(arg.second);

        current_engine_->connectStream(arg.first, buf);

        tc->on_device = true;
        tc->input_handle = arg.first;

        Json::Value tensor;
        tensor["name"] = Json::Value(arg.first);
        tensor["size"] = Json::Value::UInt64(tc->size);
        root["tensors"].append(tensor);
        total_size += tc->size;

        stream_list.push_back(std::make_pair(arg.first, 0));
      }
    }
    root["total_size"] = Json::Value::UInt64(total_size);
    Json::StreamWriterBuilder json_builder;
    std::string json_msg = Json::writeString(json_builder, root);

    current_engine_->run(PoplarProgramType::HOST_TO_DEVICE);

    if (current_config_.profiling().enable_io_trace()) {
      AddHostToDeviceEventRecord(json_msg);
    }

    for (auto arg : args_map_) {
      TensorControl* tc = arg.second.tc;
      tc->converted_data.clear();
    }
  } catch (const std::logic_error& e) {
    return PoplarExceptionToTensorflowStatus("[Host to device] ", e);
  }

  return Status::OK();
}

StatusOr<se::DeviceMemoryBase> PoplarExecutor::GetTupleBufferByIndex(
    const se::DeviceMemoryBase& base, int64 value) {
  const TensorControl* tc =
      reinterpret_cast<const TensorControl*>(base.opaque());
  void** bufs = (void**)tc->data;
  int64 size = reinterpret_cast<const TensorControl*>(bufs[value])->size;

  return se::DeviceMemoryBase(bufs[value], size);
}

void PoplarExecutor::ConnectStreamedVariablesHostToDevice() {
  // Don't connect any streams if using synthetic data
  if (UseSyntheticData()) {
    return;
  }

  for (auto arg : args_map_) {
    if (arg.second.streamed) {
      void* buf = PreProcessBuffer(arg.second);
      current_engine_->connectStream(arg.first, buf);
    }
  }
}

void PoplarExecutor::ConnectStreamedVariablesDeviceToHost() {
  // Don't connect any streams if using synthetic data
  if (UseSyntheticData()) {
    return;
  }

  for (auto output : outputs_map_) {
    if (output.second.streamed) {
      TensorControl* tc = output.second.tc;
      current_engine_->connectStream(output.first,
                                     static_cast<void*>(tc->data));
    }
  }
}

void PoplarExecutor::PostProcessStreamedVariablesDeviceToHost() {
  for (auto output : outputs_map_) {
    if (output.second.streamed) {
      PostProcessBuffer(output.second.tc);
    }
  }
}

StatusOr<se::DeviceMemoryBase> PoplarExecutor::ExecuteEngine(
    perftools::gputools::StreamExecutor* executor,
    xla::poplarplugin::PoplarExecutable& executable,
    xla::DeviceMemoryAllocator* allocator, const Args& args) {
  const auto& input_output_aliasing_map =
      executable.GetInputOutputAliasingMap();
  const auto& output_shape = executable.result_shape();
  const auto& engine = executable.Engine();

  perftools::gputools::DeviceMemoryBase retbuf;

  bool engine_changed(current_engine_ != engine);
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);

    UpdateArgsHandleMap(args, executable);

    if (engine == NULL) {
      // An empty engine is either a graph that just passes its inputs through
      // to its outputs, or a graph which returns a constant.
      if (executable.IsConstantGraph()) {
        retbuf =
            GetOutputBuffer(executable, allocator,
                            ConstantOutputAllocation(executable.LiteralValue()),
                            output_shape, args, input_output_aliasing_map);
      } else if (executable.IsRemapGraph()) {
        retbuf = GetOutputBuffer(executable, allocator,
                                 RemapOutputAllocation(executable.RemapMap()),
                                 output_shape, args, input_output_aliasing_map);
      } else {
        LOG(FATAL) << "Cannot construct a NULL graph.";
      }
    } else {
      if (!executable.has_module()) {
        return tensorflow::errors::InvalidArgument(
            "Executable must have an HloModule");
      }

      TF_ASSIGN_OR_RETURN(const bool move__device_to_host,
                          CheckMoveDeviceToHostRequired(engine_changed));

      if (move__device_to_host) {
        MoveDeviceToHost();
      }

      if (engine_changed) {
        try {
          engine->load(poplar_device_);

          if (current_config_.profiling().enable_io_trace()) {
            AddLoadEngineEventRecord(executable.module().name());
          }

          executable.OnEngineLoaded();

          current_engine_ = engine;

        } catch (const std::logic_error& e) {
          return PoplarExceptionToTensorflowStatus("[Load engine ]", e);
        }
      }

      TF_ASSIGN_OR_RETURN(const bool move__host_to_device,
                          CheckMoveHostToDeviceRequired(engine_changed));
      if (move__host_to_device) {
        MoveHostToDevice();
      }

      retbuf = GetOutputBuffer(executable, allocator, BufferOutputAllocation(),
                               output_shape, args, input_output_aliasing_map);

      UpdateOutputsHandleMap(executable, output_shape, retbuf);

      VLOG(1) << "Executing on poplar stream ordinal " << ordinal_
              << " of type " << GetDeviceTargetName();

      try {
        // Connect the streams to and from the device
        ConnectStreamedVariablesHostToDevice();
        ConnectStreamedVariablesDeviceToHost();

        // Run the main engine
        current_engine_->run(PoplarProgramType::MAIN_SEQUENCE);

        // We need to call post process to make sure all the data is in the
        // right format on the host
        PostProcessStreamedVariablesDeviceToHost();

      } catch (const std::logic_error& e) {
        return PoplarExceptionToTensorflowStatus("[Execute engine] ", e);
      }

      try {
        if (current_config_.profiling().enable_execution_trace() > 0) {
          poplar::OptionFlags opts;
          opts.set("doLayerWiseBreakdown", "true");
          if (!CompilerReportingTextFormat()) {
            opts.set("doLayerWisePerIPUBreakdown", "true");
            opts.set("doLayerWisePerTileBreakdown", "true");
          }

          std::stringstream report_stream;
          std::stringstream trace_stream;
          if (executable.ExecutionCount() == 0) {
            auto rep = current_engine_->getExecutionReport(opts);
            if (CompilerReportingTextFormat()) {
              rep.printSummary(report_stream);
            } else {
              rep.serialize(report_stream, poplar::SerializationFormat::JSON);
            }

            current_engine_->reportIntervals(trace_stream);
          }

          AddExecuteEventRecord(executable.module().name(), report_stream.str(),
                                trace_stream.str());
        }
      } catch (const std::logic_error& e) {
        return PoplarExceptionToTensorflowStatus("[Execute engine] ", e);
      }
    }
  }

  return retbuf;
}

}  // namespace poplarplugin
}  // namespace xla
