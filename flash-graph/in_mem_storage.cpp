/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashGraph.
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

#include <stdlib.h>
#include <malloc.h>

#include "safs_file.h"
#include "in_mem_storage.h"
#include "cache.h"

class in_mem_io: public io_interface
{
	const in_mem_graph &graph;
	int file_id;
	fifo_queue<io_request> req_buf;
	fifo_queue<user_compute *> compute_buf;
	fifo_queue<user_compute *> incomp_computes;

	void process_req(const io_request &req);
	void process_computes();
public:
	in_mem_io(const in_mem_graph &_graph, int file_id,
			thread *t): io_interface(t), graph(_graph), req_buf(
				get_node_id(), 1024), compute_buf(get_node_id(), 1024,
				true), incomp_computes(get_node_id(), 1024, true) {
		this->file_id = file_id;
	}

	virtual int get_file_id() const {
		return file_id;
	}

	virtual bool support_aio() {
		return true;
	}

	virtual void flush_requests() { }

	virtual int num_pending_ios() const {
		return 0;
	}

	virtual void access(io_request *requests, int num, io_status *status);
	virtual int wait4complete(int) {
		assert(compute_buf.is_empty());
		if (!incomp_computes.is_empty()) {
			compute_buf.add(&incomp_computes);
			assert(incomp_computes.is_empty());
			process_computes();
		}
		return 0;
	}
};

class in_mem_byte_array: public page_byte_array
{
	const io_request &req;
	thread_safe_page *pages;
public:
	in_mem_byte_array(const io_request &_req, thread_safe_page *pages): req(_req) {
		this->pages = pages;
	}

	virtual off_t get_offset_in_first_page() const {
		return req.get_offset() % PAGE_SIZE;
	}

	virtual thread_safe_page *get_page(int pg_idx) const {
		return &pages[pg_idx];
	}

	virtual size_t get_size() const {
		return req.get_size();
	}

	void lock() {
		assert(0);
	}

	void unlock() {
		assert(0);
	}
};

enum
{
	IN_QUEUE,
};

void in_mem_io::process_req(const io_request &req)
{
	assert(req.get_req_type() == io_request::USER_COMPUTE);
	in_mem_byte_array byte_arr(req, &graph.graph_pages[req.get_offset() / PAGE_SIZE]);
	user_compute *compute = req.get_compute();
	compute->run(byte_arr);
	// If the user compute hasn't completed and it's not in the queue,
	// add it to the queue.
	if (!compute->has_completed() && !compute->test_flag(IN_QUEUE)) {
		compute->set_flag(IN_QUEUE, true);
		if (compute_buf.is_full())
			compute_buf.expand_queue(compute_buf.get_size() * 2);
		compute_buf.push_back(compute);
	}
	else
		compute->dec_ref();

	if (compute->has_completed() && !compute->test_flag(IN_QUEUE)) {
		assert(compute->get_ref() == 0);
		// It might still be referenced by the graph engine.
		if (compute->get_ref() == 0) {
			compute_allocator *alloc = compute->get_allocator();
			alloc->free(compute);
		}
	}
}

void in_mem_io::process_computes()
{
	while (!compute_buf.is_empty()) {
		user_compute *compute = compute_buf.pop_front();
		assert(compute->get_ref() > 0);
		while (compute->has_requests()) {
			compute->fetch_requests(this, req_buf, req_buf.get_size());
			while (!req_buf.is_empty()) {
				io_request new_req = req_buf.pop_front();
				process_req(new_req);
			}
		}
		if (compute->has_completed()) {
			compute->dec_ref();
			compute->set_flag(IN_QUEUE, false);
			assert(compute->get_ref() == 0);
			if (compute->get_ref() == 0) {
				compute_allocator *alloc = compute->get_allocator();
				alloc->free(compute);
			}
		}
		else
			incomp_computes.push_back(compute);
	}
}

void in_mem_io::access(io_request *requests, int num, io_status *)
{
	for (int i = 0; i < num; i++) {
		io_request &req = requests[i];
		// Let's possess a reference to the user compute first. process_req()
		// will release the reference when the user compute is completed.
		req.get_compute()->inc_ref();
		process_req(req);
	}
	process_computes();
}

class in_mem_io_factory: public file_io_factory
{
	const in_mem_graph &graph;
	int file_id;
public:
	in_mem_io_factory(const in_mem_graph &_graph, int file_id,
			const std::string file_name): file_io_factory(file_name), graph(_graph) {
		this->file_id = file_id;
	}

	virtual int get_file_id() const {
		return file_id;
	}

	virtual io_interface::ptr create_io(thread *t) {
		return io_interface::ptr(new in_mem_io(graph, file_id, t));
	}

	virtual void destroy_io(io_interface *) {
	}
};

in_mem_graph::ptr in_mem_graph::load_graph(const std::string &file_name)
{
	file_io_factory::shared_ptr io_factory = ::create_io_factory(file_name,
			REMOTE_ACCESS);

	in_mem_graph::ptr graph = in_mem_graph::ptr(new in_mem_graph());
	graph->graph_size = io_factory->get_file_size();
	size_t num_pages = ROUNDUP_PAGE(graph->graph_size) / PAGE_SIZE;
	graph->graph_data = (char *) memalign(PAGE_SIZE, num_pages * PAGE_SIZE);
	assert(graph->graph_data);
	graph->graph_file_name = file_name;
	graph->graph_pages = new thread_safe_page[num_pages];
	assert(graph->graph_pages);

	printf("load a graph of %ld bytes\n", graph->graph_size);
	graph->graph_file_id = io_factory->get_file_id();
	io_interface::ptr io = io_factory->create_io(thread::get_curr_thread());
	const size_t MAX_IO_SIZE = 256 * 1024 * 1024;
	for (off_t off = 0; off < graph->graph_size; off += MAX_IO_SIZE) {
		data_loc_t loc(io_factory->get_file_id(), off);
		size_t req_size = min(MAX_IO_SIZE, graph->graph_size - off);
		io_request req(graph->graph_data + off, loc, req_size, READ);
		io->access(&req, 1);
		io->wait4complete(1);
	}

	for (size_t i = 0; i < num_pages; i++) {
		page_id_t pg_id(graph->graph_file_id, i * PAGE_SIZE);
		graph->graph_pages[i] = thread_safe_page(pg_id,
				graph->graph_data + i * PAGE_SIZE, 0);
	}

	return graph;
}

file_io_factory::shared_ptr in_mem_graph::create_io_factory() const
{
	return file_io_factory::shared_ptr(new in_mem_io_factory(*this,
				graph_file_id, graph_file_name));
}
