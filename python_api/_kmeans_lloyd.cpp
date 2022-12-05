#include <cstdint>
#include <vector>
#include <utility>
#include <iostream>
#include <CL/sycl.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "dpctl4pybind11.hpp"

#include "util_kernels.hpp"
#include "compute_euclidean_distance.hpp"
#include "assignment.hpp"

namespace py = pybind11;

template <std::size_t num>
bool all_c_contiguous(const dpctl::tensor::usm_ndarray (&args)[num]) {
  bool all_contig = true;
  for (size_t i = 0; all_contig && i < num; ++i) {
    all_contig = args[i].is_c_contiguous();
  }
  return all_contig;
}


/*! @brief Evaluates X /= y */
std::pair<sycl::event, sycl::event>
py_broadcast_divide(
  dpctl::tensor::usm_ndarray X,
  dpctl::tensor::usm_ndarray y,
  sycl::queue q,
  const std::vector<sycl::event> &depends={})
{
  int X_ndim = X.get_ndim();
  int y_ndim = y.get_ndim();

  if (X_ndim != 2 || y_ndim != 1 || !all_c_contiguous({X, y})) {
    throw py::value_error("Arguments must be a matrix and a vector with C-contiguous layout");
  }

  if (y.get_shape(0) != X.get_shape(1)) {
    throw py::value_error("Array dimensions of arguments are not consistent, X.shape[1] != y.shape[0]");
  }

  if (!dpctl::utils::queues_are_compatible(q, {X.get_queue(), y.get_queue()})) {
    throw py::value_error("Execution queue is not compatible with allocation queues");
  }

  auto y_typenum = y.get_typenum();
  if (X.get_typenum() != y_typenum) {
    throw py::value_error("Arguments must have the same elemental data types.");
  }

  auto &api = dpctl::detail::dpctl_capi::get();

  sycl::event comp_ev; 
  if (y_typenum == api.UAR_FLOAT_) {
    comp_ev = broadcast_division_kernel<float>(
              q, X.get_shape(0), X.get_shape(1), 32, X.get_data<float>(), y.get_data<float>(), depends
	   );
  } else if (y_typenum == api.UAR_DOUBLE_) {
    comp_ev = broadcast_division_kernel<double>(
              q, X.get_shape(0), X.get_shape(1), 32, X.get_data<double>(), y.get_data<double>(), depends
	   );
  } else {
    throw py::value_error("Unsupported elemental data type. Expecting single or double precision floating point numbers");
  }

  sycl::event ht_ev = ::dpctl::utils::keep_args_alive(q, {X, y}, {comp_ev});
  return std::make_pair(ht_ev, comp_ev);
}

/*! @brief Evaluates y = np.square(np.linalg.norm(X, axis=1)) / 2 */
std::pair<sycl::event, sycl::event>
py_half_l2_norm_squared(
  dpctl::tensor::usm_ndarray X,
  dpctl::tensor::usm_ndarray y,
  sycl::queue q,
  const std::vector<sycl::event> &depends={})
{
  int X_ndim = X.get_ndim();
  int y_ndim = y.get_ndim();

  if (X_ndim != 2 || y_ndim != 1 || !all_c_contiguous({X, y})) {
    throw py::value_error("Arguments must be a matrix and a vector with C-contiguous layout");
  }

  if (y.get_shape(0) != X.get_shape(1)) {
    throw py::value_error("Array dimensions of arguments are not consistent, X.shape[1] != y.shape[0]");
  }

  if (!dpctl::utils::queues_are_compatible(q, {X.get_queue(), y.get_queue()})) {
    throw py::value_error("Execution queue is not compatible with allocation queues");
  }

  auto y_typenum = y.get_typenum();
  if (X.get_typenum() != y_typenum) {
    throw py::value_error("Arguments must have the same elemental data types.");
  }

  auto &api = dpctl::detail::dpctl_capi::get();

  sycl::event comp_ev; 
  if (y_typenum == api.UAR_FLOAT_) {
    comp_ev = half_l2_norm_kernel<float>(
              q, X.get_shape(0), X.get_shape(1), 32, X.get_data<float>(), y.get_data<float>(), depends
	   );
  } else if (y_typenum == api.UAR_DOUBLE_) {
    comp_ev = half_l2_norm_kernel<double>(
              q, X.get_shape(0), X.get_shape(1), 32, X.get_data<double>(), y.get_data<double>(), depends
	   );
  } else {
    throw py::value_error("Unsupported elemental data type. Expecting single or double precision floating point numbers");
  }

  sycl::event ht_ev = ::dpctl::utils::keep_args_alive(q, {X, y}, {comp_ev});
  return std::make_pair(ht_ev, comp_ev);
}


std::pair<sycl::event, sycl::event>
py_reduce_centroids_data(
  dpctl::tensor::usm_ndarray cluster_sizes_private_copies, // IN (n_copies, n_clusters)                dataT
  dpctl::tensor::usm_ndarray centroids_t_private_copies,   // IN (n_copies, n_features, n_clusters,)   dataT
  dpctl::tensor::usm_ndarray out_cluster_sizes,            // OUT (n_clusters,)                        dataT
  dpctl::tensor::usm_ndarray out_centroids_t,              // OUT (n_features, n_clusters,)            dataT
  dpctl::tensor::usm_ndarray out_empty_clusters_list,      // OUT (n_clusters,)                        indT
  dpctl::tensor::usm_ndarray out_n_empty_clusters,         // OUT (1,)                                 indT
  sycl::queue q,
  const std::vector<sycl::event> &depends={}
) {
  if (cluster_sizes_private_copies.get_ndim() != 2 ||
      centroids_t_private_copies.get_ndim() != 3 ||
      out_cluster_sizes.get_ndim() != 1 ||
      out_centroids_t.get_ndim() != 2 ||
      out_empty_clusters_list.get_ndim() != 1 ||
      out_n_empty_clusters.get_size() != 1)
  {
    throw py::value_error("Array dimensions of inputs are not consistent");
  }

  py::ssize_t n_copies = cluster_sizes_private_copies.get_shape(0);
  py::ssize_t n_clusters = cluster_sizes_private_copies.get_shape(1);
  py::ssize_t n_features = centroids_t_private_copies.get_shape(1);

  if ( n_copies != centroids_t_private_copies.get_shape(0) ||
       n_clusters != centroids_t_private_copies.get_shape(2) ||
       n_clusters != out_cluster_sizes.get_shape(0) ||
       n_clusters != out_centroids_t.get_shape(1) ||
       n_features != out_centroids_t.get_shape(0) ||
       n_clusters != out_empty_clusters_list.get_shape(0) )
  {
    throw py::value_error("Dimensions mismatch");
  }

  int dataT_typenum = cluster_sizes_private_copies.get_typenum();
  int indT_typenum = out_n_empty_clusters.get_typenum();

  if (dataT_typenum != centroids_t_private_copies.get_typenum() ||
      dataT_typenum != out_cluster_sizes.get_typenum() ||
      dataT_typenum != out_centroids_t.get_typenum() ||
      indT_typenum != out_empty_clusters_list.get_typenum() )
    {
      throw py::value_error("Array element data types must be consisten");
    }

  if (!all_c_contiguous({
        cluster_sizes_private_copies, centroids_t_private_copies,
        out_cluster_sizes, out_centroids_t, out_empty_clusters_list,
        out_n_empty_clusters
      }))
  {
    throw py::value_error("All array arguments must be C-contiguous");
  }

  if (! ::dpctl::utils::queues_are_compatible(q, {
						  cluster_sizes_private_copies.get_queue(),
						  centroids_t_private_copies.get_queue(),
						  out_cluster_sizes.get_queue(),
						  out_centroids_t.get_queue(),
						  out_empty_clusters_list.get_queue(),
						  out_n_empty_clusters.get_queue()
      })) {
    throw py::value_error("Execution queue is not compatible with allocation queues");
  }

  auto &api = ::dpctl::detail::dpctl_capi::get();

  sycl::event comp_ev;
  constexpr size_t work_group_size = 256;
  
  if (dataT_typenum == api.UAR_FLOAT_ && indT_typenum == api.UAR_INT32_) {
    using dataT = float;
    using indT = std::int32_t;
    comp_ev = reduce_centroid_data_kernel<dataT, indT>(
	    q, n_copies, n_features, n_clusters, work_group_size,
	    cluster_sizes_private_copies.get_data<dataT>(),
	    centroids_t_private_copies.get_data<dataT>(),
	    out_cluster_sizes.get_data<dataT>(),
	    out_centroids_t.get_data<dataT>(),
	    out_empty_clusters_list.get_data<indT>(),
	    out_n_empty_clusters.get_data<indT>(),
	    depends);
  } else if (dataT_typenum == api.UAR_FLOAT_ && indT_typenum == api.UAR_INT64_) {
    using dataT = float;
    using indT = std::int64_t;
    comp_ev = reduce_centroid_data_kernel<dataT, indT>(
	    q, n_copies, n_features, n_clusters, work_group_size,
	    cluster_sizes_private_copies.get_data<dataT>(),
	    centroids_t_private_copies.get_data<dataT>(),
	    out_cluster_sizes.get_data<dataT>(),
	    out_centroids_t.get_data<dataT>(),
	    out_empty_clusters_list.get_data<indT>(),
	    out_n_empty_clusters.get_data<indT>(),
	    depends);
  } else if (dataT_typenum == api.UAR_DOUBLE_ && indT_typenum == api.UAR_INT32_) {
    using dataT = double;
    using indT = std::int32_t;
    comp_ev = reduce_centroid_data_kernel<dataT, indT>(
	    q, n_copies, n_features, n_clusters, work_group_size,
	    cluster_sizes_private_copies.get_data<dataT>(),
	    centroids_t_private_copies.get_data<dataT>(),
	    out_cluster_sizes.get_data<dataT>(),
	    out_centroids_t.get_data<dataT>(),
	    out_empty_clusters_list.get_data<indT>(),
	    out_n_empty_clusters.get_data<indT>(),
	    depends);
  } else if (dataT_typenum == api.UAR_DOUBLE_ && indT_typenum == api.UAR_INT64_) {
    using dataT = double;
    using indT = std::int64_t;
    comp_ev = reduce_centroid_data_kernel<dataT, indT>(
	    q, n_copies, n_features, n_clusters, work_group_size,
	    cluster_sizes_private_copies.get_data<dataT>(),
	    centroids_t_private_copies.get_data<dataT>(),
	    out_cluster_sizes.get_data<dataT>(),
	    out_centroids_t.get_data<dataT>(),
	    out_empty_clusters_list.get_data<indT>(),
	    out_n_empty_clusters.get_data<indT>(),
	    depends);
  } else {
    throw py::value_error("Unsupported data types");
  }

  sycl::event ht_ev = ::dpctl::utils::keep_args_alive(
       q, {cluster_sizes_private_copies, centroids_t_private_copies,
	   out_cluster_sizes, out_centroids_t, out_empty_clusters_list,
	   out_n_empty_clusters}, {comp_ev});

  return std::make_pair(ht_ev, comp_ev);
}

std::pair<sycl::event, sycl::event>
py_compute_threshold(
  dpctl::tensor::usm_ndarray data,
  size_t topk,
  dpctl::tensor::usm_ndarray threshold,
  sycl::queue q,
  const std::vector<sycl::event> &depends = {}
) {
  if (data.get_ndim() !=1 || !all_c_contiguous({data, threshold})) {
    throw py::value_error("Argument data must be a C-contiguous vector");
  }

  if (threshold.get_size() != 1) {
    throw py::value_error("Argument threshold must be 1-element array");
  }

  int data_typenum = data.get_typenum();
  if(data_typenum != threshold.get_typenum()) {
    throw py::value_error("Data types of arguments must be the same");
  }

  if (! ::dpctl::utils::queues_are_compatible(q, {data.get_queue(), threshold.get_queue()})) {
    throw py::value_error("Execution queue is not compatible with allocation queues");
  }

  auto &api = ::dpctl::detail::dpctl_capi::get();
  py::ssize_t n_samples = data.get_shape(0);

  if (topk == 0) {
    sycl::event e1{};
    sycl::event e2{};
    return std::make_pair(e1, e2);
  }

  sycl::event comp_ev;
  if (data_typenum == api.UAR_FLOAT_) {
    using dataT = float;
    comp_ev = compute_threshold_kernel<dataT>(q, 
      n_samples, data.get_data<dataT>(), topk, 
      threshold.get_data<dataT>(), depends);
  } else if (data_typenum == api.UAR_DOUBLE_) {
    using dataT = float;
    comp_ev = compute_threshold_kernel<dataT>(q, 
      n_samples, data.get_data<dataT>(), topk, 
      threshold.get_data<dataT>(), depends);
  } else {
    throw py::value_error("Unsupported elemental data type. Expect single- or double- floating-point types.");
  }

  sycl::event ht_ev = ::dpctl::utils::keep_args_alive(
       q, {data, threshold}, {comp_ev});

  return std::make_pair(ht_ev, comp_ev);
}

std::pair<sycl::event, sycl::event>
py_select_samples_far_from_centroid(
  size_t n_selected,
  dpctl::tensor::usm_ndarray distance_to_centroid,  // IN (n_samples, )    dataT
  dpctl::tensor::usm_ndarray threshold,             // IN (1,)             dataT
  dpctl::tensor::usm_ndarray selected_samples_idx,  // OUT (n_sample, )    indT
  dpctl::tensor::usm_ndarray n_selected_gt_threshold, // OUT (1,)          indT
  dpctl::tensor::usm_ndarray n_selected_eq_threshold, // OUT (1,)          indT
  sycl::queue q,
  const std::vector<sycl::event> &depends = {}
) {

  if (n_selected == 0) { throw py::value_error("Argument `n_selected` must be positive");}

  if (distance_to_centroid.get_ndim() != 1 || selected_samples_idx.get_ndim() != 1 ||
      threshold.get_size() != 1 || n_selected_gt_threshold.get_size() != 1 ||
      n_selected_eq_threshold.get_size() != 1
  ) {
    throw py::value_error("Array dimensionalities are not consistent");
  }

  py::ssize_t n_samples = distance_to_centroid.get_shape(0);
  if ( static_cast<size_t>(n_samples) < n_selected) {
    throw py::value_error("Argument `n_select` is too large");
  }

  if (selected_samples_idx.get_shape(0) < static_cast<py::ssize_t>(n_selected)) {
    throw py::value_error("Vector `selected_samples_idx` must have size of at least `n_selected` elements");
  }

  if (!all_c_contiguous({distance_to_centroid, selected_samples_idx})) {
    throw py::value_error("Arrays must be C-contiguous");
  }

  int dataT_typenum = distance_to_centroid.get_typenum();
  if (dataT_typenum != threshold.get_typenum()) {
    throw py::value_error("Input arrays must have the same elemental data type");
  }

  int indT_typenum = selected_samples_idx.get_typenum();

  if (indT_typenum != n_selected_gt_threshold.get_typenum() ||
      indT_typenum != n_selected_eq_threshold.get_typenum()
     ) {
      throw py::value_error("Output array must have the same elemental data type");
  }

  if (!dpctl::utils::queues_are_compatible(q, {
    distance_to_centroid.get_queue(),
    threshold.get_queue(), selected_samples_idx.get_queue(),
    n_selected_gt_threshold.get_queue(),
    n_selected_eq_threshold.get_queue()
  })) { throw py::value_error("Execution queue is not compatible with allocation queues"); }

  auto &api = ::dpctl::detail::dpctl_capi::get();
  size_t work_group_size = 64;

  sycl::event comp_ev; 
  if (dataT_typenum == api.UAR_FLOAT_ && indT_typenum == api.UAR_INT32_) {
    using dataT = float;
    using indT = std::int32_t;
    comp_ev = select_samples_far_from_centroid_kernel<dataT, indT>(
      q, n_selected, static_cast<size_t>(n_samples), work_group_size,
      distance_to_centroid.get_data<dataT>(), threshold.get_data<dataT>(),
      selected_samples_idx.get_data<indT>(), n_selected_gt_threshold.get_data<indT>(),
      n_selected_eq_threshold.get_data<indT>(), depends
    );
  } else if (dataT_typenum == api.UAR_DOUBLE_ && indT_typenum == api.UAR_INT32_) {
    using dataT = double;
    using indT = std::int32_t;
    comp_ev = select_samples_far_from_centroid_kernel<dataT, indT>(
      q, n_selected, static_cast<size_t>(n_samples), work_group_size,
      distance_to_centroid.get_data<dataT>(), threshold.get_data<dataT>(),
      selected_samples_idx.get_data<indT>(), n_selected_gt_threshold.get_data<indT>(),
      n_selected_eq_threshold.get_data<indT>(), depends
    );
  } else if (dataT_typenum == api.UAR_FLOAT_ && indT_typenum == api.UAR_INT64_) {
    using dataT = float;
    using indT = std::int64_t;
    comp_ev = select_samples_far_from_centroid_kernel<dataT, indT>(
      q, n_selected, static_cast<size_t>(n_samples), work_group_size,
      distance_to_centroid.get_data<dataT>(), threshold.get_data<dataT>(),
      selected_samples_idx.get_data<indT>(), n_selected_gt_threshold.get_data<indT>(),
      n_selected_eq_threshold.get_data<indT>(), depends
    );
  } else if (dataT_typenum == api.UAR_DOUBLE_ && indT_typenum == api.UAR_INT64_) {
    using dataT = double;
    using indT = std::int64_t;
    comp_ev = select_samples_far_from_centroid_kernel<dataT, indT>(
      q, n_selected, static_cast<size_t>(n_samples), work_group_size,
      distance_to_centroid.get_data<dataT>(), threshold.get_data<dataT>(),
      selected_samples_idx.get_data<indT>(), n_selected_gt_threshold.get_data<indT>(),
      n_selected_eq_threshold.get_data<indT>(), depends
    );
  } else {
    throw py::value_error("Unsupported data types");
  }

  sycl::event ht_ev = ::dpctl::utils::keep_args_alive(q, {
    distance_to_centroid, threshold, selected_samples_idx, 
    n_selected_gt_threshold, n_selected_eq_threshold}, {comp_ev});

  return std::make_pair(ht_ev, comp_ev);
}

std::pair<sycl::event, sycl::event>
py_relocate_empty_clusters(
  size_t n_empty_clusters,
  dpctl::tensor::usm_ndarray X_t,             // IN (n_features, n_samples, )  dataT
  dpctl::tensor::usm_ndarray sample_weights,  // IN (n_samples, )              dataT
  dpctl::tensor::usm_ndarray assignment_id,   // IN (n_samples, )              indT
  dpctl::tensor::usm_ndarray empty_clusters_list, // IN (n_clusters)           indT
  dpctl::tensor::usm_ndarray sq_dist_to_nearest_centroid, // IN (n_samples,)   dataT
  dpctl::tensor::usm_ndarray centroid_t,      // IN-OUT  (n_features, n_clusters, )  dataT
  dpctl::tensor::usm_ndarray cluster_sizes,   // IN-OUT  (n_clusters, )        dataT
  dpctl::tensor::usm_ndarray per_sample_inertia, // IN-OUT (n_samples, )       dataT
  sycl::queue q,
  const std::vector<sycl::event> &depends={}
) 
{
  if ( 2 != X_t.get_ndim() || 1 != sample_weights.get_ndim() || 
        1 != assignment_id.get_ndim() || 
        1 != empty_clusters_list.get_ndim() || 
        1 != sq_dist_to_nearest_centroid.get_ndim() ||
        2 != centroid_t.get_ndim() ||
        1 != cluster_sizes.get_ndim() ||
        1 != per_sample_inertia.get_ndim()
      )
  {
    throw py::value_error("Arguments have inconsistent array dimensionality.");
  }

  if (!all_c_contiguous({X_t, sample_weights, assignment_id, empty_clusters_list, 
                        sq_dist_to_nearest_centroid, centroid_t, cluster_sizes, per_sample_inertia}))
  {
    throw py::value_error("Inputs must be C-contiguous");
  }

  py::ssize_t n_samples = X_t.get_shape(1);
  py::ssize_t n_features = X_t.get_shape(0);
  py::ssize_t n_clusters = empty_clusters_list.get_shape(0);

  if (n_samples != sample_weights.get_shape(0)              ||
      n_samples != assignment_id.get_shape(0)               ||
      n_samples != sq_dist_to_nearest_centroid.get_shape(0) ||
      n_clusters != centroid_t.get_shape(1)                 ||
      n_features != centroid_t.get_shape(0)                 ||
      n_clusters != cluster_sizes.get_shape(0)              ||
      n_samples != per_sample_inertia.get_shape(0)
  ) 
  {
    throw py::value_error("Input dimensions are inconsistent");
  }

  if (n_empty_clusters == 0) {
    throw py::value_error("n_empty_clusters must be non-zero.");
  }

  if (!dpctl::utils::queues_are_compatible(q, {
        X_t.get_queue(), sample_weights.get_queue(), 
        assignment_id.get_queue(), empty_clusters_list.get_queue(), 
        sq_dist_to_nearest_centroid.get_queue(), centroid_t.get_queue(),
        cluster_sizes.get_queue(), per_sample_inertia.get_queue()
        }))
  {
    throw py::value_error("Execution queue is not compatible with allocation queues.");
  }

  int dataT_typenum = X_t.get_typenum();
  int indT_typenum = assignment_id.get_typenum();

  if (dataT_typenum != sample_weights.get_typenum() || 
      indT_typenum != empty_clusters_list.get_typenum() || 
      dataT_typenum != sq_dist_to_nearest_centroid.get_typenum() ||
      dataT_typenum != centroid_t.get_typenum() ||
      dataT_typenum != cluster_sizes.get_typenum() || 
      dataT_typenum != per_sample_inertia.get_typenum()) 
  {
    throw py::value_error("Inconsistent array elemental data types");
  }

  const auto &api = dpctl::detail::dpctl_capi::get();
  size_t work_group_size = 64;

  sycl::event comp_ev; 
  if (dataT_typenum == api.UAR_FLOAT_ && indT_typenum == api.UAR_INT32_) {
    using dataT = float;
    using indT = std::int32_t;

    comp_ev = relocate_empty_clusters<dataT, indT>(
      q,
      n_samples, n_features, static_cast<indT>(n_clusters), work_group_size,
      n_empty_clusters, X_t.get_data<dataT>(), sample_weights.get_data<dataT>(),
      assignment_id.get_data<indT>(), empty_clusters_list.get_data<indT>(),
      sq_dist_to_nearest_centroid.get_data<dataT>(), 
      centroid_t.get_data<dataT>(), cluster_sizes.get_data<dataT>(), 
      per_sample_inertia.get_data<dataT>(),
      depends
    );
  } else if (dataT_typenum == api.UAR_FLOAT_ && indT_typenum == api.UAR_INT64_) {
    using dataT = float;
    using indT = std::int64_t;

    comp_ev = relocate_empty_clusters<dataT, indT>(
      q,
      n_samples, n_features, static_cast<indT>(n_clusters), work_group_size,
      n_empty_clusters, X_t.get_data<dataT>(), sample_weights.get_data<dataT>(),
      assignment_id.get_data<indT>(), empty_clusters_list.get_data<indT>(),
      sq_dist_to_nearest_centroid.get_data<dataT>(), 
      centroid_t.get_data<dataT>(), cluster_sizes.get_data<dataT>(),
      per_sample_inertia.get_data<dataT>(),
      depends
    );
  } else if (dataT_typenum == api.UAR_DOUBLE_ && indT_typenum == api.UAR_INT32_) {
    using dataT = double;
    using indT = std::int32_t;

    comp_ev = relocate_empty_clusters<dataT, indT>(
      q,
      n_samples, n_features, static_cast<indT>(n_clusters), work_group_size,
      n_empty_clusters, X_t.get_data<dataT>(), sample_weights.get_data<dataT>(),
      assignment_id.get_data<indT>(), empty_clusters_list.get_data<indT>(),
      sq_dist_to_nearest_centroid.get_data<dataT>(), 
      centroid_t.get_data<dataT>(), cluster_sizes.get_data<dataT>(),
      per_sample_inertia.get_data<dataT>(),
      depends
    );
  } else if (dataT_typenum == api.UAR_DOUBLE_ && indT_typenum == api.UAR_INT64_) {
    using dataT = double;
    using indT = std::int64_t;

    comp_ev = relocate_empty_clusters<dataT, indT>(
      q,
      n_samples, n_features, static_cast<indT>(n_clusters), work_group_size,
      n_empty_clusters, X_t.get_data<dataT>(), sample_weights.get_data<dataT>(),
      assignment_id.get_data<indT>(), empty_clusters_list.get_data<indT>(),
      sq_dist_to_nearest_centroid.get_data<dataT>(), 
      centroid_t.get_data<dataT>(), cluster_sizes.get_data<dataT>(),
      per_sample_inertia.get_data<dataT>(),
      depends
    );
  } else {
    throw py::value_error("Unsupported data types");
  }

  sycl::event ht_ev = dpctl::utils::keep_args_alive(q, 
    { 
      X_t, sample_weights, assignment_id, empty_clusters_list, 
      sq_dist_to_nearest_centroid, centroid_t, cluster_sizes
    },
    {comp_ev}
  );

  return std::make_pair(ht_ev, comp_ev);
}

std::pair<sycl::event, sycl::event>
py_compute_centroid_shifts_squared_kernel(
  dpctl::tensor::usm_ndarray old_centroid_t,    // IN (n_features, n_clusters)
  dpctl::tensor::usm_ndarray new_centroid_t,    // IN (n_features, n_clusters)
  dpctl::tensor::usm_ndarray centroid_shifts,   // OUT (n_clusters)
  sycl::queue q,
  const std::vector<sycl::event> &depends={}
) {
  if (2 != new_centroid_t.get_ndim() || 
      2 != old_centroid_t.get_ndim() || 
      1 != centroid_shifts.get_ndim()
  ) {
    throw py::value_error("Input dimensionalities are not consistent.");
  }

  py::ssize_t n_features = old_centroid_t.get_shape(0);
  py::ssize_t n_clusters = old_centroid_t.get_shape(1);

  if (n_features != new_centroid_t.get_shape(0) ||
      n_clusters != new_centroid_t.get_shape(1) ||
      n_clusters != centroid_shifts.get_shape(0)
  ) {
    throw py::value_error("Array dimensions are not consistent.");
  }

  if (!all_c_contiguous({new_centroid_t, old_centroid_t, centroid_shifts})) {
    throw py::value_error("Arguments must be C-contiguous arrays");
  }

  if (!dpctl::utils::queues_are_compatible(q, {new_centroid_t.get_queue(), old_centroid_t.get_queue(), centroid_shifts.get_queue()})) {
    throw py::value_error("Execution queue is not compatible with allocation queues");
  }

  int typenum = old_centroid_t.get_typenum();
  if (typenum != new_centroid_t.get_typenum() || typenum != centroid_shifts.get_typenum()) {
    throw py::value_error("All array arguments must have the same elemental data types");
  }

  auto &api = ::dpctl::detail::dpctl_capi::get();
  size_t work_group_size = 128;

  sycl::event comp_ev;
  if (typenum == api.UAR_FLOAT_ ) {
    using dataT = float;

    comp_ev = compute_centroid_shifts_squared_kernel<dataT>(
      q, n_features, n_clusters, work_group_size, 
      old_centroid_t.get_data<dataT>(),
      new_centroid_t.get_data<dataT>(),
      centroid_shifts.get_data<dataT>(),
      depends
    );
  } else if (typenum == api.UAR_DOUBLE_) {
    using dataT = double;

    comp_ev = compute_centroid_shifts_squared_kernel<dataT>(
      q, n_features, n_clusters, work_group_size, 
      old_centroid_t.get_data<dataT>(),
      new_centroid_t.get_data<dataT>(),
      centroid_shifts.get_data<dataT>(),
      depends
    );

  } else {
    throw py::value_error("Unsupported elemental data type.");
  }

  sycl::event ht_ev = dpctl::utils::keep_args_alive(
    q, 
    {old_centroid_t, new_centroid_t, centroid_shifts}, {comp_ev}
  );

  return std::make_pair(ht_ev, comp_ev);
}

std::pair<sycl::event, sycl::event>
py_compute_distances(
  dpctl::tensor::usm_ndarray X_t,                    // IN (n_feautes, n_samples)
  dpctl::tensor::usm_ndarray centroid_t,             // IN (n_features, n_clusters)
  dpctl::tensor::usm_ndarray euclidean_distances_t,  // OUT (n_clusters, n_samples)
  size_t work_group_size, 
  size_t centroids_window_height, 
  sycl::queue q,
  const std::vector<sycl::event> &depends = {}
) {
  if ( 2 != X_t.get_ndim() || 2 != centroid_t.get_ndim() || 2 != euclidean_distances_t.get_ndim()) {
    throw py::value_error("Input arrays must have dimensionality 2.");
  }

  if (!all_c_contiguous({X_t, centroid_t, euclidean_distances_t})) {
    throw py::value_error("Input arrays must be C-contiguous.");
  }

  py::ssize_t n_features = X_t.get_shape(0);
  py::ssize_t n_samples = X_t.get_shape(1);
  py::ssize_t n_clusters = euclidean_distances_t.get_shape(0);

  if ( n_features != centroid_t.get_shape(0) || n_clusters != centroid_t.get_shape(1) || n_samples != euclidean_distances_t.get_shape(1)) {
    std::cout << (n_features != centroid_t.get_shape(0))
    << " " <<  (n_clusters != centroid_t.get_shape(1)) << " " << (n_samples != euclidean_distances_t.get_shape(1)) << std::endl;
    throw py::value_error("Input array dimensions are not consistant");
  }

  if (!dpctl::utils::queues_are_compatible(q, {X_t.get_queue(), centroid_t.get_queue(), euclidean_distances_t.get_queue()})) {
    throw py::value_error("Execution queue is not compatible with allocation queues");
  }

  int typenum = X_t.get_typenum();

  if (typenum != centroid_t.get_typenum() || typenum != euclidean_distances_t.get_typenum()) {
    throw py::value_error("Arrays must have the same elemental data types");
  }

  const auto &api = ::dpctl::detail::dpctl_capi::get();
  constexpr size_t preferred_work_group_size_multiplier = 8;
  constexpr size_t centroids_window_width_multiplier = 4;

  sycl::event comp_ev; 
  if (typenum == api.UAR_FLOAT_) {
    using dataT = float;

    comp_ev = compute_distances<dataT, preferred_work_group_size_multiplier, centroids_window_width_multiplier>(
      q, 
      n_samples, 
      n_features, 
      n_clusters,
      centroids_window_height,
      work_group_size,
      X_t.get_data<dataT>(),
      centroid_t.get_data<dataT>(), 
      euclidean_distances_t.get_data<dataT>(),
      depends
    );
  } else if (typenum == api.UAR_DOUBLE_) {
    using dataT = double;

    comp_ev = compute_distances<dataT, preferred_work_group_size_multiplier, centroids_window_width_multiplier>(
      q, 
      n_samples, 
      n_features, 
      n_clusters,
      centroids_window_height,
      work_group_size,
      X_t.get_data<dataT>(),
      centroid_t.get_data<dataT>(), 
      euclidean_distances_t.get_data<dataT>(),
      depends
    );
  } else {
    throw py::value_error("Unsupported elemental data type");
  }

  sycl::event ht_ev = dpctl::utils::keep_args_alive(q, 
    {X_t, centroid_t, euclidean_distances_t}, {comp_ev});

  return std::make_pair(ht_ev, comp_ev);   
}

std::pair<sycl::event, sycl::event>
py_assignment(
  dpctl::tensor::usm_ndarray X_t,        // IN (n_features, n_samples)
  dpctl::tensor::usm_ndarray centroid_t, // IN (n_features, n_clusters)
  dpctl::tensor::usm_ndarray centroids_half_l2_norm, // (n_clusters,)
  dpctl::tensor::usm_ndarray assignment_id,  // OUT (n_samples, )
  size_t centroids_window_height, 
  size_t work_group_size, 
  sycl::queue q,
  const std::vector<sycl::event> &depends={}
) {
  if ( 2 != X_t.get_ndim() || 2 != centroid_t.get_ndim() || 1 != centroids_half_l2_norm.get_ndim() || 1 != assignment_id.get_ndim()) {
    throw py::value_error("Inputs have unexpected dimensionality.");
  }

  if (!all_c_contiguous({X_t, centroid_t, centroids_half_l2_norm, assignment_id})) {
    throw py::value_error("Inputs must be C-contiguous arrays.");
  }

  py::ssize_t n_features = X_t.get_shape(0); 
  py::ssize_t n_samples = X_t.get_shape(1);
  py::ssize_t n_clusters = centroids_half_l2_norm.get_shape(0);

  if (n_features != centroid_t.get_shape(0) || n_clusters != centroid_t.get_shape(1) || n_samples != assignment_id.get_shape(0)) {
    throw py::value_error("Inputs have inconsistent dimensions.");
  } 

  if(!dpctl::utils::queues_are_compatible(q, {X_t.get_queue(), centroid_t.get_queue(), centroids_half_l2_norm.get_queue(), assignment_id.get_queue()})) {
    throw py::value_error("Execution queue is incompatible with allocation queues.");
  }

  int dataT_typenum = X_t.get_typenum();
  int indT_typenum = assignment_id.get_typenum();

  if (dataT_typenum != centroid_t.get_typenum() || dataT_typenum != centroids_half_l2_norm.get_typenum()) {
    throw py::value_error("Arrays have inconsistent elemental data types");
  }

  const auto &api = dpctl::detail::dpctl_capi::get();
  constexpr size_t preferred_work_group_size_multiplier = 8;
  constexpr size_t centroids_window_width_multiplier = 4;

  sycl::event comp_ev;
  if(dataT_typenum == api.UAR_FLOAT_ && indT_typenum == api.UAR_INT32_) {
    using dataT = float;
    using indT = std::int32_t;

    comp_ev = assignment<dataT, indT, preferred_work_group_size_multiplier, centroids_window_width_multiplier>(
      q,
      n_samples, n_features, n_clusters, centroids_window_height, work_group_size,
      X_t.get_data<dataT>(), centroid_t.get_data<dataT>(), 
      centroids_half_l2_norm.get_data<dataT>(), assignment_id.get_data<indT>(),
      depends
    );
  } else if (dataT_typenum == api.UAR_DOUBLE_ && indT_typenum == api.UAR_INT32_) {
    using dataT = double;
    using indT = std::int32_t;

    comp_ev = assignment<dataT, indT, preferred_work_group_size_multiplier, centroids_window_width_multiplier>(
      q,
      n_samples, n_features, n_clusters, centroids_window_height, work_group_size,
      X_t.get_data<dataT>(), centroid_t.get_data<dataT>(), 
      centroids_half_l2_norm.get_data<dataT>(), assignment_id.get_data<indT>(),
      depends
    );
  } else if(dataT_typenum == api.UAR_FLOAT_ && indT_typenum == api.UAR_INT64_) {
    using dataT = float;
    using indT = std::int64_t;

    comp_ev = assignment<dataT, indT, preferred_work_group_size_multiplier, centroids_window_width_multiplier>(
      q,
      n_samples, n_features, n_clusters, centroids_window_height, work_group_size,
      X_t.get_data<dataT>(), centroid_t.get_data<dataT>(), 
      centroids_half_l2_norm.get_data<dataT>(), assignment_id.get_data<indT>(),
      depends
    );
  } else if (dataT_typenum == api.UAR_DOUBLE_ && indT_typenum == api.UAR_INT64_) {
    using dataT = double;
    using indT = std::int64_t;

    comp_ev = assignment<dataT, indT, preferred_work_group_size_multiplier, centroids_window_width_multiplier>(
      q,
      n_samples, n_features, n_clusters, centroids_window_height, work_group_size,
      X_t.get_data<dataT>(), centroid_t.get_data<dataT>(), 
      centroids_half_l2_norm.get_data<dataT>(), assignment_id.get_data<indT>(),
      depends
    );
  } else {
    throw py::value_error("Unsupported array elemental data type");
  }

  sycl::event ht_ev = dpctl::utils::keep_args_alive(q, {X_t, centroid_t, centroids_half_l2_norm, assignment_id}, {comp_ev});

  return std::make_pair(ht_ev, comp_ev);
}

PYBIND11_MODULE(_kmeans_dpcpp, m) {
  m.def(
    "broadcast_divide", &py_broadcast_divide,
	  "broadcast_divide(divident=src, divisor=dst, sycl_queue=q, depends=[]) evaluates "
    "`src /= dst` for matrix src and vector dst",
	  py::arg("divident"), py::arg("divisor"),
	  py::arg("sycl_queue"), py::arg("depends")=py::list()
  );
  m.def(
    "half_l2_norm_squared", &py_half_l2_norm_squared,
	  "half_l2_norm_squared(centroids=X, centroids_half_l2_norm_squared=y, sycl_queue=q, depends=[]) "
    "computes row-wise half of norm squared of X and places it in y",
	  py::arg("centroids"), py::arg("centroids_half_l2_norm_squared"),
	  py::arg("sycl_queue"), py::arg("depends") = py::list()
  );

  m.def(
    "reduce_centroids_data", &py_reduce_centroids_data,
    "reduce_centroids_data(cluster_sizes_private_copies, centroids_t_private_copies, out_cluster_sizes, "
    " out_centroids_t, out_empty_clusters_list, out_n_empty_clusters, sycl_queue=q, depends=[])", 
    py::arg("cluster_sizes_private_copies"),  // IN (n_copies, n_clusters)                dataT
    py::arg("centroids_t_private_copies"),    // IN (n_copies, n_features, n_clusters,)   dataT
    py::arg("out_cluster_sizes"),             // OUT (n_clusters,)                        dataT
    py::arg("out_centroids_t"),               // OUT (n_features, n_clusters,)            dataT
    py::arg("out_empty_clusters_list"),       // OUT (n_clusters,)                        indT
    py::arg("out_n_empty_clusters"),          // OUT (1,)                                 indT
    py::arg("sycl_queue"),
    py::arg("depends") = py::list()
  );

  m.def("compute_threshold", &py_compute_threshold,
    "compute_threshold(data, topk, threshold, sycl_queue=q, depends=[]) finds "
    "topk-th largest element in data and puts in threshold",
    py::arg("data"), py::arg("topk"), py::arg("threshold"), 
    py::arg("sycl_queue"), py::arg("depends") = py::list()
  );

  m.def(
    "select_samples_far_from_centroid", &py_select_samples_far_from_centroid,
    "select_samples_far_from_centroid(n_selected, distance_to_centroid, threshold, selected_samples_idx, n_selected_gt_threshold, n_selected_eq_threshold, sycl_queue=q, depends=[]) "
    " populates `selected_samples_idx` with ids of observations whose distance to nearest centroid is greater than `threshold`. The last element of `selected_samples_idx` is "
    " populated with id of the observation whose distance to centroid centroid equals to `threshold`. `n_selected_gt_threshold` and `n_selected_eq_threshold` are temporary scalars.",
    py::arg("n_selected"), py::arg("distance_to_centroid"), py::arg("threshold"),
    py::arg("selected_samples_idx"), py::arg("n_selected_gt_threshold"), 
    py::arg("n_selected_eq_threshold"), py::arg("sycl_queue"), py::arg("depends") = py::list()
  );

  m.def(
    "relocate_empty_clusters", &py_relocate_empty_clusters,
    "Relocates empty clustsers, modifies centroid_t, cluster_sizes, per_sample_inertia",
    py::arg("n_empty_clusters"),    // int
    py::arg("X_t"),                 // IN (n_features, n_samples,)      dataT
    py::arg("sample_weights"),      // IN (n_samples, )                 dataT
    py::arg("assignment_id"),       // IN (n_samples, )                 indT
    py::arg("empty_clusters_list"), // IN (n_clusters, )                indT
    py::arg("sq_dist_to_nearest_centroid"), // IN (n_samples, )         dataT
    py::arg("centroid_t"),          // INTOUT (n_features, n_clusters,) dataT
    py::arg("cluster_sizes"),       // INOUT  (n_clusters, )            dataT
    py::arg("per_sample_inertia"),  // INOUT  (n_samples,)              dataT
    py::arg("sycl_queue"),
    py::arg("depends") = py::list()
  );

  m.def(
    "compute_centroid_shifts_squared", &py_compute_centroid_shifts_squared_kernel,
    "Computes equivalent of `np.sum(np.square(old_centroid_t - new_centroid_t), axis=0)",
    py::arg("centroid_t"),          // IN (n_features, n_clusters, )
    py::arg("new_centroid_t"),      // IN (n_features, n_clusters, )
    py::arg("out_centroid_shifts"), // OUT (n_clusters)
    py::arg("sycl_queue"),
    py::arg("depends") = py::list()
  );

  m.def("compute_centroid_to_sample_distances", &py_compute_distances,
  "Computes distances from centroids to samples. "
  "Inputs: X_t - samples with shape (n_features, n_samples), "
  "centroid_t - centroids with shape (n_features, n_clusters), "
  "and output - euclidean_distances_t with shape (n_clusters, n_samples).",
  py::arg("X_t"),                  // IN (n_features, n_samples)
  py::arg("centroid_t"),           // IN (n_features, n_clusters)
  py::arg("euclidean_distances_t"),// OUT (n_clusters, n_samples)
  py::arg("work_group_size"),
  py::arg("centroids_window_height"),
  py::arg("sycl_queue"),
  py::arg("depends") = py::list()
  );

  m.def(
    "assignment", &py_assignment, 
    "",
    py::arg("X_t"),                     // IN (n_features, n_samples,)
    py::arg("centroids_t"),             // IN (n_features, n_clusters, )
    py::arg("centroids_half_l2_norm"),  // IN (n_clusters, )
    py::arg("assignment_id"),           // OUT (n_samples,)
    py::arg("centroids_window_height"),
    py::arg("work_group_size"),
    py::arg("sycl_queue"),
    py::arg("depends") = py::list()
  );
}
