#include <stdio.h>

#include "messaging.h"
#include "container.cpp"

void io_request::assign(io_request &req)
{
	this->offset = req.offset;
	this->io = req.io;
	this->priv = req.priv;
	this->access_method = req.access_method;
	this->num_bufs = req.num_bufs;
	this->vec_capacity = req.vec_capacity;
	this->partial = req.partial;
	this->completed_size = req.completed_size;
	this->orig = req.orig;
	this->refcnt = 0;
	this->node_id = req.node_id;
	/*
	 * If the request uses embedded vector, then the new request
	 * should point to its own embedded vector. Otherwise,
	 * the new request steals the vector from the old one.
	 */
	if (req.use_embedded())
		this->vec_pointer = this->embedded_vecs;
	else
		this->vec_pointer = req.vec_pointer;
	this->next = req.next;
	this->completed_size = req.completed_size;
	memcpy(this->embedded_vecs, req.embedded_vecs,
			sizeof(req.embedded_vecs[0]) * NUM_EMBEDDED_IOVECS);
	req.vec_pointer = req.embedded_vecs;
	req.vec_capacity = NUM_EMBEDDED_IOVECS;
	req.clear();
}

void io_request::add_buf(char *buf, int size)
{
	if (num_bufs >= vec_capacity) {
		if (vec_pointer == embedded_vecs) {
			vec_capacity = MIN_NUM_ALLOC_IOVECS;
			vec_pointer = new struct iovec[vec_capacity];
			memcpy(vec_pointer, embedded_vecs,
					sizeof(embedded_vecs[0]) * NUM_EMBEDDED_IOVECS);
		}
		else {
			vec_capacity *= 2;
			struct iovec *tmp = new struct iovec[vec_capacity];
			memcpy(tmp, vec_pointer,
					sizeof(vec_pointer[0]) * vec_capacity / 2);
			delete [] vec_pointer;
			vec_pointer = tmp;
		}
	}
	assert(num_bufs < vec_capacity);
	vec_pointer[num_bufs].iov_base = buf;
	vec_pointer[num_bufs].iov_len = size;
	num_bufs++;
}

void io_request::add_buf_front(char *buf, int size)
{
	if (num_bufs >= vec_capacity) {
		if (vec_pointer == embedded_vecs) {
			vec_capacity = MIN_NUM_ALLOC_IOVECS;
			vec_pointer = new struct iovec[vec_capacity];
			memcpy(vec_pointer + 1, embedded_vecs,
					sizeof(embedded_vecs[0]) * NUM_EMBEDDED_IOVECS);
		}
		else {
			vec_capacity *= 2;
			struct iovec *tmp = new struct iovec[vec_capacity];
			memcpy(tmp + 1, vec_pointer,
					sizeof(vec_pointer[0]) * vec_capacity / 2);
			delete [] vec_pointer;
			vec_pointer = tmp;
		}
	}
	else {
		memmove(vec_pointer + 1, vec_pointer,
				sizeof(vec_pointer[0]) * num_bufs);
	}
	assert(num_bufs < vec_capacity);
	vec_pointer[0].iov_base = buf;
	vec_pointer[0].iov_len = size;
	num_bufs++;
}

template<class T>
msg_sender<T>::msg_sender(int buf_size, thread_safe_FIFO_queue<T> **queues,
		int num_queues, bool thread_safe) {
	this->thread_safe = thread_safe;
	pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
	buf = (T *) numa_alloc_local(sizeof(T) * buf_size);
	this->buf_size = buf_size;
	num_current = 0;
	dest_queues = (thread_safe_FIFO_queue<T> **) numa_alloc_local(
			sizeof(thread_safe_FIFO_queue<T> *) * num_queues);
	memcpy(dest_queues, queues, sizeof(thread_safe_FIFO_queue<T> *) * num_queues);
	this->num_queues = num_queues;
}

/**
 * flush the entries in the buffer to the queues.
 * A queue is randomly picked. If the queue is full, pick the next queue
 * until all queues are tried or all entries in the buffer is flushed.
 * return the number of entries that have been flushed.
 */
template<class T>
int msg_sender<T>::flush(bool locked) {
	if (!locked && thread_safe)
		pthread_spin_lock(&lock);
	if (num_current == 0) {
		if (!locked && thread_safe)
			pthread_spin_unlock(&lock);
		return 0;
	}

	int base_idx;
	if (num_queues == 1)
		base_idx = 0;
	else
		base_idx = random() % num_queues;
	int num_sent = 0;
	T *tmp = buf;
	for (int i = 0; num_current > 0 && i < num_queues; i++) {
		thread_safe_FIFO_queue<T> *q = dest_queues[(base_idx + i) % num_queues];
		assert(q);

		// TODO the thread might be blocked if it's full.
		// it might hurt performance. We should try other
		// queues first before being blocked.
		/*
		 * If the sender is in thread-safe mode, we have to release the lock
		 * no matter whether it is locked in flush() or in send(). Later,
		 * we have to make sure the lock is grabbed in either case.
		 */
		if (thread_safe)
			pthread_spin_unlock(&lock);
		int ret = q->add(tmp, num_current);
		if (thread_safe)
			pthread_spin_lock(&lock);
		tmp += ret;
		num_current -= ret;
		num_sent += ret;
	}

	/* move the remaining entries to the beginning of the buffer. */
	if (num_current && buf != tmp) {
		for (int i = 0; i < num_current; i++) {
			assert(tmp[i].get_offset() >= 0);
			buf[i] = tmp[i];
			assert(buf[i].get_offset() >= 0);
		}
	}

	if (!locked && thread_safe)
		pthread_spin_unlock(&lock);
	return num_sent;
}

template<class T>
int msg_sender<T>::send_cached(T *msg) {
	/* 
	 * if the buffer is full, and we can't flush
	 * any messages, there is nothing we can do.
	 */
	if (thread_safe)
		pthread_spin_lock(&lock);
	if (num_current == buf_size && flush(true) == 0) {
		if (thread_safe)
			pthread_spin_unlock(&lock);
		return 0;
	}

	buf[num_current++] = *msg;
	if (num_current == buf_size)
		flush(true);
	if (thread_safe)
		pthread_spin_unlock(&lock);
	/* one message has been cached. */
	return 1;
}

/**
 * these are to force to instantiate the templates
 * for io_request and io_reply.
 */
template class thread_safe_FIFO_queue<io_request>;
template class thread_safe_FIFO_queue<io_reply>;
template class blocking_FIFO_queue<io_request>;
template class blocking_FIFO_queue<io_reply>;
template class msg_sender<io_request>;
template class msg_sender<io_reply>;
