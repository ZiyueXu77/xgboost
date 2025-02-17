/**
 * Copyright 2023-2024, XGBoost contributors
 *
 * Higher level functions built on top the Communicator API, taking care of behavioral differences
 * between row-split vs column-split distributed training, and horizontal vs vertical federated
 * learning.
 */
#pragma once
#include <limits>
#include <string>
#include <utility>

#include "allreduce.h"
#include "broadcast.h"
#include "comm.h"
#include "communicator-inl.h"
#include "xgboost/collective/result.h"  // for Result
#include "xgboost/data.h"               // for MetaINfo

#if defined(XGBOOST_USE_FEDERATED)
#include "../../plugin/federated/federated_comm.h"
#endif  // defined(XGBOOST_USE_FEDERATED)

namespace xgboost::collective {
namespace detail {
// Apply function fn, and handle potential errors.
template <typename Fn>
[[nodiscard]] Result TryApplyWithLabels(Context const* ctx, Fn&& fn) {
  std::string msg;
  if (collective::GetRank() == 0) {
    try {
      fn();
    } catch (dmlc::Error const& e) {
      msg = e.what();
    }
  }
  // Error handling
  std::size_t msg_size{msg.size()};
  auto rc = Success() << [&] {
    return collective::Broadcast(ctx, linalg::MakeVec(&msg_size, 1), 0);
  } << [&] {
    if (msg_size > 0) {
      msg.resize(msg_size);
      return collective::Broadcast(ctx, linalg::MakeVec(msg.data(), msg.size()), 0);
    }
    return Success();
  } << [&] {
    if (msg_size > 0) {
      LOG(FATAL) << msg;
    }
    return Success();
  };
  return rc;
}
}  // namespace detail

/**
 * @brief Apply the given function where the labels are.
 *
 * Normally all the workers have access to the labels, so the function is just applied locally. In
 * vertical federated learning, we assume labels are only available on worker 0, so the function is
 * applied there, with the results broadcast to other workers.
 *
 * @tparam Function The function used to calculate the results.
 * @param info MetaInfo about the DMatrix.
 * @param buffer The buffer storing the results.
 * @param size The size of the buffer.
 * @param function The function used to calculate the results.
 */
template <typename Fn>
void ApplyWithLabels(Context const* ctx, MetaInfo const& info, void* buffer, std::size_t size,
                     Fn&& fn) {
  if (info.IsVerticalFederated()) {
    auto rc = detail::TryApplyWithLabels(ctx, fn) << [&] {
      // We assume labels are only available on worker 0, so the calculation is done there and
      // result broadcast to other workers.
      return collective::Broadcast(
          ctx, linalg::MakeVec(reinterpret_cast<std::int8_t*>(buffer), size), 0);
    };
    SafeColl(rc);
  } else {
    std::forward<Fn>(fn)();
  }
}

/**
 * @brief Apply the given function where the labels are.
 *
 * Normally all the workers have access to the labels, so the function is just applied locally. In
 * vertical federated learning, we assume labels are only available on worker 0, so the function is
 * applied there, with the results broadcast to other workers.
 *
 * @tparam T Type of the HostDeviceVector storing the results.
 * @tparam Function The function used to calculate the results.
 * @param info MetaInfo about the DMatrix.
 * @param result The HostDeviceVector storing the results.
 * @param function The function used to calculate the results.
 */
template <typename T, typename Fn>
void ApplyWithLabels(Context const* ctx, MetaInfo const& info, HostDeviceVector<T>* result,
                     Fn&& fn) {
  if (info.IsVerticalFederated()) {
    // We assume labels are only available on worker 0, so the calculation is done there
    // and result is broadcasted to other workers.
    auto rc = detail::TryApplyWithLabels(ctx, std::forward<Fn>(fn));
    // Broadcast the result
    std::size_t size{result->Size()};
    rc = std::move(rc) << [&] {
      return collective::Broadcast(ctx, linalg::MakeVec(&size, 1), 0);
    } << [&] {
      result->Resize(size);
      return collective::Broadcast(ctx, linalg::MakeVec(result->HostPointer(), size), 0);
    };
    SafeColl(rc);
  } else {
    fn();
  }
}

/**
 * @brief Find the global max of the given value across all workers.
 *
 * This only applies when the data is split row-wise (horizontally). When data is split
 * column-wise (vertically), the local value is returned.
 *
 * @tparam T The type of the value.
 * @param info MetaInfo about the DMatrix.
 * @param value The input for finding the global max.
 * @return The global max of the input.
 */
template <typename T>
std::enable_if_t<std::is_trivially_copy_assignable_v<T>, T> GlobalMax(Context const* ctx,
                                                                      MetaInfo const& info,
                                                                      T value) {
  if (info.IsRowSplit()) {
    auto rc = collective::Allreduce(ctx, linalg::MakeVec(&value, 1), collective::Op::kMax);
    SafeColl(rc);
  }
  return value;
}

/**
 * @brief Find the global sum of the given values across all workers.
 *
 * This only applies when the data is split row-wise (horizontally). When data is split
 * column-wise (vertically), the original values are returned.
 *
 * @tparam T The type of the values.
 * @param info MetaInfo about the DMatrix.
 * @param values Pointer to the inputs to sum.
 * @param size Number of values to sum.
 */
template <typename T, std::int32_t kDim>
[[nodiscard]] Result GlobalSum(Context const* ctx, MetaInfo const& info,
                               linalg::TensorView<T, kDim> values) {
  if (info.IsRowSplit()) {
    return collective::Allreduce(ctx, values, collective::Op::kSum);
  }
  return Success();
}

/**
 * @brief Find the global ratio of the given two values across all workers.
 *
 * This only applies when the data is split row-wise (horizontally). When data is split
 * column-wise (vertically), the local ratio is returned.
 *
 * @tparam T The type of the values.
 * @param info MetaInfo about the DMatrix.
 * @param dividend The dividend of the ratio.
 * @param divisor The divisor of the ratio.
 * @return The global ratio of the two inputs.
 */
template <typename T>
T GlobalRatio(Context const* ctx, MetaInfo const& info, T dividend, T divisor) {
  std::array<T, 2> results{dividend, divisor};
  auto rc = GlobalSum(ctx, info, linalg::MakeVec(results.data(), results.size()));
  SafeColl(rc);
  std::tie(dividend, divisor) = std::tuple_cat(results);
  if (divisor <= 0) {
    return std::numeric_limits<T>::quiet_NaN();
  } else {
    return dividend / divisor;
  }
}

/**
 * @brief Broadcast the gradient for federated learning.
 *
 * We need to handle three different cases here:
 * - Normal training, handled in the apply with labels.
 * - Federated non-encrypted, handled in the apply with labels.
 * - Federated encrypted, need to sync with the plugin.
 */
template <typename GradFn>
void BroadcastGradient(Context const* ctx, MetaInfo const& info, GradFn&& grad_fn,
                       linalg::Matrix<GradientPair>* out_gpair) {
  if (info.IsVerticalFederated() && IsEncrypted()) {
#if defined(XGBOOST_USE_FEDERATED)
    // Need to encrypt the gradient before broadcasting.
    common::Span<std::uint8_t> encrypted;
    auto const& comm = GlobalCommGroup()->Ctx(ctx, DeviceOrd::CPU());
    auto const& fed = dynamic_cast<FederatedComm const&>(comm);
    if (GetRank() == 0) {
      // Obtain the gradient
      grad_fn(out_gpair);
      auto values = out_gpair->HostView().Values();
      // Encrypt the gradient
      static_assert(sizeof(GradientPair) == sizeof(float) * 2);
      auto casted = reinterpret_cast<float const*>(values.data());
      auto data = common::Span{casted, values.size() * 2};

      encrypted = fed.EncryptionPlugin()->EncryptGradient(data);
    }
    // Broadcast the gradient
    std::uint64_t n_bytes = encrypted.size();
    HostDeviceVector<std::uint8_t> grad;
    auto rc = Success() << [&] {
      return Broadcast(ctx, linalg::MakeVec(&n_bytes, 1), 0);
    } << [&] {
      if (GetRank() != 0) {
        grad.Resize(n_bytes);
        encrypted = grad.HostSpan();
      }
      return Broadcast(ctx, linalg::MakeVec(encrypted), 0);
    };
    SafeColl(rc);
    // Pass the gradient to the plugin
    fed.EncryptionPlugin()->SyncEncryptedGradient(encrypted);

    // !!!Temporarily solution
    // This step is needed for memory allocation in the case of vertical secure GPU
    // make out_gpair data value to all zero to avoid information leak
    auto gpair_data = out_gpair->Data();
    gpair_data->Fill(GradientPair{0.0f, 0.0f});
    ApplyWithLabels(ctx, info, gpair_data, [&] { grad_fn(out_gpair); });
#else
    LOG(FATAL) << error::NoFederated();
#endif
  } else {
    ApplyWithLabels(ctx, info, out_gpair->Data(), [&] { grad_fn(out_gpair); });
  }
}
}  // namespace xgboost::collective
