#define TORCH_ASSERT_ONLY_METHOD_OPERATORS
#include <sstream>

#include <ATen/core/dispatch/Dispatcher.h>
#include <ATen/core/ivalue.h>
#include <ATen/core/stack.h>
#include <ATen/native/CPUFallback.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/Functions.h>
#else
#include <ATen/ops/_copy_from_and_resize.h>
#include <ATen/ops/_to_cpu.h>
#endif

namespace dipu {
namespace native {

// convenience helper for converting tensors to cpu

std::vector<at::Tensor> to_cpu(const at::TensorList& tensors) {
  // We can't just call at::to_cpu() on the entire list of Tensors
  // Because it will break on undefined tensors. Separate out undefined tensors
  // first.
  std::vector<at::Tensor> cpu_tensors(tensors.size());
  std::vector<at::Tensor> valid_tensors;
  std::vector<bool> to_translate(tensors.size());
  for (const auto i : c10::irange(tensors.size())) {
    const at::Tensor& tensor = tensors[i];
    // Explicitly handling undefined tensors here instead of letting
    // `at::_to_cpu` handle it. Otherwise, we'd need to require all backends
    // with their own implementation of _to_cpu to properly handle undefined
    // tensors.
    if (tensor.defined()) {
      to_translate[i] = true;
      valid_tensors.push_back(tensor);
    } else {
      cpu_tensors[i] = tensor;
    }
  }
  auto cpu_valid_tensors = at::_to_cpu(valid_tensors);
  for (size_t i = 0, defined_pos = 0; i < tensors.size(); ++i) {
    if (to_translate[i]) {
      cpu_tensors[i] = std::move(cpu_valid_tensors[defined_pos++]);
    }
  }
  return cpu_tensors;
}

c10::optional<c10::Device> compute_target_device(
    std::vector<at::Tensor>& t_args,
    std::vector<c10::List<at::Tensor>> tlist_args) {
  // Decide what device to move the output tensor(s) to.
  // The current convention is that we use the first tensor arg to pick the
  // device Barring that, we take the first tensor from a TensorList arg.
  if (!t_args.empty()) {
    return t_args[0].device();
  } else {
    // We need to loop through all of the (potentially multiple) TensorList
    // arguments In case, e.g. the first one is empty but the second is not.
    for (auto& tens_list : tlist_args) {
      for (const auto i : c10::irange(tens_list.size())) {
        return tens_list.get(i).device();
      }
    }
  }
  return c10::nullopt;
}

void cpu_fallback(const c10::OperatorHandle& op, torch::jit::Stack* stack) {
  auto& schema_args = op.schema().arguments();
  const auto num_arguments = schema_args.size();
  auto arguments = torch::jit::last(stack, num_arguments);
  const auto arguments_begin = stack->size() - num_arguments;

  std::vector<at::Tensor> tensor_args;
  std::vector<int> tensor_args_indices;

  std::vector<c10::List<at::Tensor>> tensorlist_args;
  std::vector<c10::List<at::Tensor>> cpu_tensorlist_args;
  std::vector<int> tensorlist_args_indices;

  static bool log_fallback_detail =
      std::getenv("DIPU_LOG_FALLBACK_INFO") != nullptr;

  // Step 1: Convert all non-CPU tensor inputs into CPU tensors
  // and put them on the stack at the correct indices.
  for (const auto idx : c10::irange(arguments.size())) {
    const auto& ivalue = arguments[idx];
    if (log_fallback_detail) {
      std::cout << "cpu_fallback:\t" << op.schema().name() << "\t arguments["
                << idx << "] :" << schema_args[idx].name()
                << "\ttype:" << schema_args[idx].type()
                << "\tisTensor:" << ivalue.isTensor()
                << "\tisTensorList:" << ivalue.isTensorList()
                << "\talias_info:" << schema_args[idx].alias_info()
                << std::endl;
    }

    if (ivalue.isTensor()) {
      tensor_args.push_back(ivalue.toTensor());
      tensor_args_indices.push_back(idx);
    } else if (ivalue.isTensorList()) {
      // Note: we copy each TensorList argument to CPU individually out of
      // convenience, but XLA would benefit from materializing all tensor and
      // TensorList args onto the CPU at the same time. We can improve this if
      // we need better perf for XLA's CPU fallbacks.
      tensorlist_args.push_back(ivalue.toTensorList());
      auto cpu_ivalue = c10::IValue(
          c10::List<at::Tensor>(to_cpu(ivalue.toTensorList().vec())));
      (*stack)[arguments_begin + idx] = std::move(cpu_ivalue);
      cpu_tensorlist_args.push_back(
          (*stack)[arguments_begin + idx].toTensorList());
      tensorlist_args_indices.push_back(idx);
    }
  }
  // XLA requires all of the tensor arguments to be gathered up and converted to
  // CPU together.
  auto cpu_tensors = to_cpu(tensor_args);

  for (const auto i : c10::irange(tensor_args_indices.size())) {
    auto idx = tensor_args_indices[i];
    (*stack)[arguments_begin + idx] = c10::IValue(cpu_tensors[i]);
  }

  // Step 2: Call the underlying CPU implementation of the operator
  op.redispatchBoxed(c10::DispatchKeySet(c10::DispatchKey::CPU), stack);

  static bool force_copy_tensor =
      std::getenv("DIPU_DISABLE_FORCE_FALLBACK_COPY_TENSOR") == nullptr;

  // Step 3: We need to take special care to handle mutable aliases properly:
  // If any input tensors are mutable aliases, we need to
  // directly copy the updated data on the CPU tensors back to the original
  // inputs.
  for (const auto i : c10::irange(tensor_args_indices.size())) {
    auto tensor_idx = tensor_args_indices[i];
    const at::AliasInfo* alias_info = schema_args[tensor_idx].alias_info();
    if ((alias_info != nullptr && alias_info->isWrite())) {
      if (log_fallback_detail) {
        std::cout << "write back: " << tensor_idx << ":"
                  << cpu_tensors[i].options()
                  << ",size:" << cpu_tensors[i].sizes() << ", "
                  << tensor_args[i].options()
                  << ",size:" << cpu_tensors[i].sizes() << std::endl;
      }
      tensor_args[i].reshape_as(cpu_tensors[i]).copy_(cpu_tensors[i], false);
    }
  }
  for (const auto i : c10::irange(tensorlist_args_indices.size())) {
    auto tensorlist_idx = tensorlist_args_indices[i];
    const at::AliasInfo* alias_info = schema_args[tensorlist_idx].alias_info();
    if ((alias_info != nullptr && alias_info->isWrite())) {
      c10::List<at::Tensor> cpu_tensorlist = cpu_tensorlist_args[i];
      std::vector<at::Tensor> tensorlist = tensorlist_args[i].vec();
      for (auto j = 0; j < tensorlist.size(); j++) {
        if (cpu_tensorlist.get(j).defined()) {
          tensorlist[j]
              .reshape_as(cpu_tensorlist.get(j))
              .copy_(cpu_tensorlist.get(j), false);
        }
        if (log_fallback_detail) {
          std::cout << "write back " << tensorlist_idx << "th args " << j
                    << "th tensor:" << cpu_tensorlist.get(j).sizes()
                    << cpu_tensorlist.get(j).options() << tensorlist[j].sizes()
                    << tensorlist[j].options() << std::endl;
        }
      }
      (*stack)[arguments_begin + tensorlist_idx] =
          c10::IValue(c10::List<at::Tensor>(tensorlist));
    }
  }
  // Step 4: Convert any CPU output tensors back to the original input device.
  // For mutable alias'd outputs, we also need to take special care
  // to move the ORIGINAL input tensor back onto the stack, in place of
  // the temporary CPU output tensor that we created.
  //
  // Note [CPU Fallback Does Not Handle View Operators]
  // Also note that we are incapable of handling immutable alises properly.
  // Why?
  // Schemas with an immutable alias'd tensor outputs correspond to view
  // operators. For example, the `view_as` schema from native_functions.yaml:
  // `view_as(Tensor(a) self, Tensor other) -> Tensor(a)`
  // We can't handle these ops properly, because view ops are supposed to return
  // a NEW tensor that shares the SAME storage as the original tensor.
  // However, the new tensor that we created cannot share the same storage,
  // since it lives on CPU and the original tensor lives on a different device.
  // Because of that, we warn if someone attempts to call the
  // CPU fallback on a view operator (this is to maintain BC for view ops for
  // XLA that fall back to CPU).
  const auto& schema_returns = op.schema().returns();
  const auto& num_returns = schema_returns.size();
  auto returns = torch::jit::last(stack, num_returns);
  const auto returns_begin = stack->size() - num_returns;

  for (const auto idx : c10::irange(returns.size())) {
    if (returns[idx].isTensor()) {
      const auto& return_tens = returns[idx].toTensor();
      if (return_tens.defined()) {
        const at::AliasInfo* alias_info = schema_returns[idx].alias_info();
        if (alias_info != nullptr && alias_info->isWrite()) {
          // Case (1): mutable alias case. Move the input ivalue directly onto
          // the stack in place of the existing cpu output tensor.
          bool found_alias = false;
          // We could store some extra metadata on the function schema to avoid
          // the loop here if we need to improve perf.
          for (const auto i : c10::irange(tensor_args_indices.size())) {
            auto input_tensor_idx = tensor_args_indices[i];
            const auto& input_tensor = cpu_tensors[i];
            const at::AliasInfo* input_alias_info =
                schema_args[input_tensor_idx].alias_info();
            // Checked above; adding assert to guard against breakage of the
            // below condition due to changing the above if test.
            TORCH_INTERNAL_ASSERT_DEBUG_ONLY(alias_info != nullptr);
            if (input_tensor.defined() &&
                (alias_info == input_alias_info ||
                 (input_alias_info != nullptr &&
                  *alias_info == *input_alias_info))) {
              // We've found the original input tensor that aliases with the
              // current output. Wrap it in an IValue and put it directly on the
              // stack.
              (*stack)[returns_begin + idx] = c10::IValue(tensor_args[i]);
              found_alias = true;
              break;
            }
          }
          TORCH_CHECK(found_alias, "The operator ", op.schema().operator_name(),
                      " appears to have invalid alias information. ",
                      "Found a return tensor argument with a mismatched "
                      "mutable alias: ",
                      schema_returns[idx]);
        } else {
          c10::optional<c10::Device> tgt_device =
              compute_target_device(tensor_args, tensorlist_args);
          if (alias_info != nullptr && !alias_info->isWrite()) {
            // immutable alias (view) case: Warn here, since we're copying and
            // not creating a view.
            // If this operator is needed, the backend should provide a kernel
            // for it.
            // See Note [CPU Fallback Does Not Handle View Operators]
            std::stringstream dev_str;
            if (tgt_device) {
              dev_str << *tgt_device;
            } else {
              dev_str << "<none>";
            }
            TORCH_WARN(false, "The operator ", op.schema().operator_name(),
                       " appears to be a view operator, ",
                       "but it has no implementation for the backend \"",
                       dev_str.str(), "\". View operators don't support ",
                       "falling back to run on the CPU, since the tensor's "
                       "storage cannot be shared across devices.");
          }
          // Case (2): copy case. Copy the cpu output tensor to the original
          // device.

          // We technically  might not have a target device, e.g. if you call
          // torch.cat() with an empty list In that case, we shouldn't have any
          // tensors to schlep across devices anyway.
          if (tgt_device) {
            (*stack)[returns_begin + idx] =
                c10::IValue(returns[idx].toTensor().to(*tgt_device));
          }
        }
      }
    }
  }
}

}  // namespace native
}  // namespace dipu
