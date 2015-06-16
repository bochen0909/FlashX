/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(_OPENMP)
#include <omp.h>
#endif
#include <cblas.h>

#include <atomic>

#include <boost/format.hpp>

#include "log.h"

#include "mem_dense_matrix.h"
#include "generic_type.h"
#include "vector.h"
#include "local_matrix_store.h"
#include "mem_matrix_store.h"
#include "NUMA_dense_matrix.h"
#include "mem_worker_thread.h"
#include "one_val_matrix_store.h"
#include "mapply_matrix_store.h"

namespace fm
{

mem_dense_matrix::ptr mem_dense_matrix::create(size_t nrow, size_t ncol,
		matrix_layout_t layout, const scalar_type &type, int num_nodes)
{
	// If nothing is specified, it creates a zero matrix.
	detail::mem_matrix_store::ptr store(new detail::one_val_matrix_store(
				type.create_scalar(), nrow, ncol, layout, num_nodes));
	return mem_dense_matrix::ptr(new mem_dense_matrix(store));
}

mem_dense_matrix::ptr mem_dense_matrix::create(size_t nrow, size_t ncol,
		matrix_layout_t layout, const scalar_type &type, const set_operate &op,
		int num_nodes)
{
	detail::matrix_store::ptr store = detail::mem_matrix_store::create(
			nrow, ncol, layout, type, num_nodes);
	store->set_data(op);
	return mem_dense_matrix::ptr(new mem_dense_matrix(store));
}

vector::ptr mem_dense_matrix::get_col(off_t idx) const
{
	detail::vec_store::const_ptr vec
		= static_cast<const detail::mem_matrix_store &>(
				get_data()).get_col_vec(idx);
	if (vec)
		return vector::create(vec);
	else
		return vector::ptr();
}

vector::ptr mem_dense_matrix::get_row(off_t idx) const
{
	detail::vec_store::const_ptr vec
		= static_cast<const detail::mem_matrix_store &>(
				get_data()).get_row_vec(idx);
	if (vec)
		return vector::create(vec);
	else
		return vector::ptr();
}

dense_matrix::ptr mem_dense_matrix::get_cols(const std::vector<off_t> &idxs) const
{
	if (store_layout() == matrix_layout_t::L_COL) {
		const detail::mem_matrix_store &store
			= static_cast<const detail::mem_matrix_store &>(get_data());
		return dense_matrix::ptr(new mem_dense_matrix(store.get_cols(idxs)));
	}
	else
		return dense_matrix::ptr();
}

dense_matrix::ptr mem_dense_matrix::get_rows(const std::vector<off_t> &idxs) const
{
	if (store_layout() == matrix_layout_t::L_ROW) {
		const detail::mem_matrix_store &store
			= static_cast<const detail::mem_matrix_store &>(get_data());
		return dense_matrix::ptr(new mem_dense_matrix(store.get_rows(idxs)));
	}
	else
		return dense_matrix::ptr();
}

dense_matrix::ptr mem_dense_matrix::transpose() const
{
	return dense_matrix::ptr(new mem_dense_matrix(get_data().transpose()));
}

dense_matrix::ptr mem_dense_matrix::inner_prod(const dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op,
		matrix_layout_t out_layout) const
{
	if (!verify_inner_prod(m, left_op, right_op))
		return dense_matrix::ptr();

	if (out_layout == matrix_layout_t::L_NONE) {
		if (this->store_layout() == matrix_layout_t::L_ROW)
			out_layout = matrix_layout_t::L_ROW;
		else if (this->is_wide())
			out_layout = matrix_layout_t::L_ROW;
		else
			out_layout = matrix_layout_t::L_COL;
	}

	detail::mem_matrix_store::ptr res = detail::mem_matrix_store::create(
			get_num_rows(), m.get_num_cols(), out_layout,
			right_op.get_output_type(), get_num_nodes());
	const detail::mem_matrix_store &mem_m
		= dynamic_cast<const detail::mem_matrix_store &>(m.get_data());
	if (is_wide())
		inner_prod_wide(mem_m, left_op, right_op, *res);
	else
		inner_prod_tall(mem_m, left_op, right_op, *res);

	return dense_matrix::ptr(new mem_dense_matrix(res));
}

namespace
{

class inner_prod_tall_task: public thread_task
{
	detail::local_matrix_store::const_ptr local_right;
	detail::local_matrix_store::const_ptr local_store;
	detail::local_matrix_store::ptr local_res;
	const bulk_operate &left_op;
	const bulk_operate &right_op;
public:
	inner_prod_tall_task(detail::local_matrix_store::const_ptr local_store,
			detail::local_matrix_store::const_ptr local_right,
			const bulk_operate &_left_op, const bulk_operate &_right_op,
			detail::local_matrix_store::ptr local_res): left_op(
				_left_op), right_op(_right_op) {
		this->local_right = local_right;
		this->local_store = local_store;
		this->local_res = local_res;
	}
	void run() {
		local_res->reset_data();
		detail::inner_prod(*local_store, *local_right, left_op, right_op,
				*local_res);
	}
};

}

void mem_dense_matrix::inner_prod_tall(const detail::mem_matrix_store &m,
		const bulk_operate &left_op, const bulk_operate &right_op,
		detail::mem_matrix_store &res) const
{
	// We assume the right matrix is small, so we don't need to partition it.
	detail::local_matrix_store::const_ptr local_right = m.get_portion(0);
	assert(local_right->get_num_rows() == m.get_num_rows()
			&& local_right->get_num_cols() == m.get_num_cols());
	// If the left matrix is row-major, the right matrix should be
	// column-major. When the left matrix is tall, the right matrix should
	// be small. It makes sense to convert the right matrix to column major
	// before we break up the left matrix for parallel processing.
	if (!is_wide() && this->store_layout() == matrix_layout_t::L_ROW)
		local_right = local_right->conv2(matrix_layout_t::L_COL);
	const detail::mem_matrix_store &this_store
		= dynamic_cast<const detail::mem_matrix_store &>(get_data());
	size_t num_chunks = this_store.get_num_portions();
	assert(this_store.get_portion_size().first == res.get_portion_size().first);
	detail::mem_thread_pool::ptr mem_threads
		= detail::mem_thread_pool::get_global_mem_threads();
	for (size_t i = 0; i < num_chunks; i++) {
		detail::local_matrix_store::const_ptr local_store
			= this_store.get_portion(i);
		detail::local_matrix_store::ptr local_res = res.get_portion(i);
		assert(local_store->get_global_start_row()
				== local_res->get_global_start_row());
		assert(local_store->get_global_start_col()
				== local_res->get_global_start_col());
		assert(local_store->get_node_id() == local_res->get_node_id());
		int node_id = local_store->get_node_id();
		// If the local matrix portion is not assigned to any node, 
		// assign the tasks in round robin fashion.
		if (node_id < 0)
			node_id = i % mem_threads->get_num_nodes();
		mem_threads->process_task(node_id,
				new inner_prod_tall_task(local_store, local_right,
					left_op, right_op, local_res));
	}
	mem_threads->wait4complete();
}

namespace
{

class inner_prod_wide_task: public thread_task
{
	detail::local_matrix_store::const_ptr local_store;
	detail::local_matrix_store::const_ptr local_store2;
	const bulk_operate &left_op;
	const bulk_operate &right_op;
	const detail::mem_matrix_store &res;
	std::vector<detail::local_matrix_store::ptr> &local_ms;
public:
	inner_prod_wide_task(detail::local_matrix_store::const_ptr local_store,
			detail::local_matrix_store::const_ptr local_store2,
			const bulk_operate &_left_op, const bulk_operate &_right_op,
			const detail::mem_matrix_store &_res,
			std::vector<detail::local_matrix_store::ptr> &_local_ms): left_op(
				_left_op), right_op(_right_op), res(_res), local_ms(_local_ms) {
		this->local_store = local_store;
		this->local_store2 = local_store2;
	}

	void run();
};

void inner_prod_wide_task::run()
{
	detail::pool_task_thread *curr
		= dynamic_cast<detail::pool_task_thread *>(thread::get_curr_thread());
	int thread_id = curr->get_pool_thread_id();
	detail::local_matrix_store::ptr local_m = local_ms[thread_id];
	if (local_m == NULL) {
		int node_id = curr->get_node_id();
		if (res.store_layout() == matrix_layout_t::L_COL)
			local_m = detail::local_matrix_store::ptr(
					new detail::local_buf_col_matrix_store(0, 0,
						res.get_num_rows(), res.get_num_cols(),
						right_op.get_output_type(), node_id));
		else
			local_m = detail::local_matrix_store::ptr(
					new detail::local_buf_row_matrix_store(0, 0,
						res.get_num_rows(), res.get_num_cols(),
						right_op.get_output_type(), node_id));
		local_m->reset_data();
		local_ms[thread_id] = local_m;
	}
	detail::inner_prod(*local_store, *local_store2, left_op, right_op,
			*local_m);
}

}

void mem_dense_matrix::inner_prod_wide(const detail::mem_matrix_store &m,
		const bulk_operate &left_op, const bulk_operate &right_op,
		detail::mem_matrix_store &res) const
{
	assert(this->get_num_rows() == res.get_num_rows());
	assert(m.get_num_cols() == res.get_num_cols());

	const detail::mem_matrix_store &this_store
		= dynamic_cast<const detail::mem_matrix_store &>(get_data());
	size_t num_chunks = this_store.get_num_portions();
	detail::mem_thread_pool::ptr mem_threads
		= detail::mem_thread_pool::get_global_mem_threads();
	int nthreads = mem_threads->get_num_threads();
	std::vector<detail::local_matrix_store::ptr> local_ms(nthreads);
	for (size_t i = 0; i < num_chunks; i++) {
		detail::local_matrix_store::const_ptr local_store
			= this_store.get_portion(i);
		detail::local_matrix_store::const_ptr local_store2 = m.get_portion(i);
		assert(local_store->get_global_start_row()
				== local_store2->get_global_start_col());
		assert(local_store->get_global_start_col()
				== local_store2->get_global_start_row());
		assert(local_store->get_node_id() == local_store2->get_node_id());
		int node_id = local_store->get_node_id();
		// If the local matrix portion is not assigned to any node, 
		// assign the tasks in round robin fashion.
		if (node_id < 0)
			node_id = i % mem_threads->get_num_nodes();
		mem_threads->process_task(node_id,
				new inner_prod_wide_task(local_store, local_store2,
					left_op, right_op, res, local_ms));
	}
	mem_threads->wait4complete();

	// Aggregate the results from omp threads.
	res.reset_data();
	detail::local_matrix_store::ptr local_res = res.get_portion(0);
	assert(local_res->get_num_rows() == res.get_num_rows()
			&& local_res->get_num_cols() == res.get_num_cols());
	for (int j = 0; j < nthreads; j++) {
		// It's possible that the local matrix store doesn't exist
		// because the input matrix is very small.
		if (local_ms[j])
			detail::mapply2(*local_res, *local_ms[j], right_op, *local_res);
	}
}

namespace
{

class aggregate_task: public thread_task
{
	detail::local_matrix_store::const_ptr local_store;
	const bulk_operate &op;
	char *local_res;
public:
	aggregate_task(detail::local_matrix_store::const_ptr local_store,
			const bulk_operate &_op, char *local_res): op(_op) {
		this->local_store = local_store;
		this->local_res = local_res;
	}

	void run() {
		detail::aggregate(*local_store, op, local_res);
	}
};

}

scalar_variable::ptr mem_dense_matrix::aggregate(const bulk_operate &op) const
{
	if (!verify_aggregate(op))
		return scalar_variable::ptr();
	scalar_variable::ptr res = op.get_output_type().create_scalar();

	const detail::mem_matrix_store &this_store
		= dynamic_cast<const detail::mem_matrix_store &>(get_data());
	size_t num_chunks = this_store.get_num_portions();
	detail::mem_thread_pool::ptr mem_threads
		= detail::mem_thread_pool::get_global_mem_threads();
	std::unique_ptr<char[]> raw_arr(new char[res->get_size() * num_chunks]);
	for (size_t i = 0; i < num_chunks; i++) {
		detail::local_matrix_store::const_ptr local_store
			= this_store.get_portion(i);

		int node_id = local_store->get_node_id();
		// If the local matrix portion is not assigned to any node, 
		// assign the tasks in round robin fashion.
		if (node_id < 0)
			node_id = i % mem_threads->get_num_nodes();
		mem_threads->process_task(node_id,
				new aggregate_task(local_store, op,
					raw_arr.get() + i * op.output_entry_size()));
	}
	mem_threads->wait4complete();

	char raw_res[res->get_size()];
	op.runA(num_chunks, raw_arr.get(), raw_res);
	res->set_raw(raw_res, res->get_size());
	return res;
}

namespace
{

class mapply2_op: public detail::portion_mapply_op
{
	bulk_operate::const_ptr op;
public:
	mapply2_op(bulk_operate::const_ptr op, size_t out_num_rows,
			size_t out_num_cols): detail::portion_mapply_op(out_num_rows,
				out_num_cols, op->get_output_type()) {
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const {
		return portion_mapply_op::const_ptr(new mapply2_op(op,
					get_out_num_cols(), get_out_num_rows()));
	}
};

void mapply2_op::run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 2);
	assert(ins[0]->get_global_start_col() == ins[1]->get_global_start_col());
	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == ins[1]->get_global_start_row());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	detail::mapply2(*ins[0], *ins[1], *op, out);
}

}

dense_matrix::ptr mem_dense_matrix::mapply2(const dense_matrix &m,
		bulk_operate::const_ptr op) const
{
	assert(m.is_in_mem());
	// The same shape and the same data layout.
	if (!verify_mapply2(m, *op))
		return dense_matrix::ptr();

	std::vector<detail::matrix_store::const_ptr> ins(2);
	ins[0] = this->get_raw_store();
	ins[1] = static_cast<const mem_dense_matrix &>(m).get_raw_store();
	mapply2_op::const_ptr mapply_op(new mapply2_op(op, get_num_rows(),
				get_num_cols()));
	return mem_dense_matrix::ptr(new mem_dense_matrix(
				__mapply_portion_virtual(ins, mapply_op, this->store_layout())));
}

namespace
{

class sapply_op: public detail::portion_mapply_op
{
	bulk_uoperate::const_ptr op;
public:
	sapply_op(bulk_uoperate::const_ptr op, size_t out_num_rows,
			size_t out_num_cols): detail::portion_mapply_op(out_num_rows,
				out_num_cols, op->get_output_type()) {
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const {
		return portion_mapply_op::const_ptr(new sapply_op(op, get_out_num_cols(),
					get_out_num_rows()));
	}
};

void sapply_op::run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 1);
	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	detail::sapply(*ins[0], *op, out);
}

}

dense_matrix::ptr mem_dense_matrix::sapply(bulk_uoperate::const_ptr op) const
{
	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = this->get_raw_store();
	sapply_op::const_ptr mapply_op(new sapply_op(op, get_num_rows(),
				get_num_cols()));
	detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
			mapply_op, this->store_layout());
	return dense_matrix::create(ret);
}

class matrix_margin_apply_op: public detail::portion_mapply_op
{
	apply_margin margin;
	arr_apply_operate::const_ptr op;
public:
	matrix_margin_apply_op(apply_margin margin, arr_apply_operate::const_ptr op,
			size_t out_num_rows, size_t out_num_cols): detail::portion_mapply_op(
				out_num_rows, out_num_cols, op->get_output_type()) {
		this->margin = margin;
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const {
		detail::apply(margin, *op, *ins[0], out);
	}
	virtual portion_mapply_op::const_ptr transpose() const {
		apply_margin new_margin = this->margin == apply_margin::MAR_ROW ?
			apply_margin::MAR_COL : apply_margin::MAR_ROW;
		return portion_mapply_op::const_ptr(new matrix_margin_apply_op(
					new_margin, op, get_out_num_cols(), get_out_num_rows()));
	}
};

dense_matrix::ptr mem_dense_matrix::apply(apply_margin margin,
		arr_apply_operate::const_ptr op) const
{
	assert(op->get_num_out_eles() > 0);
	// In these two cases, we need to convert the matrix store layout
	// before we can apply the function to the matrix.
	detail::matrix_store::const_ptr this_mat;
	if (is_wide() && store_layout() == matrix_layout_t::L_COL
			&& margin == apply_margin::MAR_ROW)
		this_mat = static_cast<const detail::mem_matrix_store &>(get_data()).conv2(
				matrix_layout_t::L_ROW);
	else if (!is_wide() && store_layout() == matrix_layout_t::L_ROW
			&& margin == apply_margin::MAR_COL)
		this_mat = static_cast<const detail::mem_matrix_store &>(get_data()).conv2(
				matrix_layout_t::L_COL);
	else
		this_mat = get_raw_store();
	assert(this_mat);

	// In these two cases, apply the function on the rows/columns in the long
	// dimension. The previous two cases are handled as one of the two cases
	// after the matrix layout conversion from the previous cases.
	if (is_wide() && this_mat->store_layout() == matrix_layout_t::L_ROW
			&& margin == apply_margin::MAR_ROW) {
		// In this case, it's very difficult to make it work for NUMA matrix.
		assert(get_num_nodes() == -1);
		detail::mem_row_matrix_store::const_ptr row_mat
			= detail::mem_row_matrix_store::cast(this_mat);
		detail::mem_row_matrix_store::ptr ret_mat
			= detail::mem_row_matrix_store::create(get_num_rows(),
					op->get_num_out_eles(), op->get_output_type());
		for (size_t i = 0; i < get_num_rows(); i++) {
			local_cref_vec_store in_vec(row_mat->get_row(i), 0,
					this_mat->get_num_cols(), get_type(), -1);
			local_ref_vec_store out_vec(ret_mat->get_row(i), 0,
					ret_mat->get_num_cols(), ret_mat->get_type(), -1);
			op->run(in_vec, out_vec);
		}
		return mem_dense_matrix::create(ret_mat);
	}
	else if (!is_wide() && this_mat->store_layout() == matrix_layout_t::L_COL
			&& margin == apply_margin::MAR_COL) {
		assert(get_num_nodes() == -1);
		detail::mem_col_matrix_store::const_ptr col_mat
			= detail::mem_col_matrix_store::cast(this_mat);
		detail::mem_col_matrix_store::ptr ret_mat
			= detail::mem_col_matrix_store::create(op->get_num_out_eles(),
					get_num_cols(), op->get_output_type());
		for (size_t i = 0; i < get_num_cols(); i++) {
			local_cref_vec_store in_vec(col_mat->get_col(i), 0,
					this_mat->get_num_rows(), get_type(), -1);
			local_ref_vec_store out_vec(ret_mat->get_col(i), 0,
					ret_mat->get_num_rows(), ret_mat->get_type(), -1);
			op->run(in_vec, out_vec);
		}
		return mem_dense_matrix::create(ret_mat);
	}
	// There are four cases left. In these four cases, apply the function
	// on the rows/columns in the short dimension. We can use mapply to
	// parallelize the computation here.
	else {
		std::vector<detail::matrix_store::const_ptr> ins(1);
		ins[0] = this->get_raw_store();
		size_t out_num_rows;
		size_t out_num_cols;
		if (margin == apply_margin::MAR_ROW) {
			out_num_rows = this->get_num_rows();
			out_num_cols = op->get_num_out_eles();
		}
		else {
			out_num_rows = op->get_num_out_eles();
			out_num_cols = this->get_num_cols();
		}
		matrix_margin_apply_op::const_ptr apply_op(new matrix_margin_apply_op(
					margin, op, out_num_rows, out_num_cols));
		matrix_layout_t output_layout = (margin == apply_margin::MAR_ROW
				? matrix_layout_t::L_ROW : matrix_layout_t::L_COL);
		detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
				apply_op, output_layout);
		return dense_matrix::create(ret);
	}
}

bool mem_dense_matrix::verify_inner_prod(const dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (!m.is_in_mem()) {
		BOOST_LOG_TRIVIAL(error) << "The right matrix isn't in memory";
		return false;
	}
	return dense_matrix::verify_inner_prod(m, left_op, right_op);
}

mem_dense_matrix::ptr mem_dense_matrix::cast(dense_matrix::ptr m)
{
	if (!m->is_in_mem()) {
		BOOST_LOG_TRIVIAL(error)
			<< "Can't cast an EM matrix to mem_dense_matrix";
		return mem_dense_matrix::ptr();
	}
	return std::static_pointer_cast<mem_dense_matrix>(m);
}

mem_dense_matrix::const_ptr mem_dense_matrix::cast(dense_matrix::const_ptr m)
{
	if (!m->is_in_mem()) {
		BOOST_LOG_TRIVIAL(error)
			<< "Can't cast an EM matrix to mem_dense_matrix";
		return mem_dense_matrix::const_ptr();
	}
	return std::static_pointer_cast<const mem_dense_matrix>(m);
}

namespace
{

class scale_col_op: public detail::portion_mapply_op
{
	vector::const_ptr vals;
public:
	scale_col_op(vector::const_ptr vals, size_t out_num_rows,
			size_t out_num_cols, const scalar_type &type): detail::portion_mapply_op(
				out_num_rows, out_num_cols, type) {
		this->vals = vals;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const;
};

class scale_row_op: public detail::portion_mapply_op
{
	vector::const_ptr vals;
public:
	scale_row_op(vector::const_ptr vals, size_t out_num_rows,
			size_t out_num_cols, const scalar_type &type): detail::portion_mapply_op(
				out_num_rows, out_num_cols, type) {
		this->vals = vals;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const;
};

detail::portion_mapply_op::const_ptr scale_col_op::transpose() const
{
	return detail::portion_mapply_op::const_ptr(new scale_row_op(vals,
				get_out_num_cols(), get_out_num_rows(), get_output_type()));
}

void scale_col_op::run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 1);
	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	const detail::mem_vec_store &store
		= dynamic_cast<const detail::mem_vec_store &>(vals->get_data());
	// This is a tall matrix. We divide the matrix horizontally.
	if (ins[0]->get_num_cols() == get_out_num_cols()) {
		// If we use get_raw_arr, it may not work with NUMA vector.
		const char *arr = store.get_sub_arr(0, vals->get_length());
		assert(arr);
		local_cref_vec_store lvals(arr, 0, vals->get_length(),
				vals->get_type(), -1);
		detail::scale_cols(*ins[0], lvals, out);
	}
	// We divide the matrix vertically.
	else {
		assert(vals->get_length() == get_out_num_cols());
		off_t global_start = ins[0]->get_global_start_col();
		size_t len = ins[0]->get_num_cols();
		local_vec_store::const_ptr portion = store.get_portion(global_start,
				len);
		assert(portion);
		detail::scale_cols(*ins[0], *portion, out);
	}
}

void scale_row_op::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 1);
	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	const detail::mem_vec_store &store
		= dynamic_cast<const detail::mem_vec_store &>(vals->get_data());
	// This is a wide matrix. We divide the matrix vertically.
	if (ins[0]->get_num_rows() == get_out_num_rows()) {
		// If we use get_raw_arr, it may not work with NUMA vector.
		const char *arr = store.get_sub_arr(0, vals->get_length());
		assert(arr);
		local_cref_vec_store lvals(arr, 0, vals->get_length(),
				vals->get_type(), -1);
		detail::scale_rows(*ins[0], lvals, out);
	}
	// We divide the tall matrix horizontally.
	else {
		assert(vals->get_length() == get_out_num_rows());
		off_t global_start = ins[0]->get_global_start_row();
		size_t len = ins[0]->get_num_rows();
		local_vec_store::const_ptr portion = store.get_portion(global_start,
				len);
		assert(portion);
		detail::scale_rows(*ins[0], *portion, out);
	}
}

detail::portion_mapply_op::const_ptr scale_row_op::transpose() const
{
	return detail::portion_mapply_op::const_ptr(new scale_col_op(vals,
				get_out_num_cols(), get_out_num_rows(), get_output_type()));
}

}

dense_matrix::ptr mem_dense_matrix::scale_cols(vector::const_ptr vals) const
{
	assert(get_num_cols() == vals->get_length());
	assert(get_type() == vals->get_type());
	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = this->get_raw_store();
	scale_col_op::const_ptr mapply_op(new scale_col_op(vals, get_num_rows(),
						get_num_cols(), get_type()));
	detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
			mapply_op, this->store_layout());
	return dense_matrix::create(ret);
}

dense_matrix::ptr mem_dense_matrix::scale_rows(vector::const_ptr vals) const
{
	assert(get_num_rows() == vals->get_length());
	assert(get_type() == vals->get_type());
	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = this->get_raw_store();
	scale_row_op::const_ptr mapply_op(new scale_row_op(vals, get_num_rows(),
						get_num_cols(), get_type()));
	detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
			mapply_op, this->store_layout());
	return dense_matrix::create(ret);
}

dense_matrix::ptr mem_dense_matrix::conv2(matrix_layout_t layout) const
{
	const detail::mem_matrix_store &in_store
		= static_cast<const detail::mem_matrix_store &>(get_data());
	detail::matrix_store::ptr tmp = in_store.conv2(layout);
	return mem_dense_matrix::create(detail::mem_matrix_store::cast(tmp));

}

namespace detail
{

namespace
{

class mapply_task: public thread_task
{
	std::vector<detail::local_matrix_store::const_ptr> local_stores;
	detail::local_matrix_store::ptr local_res;
	const portion_mapply_op &op;
public:
	mapply_task(
			const std::vector<detail::local_matrix_store::const_ptr> &local_stores,
			const portion_mapply_op &_op,
			detail::local_matrix_store::ptr local_res): op(_op) {
		this->local_stores = local_stores;
		this->local_res = local_res;
	}

	void run() {
		op.run(local_stores, *local_res);
	}
};

}

matrix_store::ptr __mapply_portion(
		const std::vector<matrix_store::const_ptr> &mats,
		portion_mapply_op::const_ptr op, matrix_layout_t out_layout)
{
	std::vector<mem_matrix_store::const_ptr> mem_mats(mats.size());
	for (size_t i = 0; i < mats.size(); i++)
		mem_mats[i] = mem_matrix_store::cast(mats[i]);

	assert(mats.size() >= 1);
	size_t num_chunks = mats.front()->get_num_portions();
	std::pair<size_t, size_t> first_size = mats.front()->get_portion_size();
	if (mats.front()->is_wide()) {
		assert(op->get_out_num_cols() == mats.front()->get_num_cols());
		for (size_t i = 1; i < mats.size(); i++) {
			assert(first_size.second == mats[i]->get_portion_size().second);
			assert(mats[i]->store_layout() == mats.front()->store_layout());
			assert(mem_mats[i]->get_num_nodes()
					== mem_mats.front()->get_num_nodes());
			assert(mats[i]->get_num_cols() == mats.front()->get_num_cols());
		}
	}
	else {
		assert(op->get_out_num_rows() == mats.front()->get_num_rows());
		for (size_t i = 1; i < mats.size(); i++) {
			assert(first_size.first == mats[i]->get_portion_size().first);
			assert(mats[i]->store_layout() == mats.front()->store_layout());
			assert(mem_mats[i]->get_num_nodes()
					== mem_mats.front()->get_num_nodes());
			assert(mats[i]->get_num_rows() == mats.front()->get_num_rows());
		}
	}

	detail::mem_matrix_store::ptr res = detail::mem_matrix_store::create(
			op->get_out_num_rows(), op->get_out_num_cols(),
			out_layout, op->get_output_type(), mem_mats.front()->get_num_nodes());

	std::vector<detail::local_matrix_store::const_ptr> local_stores(mats.size());
	detail::mem_thread_pool::ptr mem_threads
		= detail::mem_thread_pool::get_global_mem_threads();
	for (size_t i = 0; i < num_chunks; i++) {
		detail::local_matrix_store::ptr local_res = res->get_portion(i);
		for (size_t j = 0; j < local_stores.size(); j++) {
			local_stores[j] = mem_mats[j]->get_portion(i);
			assert(local_res->get_node_id() == local_stores[j]->get_node_id());
		}

		int node_id = local_res->get_node_id();
		// If the local matrix portion is not assigned to any node, 
		// assign the tasks in round robin fashion.
		if (node_id < 0)
			node_id = i % mem_threads->get_num_nodes();
		mem_threads->process_task(node_id,
				new mapply_task(local_stores, *op, local_res));
	}
	mem_threads->wait4complete();
	return res;
}

matrix_store::ptr __mapply_portion_virtual(
		const std::vector<matrix_store::const_ptr> &stores,
		portion_mapply_op::const_ptr op, matrix_layout_t out_layout)
{
	std::vector<mem_matrix_store::const_ptr> mem_stores(stores.size());
	for (size_t i = 0; i < stores.size(); i++)
		mem_stores[i] = mem_matrix_store::cast(stores[i]);
	return mem_matrix_store::ptr(new mapply_matrix_store(mem_stores, op,
				out_layout, op->get_out_num_rows(), op->get_out_num_cols()));
}

dense_matrix::ptr mapply_portion(
		const std::vector<dense_matrix::const_ptr> &mats,
		portion_mapply_op::const_ptr op, matrix_layout_t out_layout)
{
	std::vector<mem_matrix_store::const_ptr> stores(mats.size());
	for (size_t i = 0; i < stores.size(); i++)
		stores[i] = mem_matrix_store::cast(mats[i]->get_raw_store());
	matrix_store::const_ptr ret(new mapply_matrix_store(stores, op,
				out_layout, op->get_out_num_rows(), op->get_out_num_cols()));
	return dense_matrix::create(ret);
}

}

}
