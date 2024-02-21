// SPDX-License-Identifier: BSD-3-Clause

#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include <string.h>
#include "osmem.h"
#include "block_meta.h"

#define META_SIZE (sizeof(struct block_meta))
#define MMAP_THRESHOLD (128 * 1024)
#define ALIGNMENT 8

struct block_meta *last;
void *global_base;

// preallocate a big chunck of memory
void *preallocate(size_t size)
{
	void *request = sbrk(MMAP_THRESHOLD);
	struct block_meta *block = NULL;

	DIE((void *)request == (void *)-1, "Preallocation failed");
	block = (struct block_meta *)request;
	block->size = size;
	block->status = 1;

	block->next = NULL;
	block->prev = NULL;

	global_base = block;
	last = block;

	return (void *)(block + 1);
}

// creates a block of memory with a size equal to the parameter given using mmap
void *request_mmap(size_t size)
{
	struct block_meta *block = NULL;

	void *request = mmap(NULL, size + META_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	DIE(request == (void *)-1, "mmap failed");

	block = (struct block_meta *)request;
	block->size = size;
	block->status = 2;

	return (void *)(block + 1);
}

// search the whole list of blocks and returns the best fitting free block
struct block_meta *find_best_block(size_t size)
{
	size_t best_fit_size = MMAP_THRESHOLD;
	struct block_meta *best_fit_block = NULL;
	struct block_meta *block = global_base;

	while (block) {
		if (block->status == 0 && block->size >= size) {
			if (block->size < best_fit_size) {
				best_fit_block = block;
				best_fit_size = block->size;
			}
		}
		block = block->next;
	}

	return best_fit_block;
}

// if it is possible, split the best_fit_block in 2 blocks
void *try_split(struct block_meta *best_fit_block, size_t size)
{
	size_t remaining_size = best_fit_block->size - size;
	struct block_meta *block = NULL;

	// check if we have enough space for another block
	if (remaining_size >= (META_SIZE + 1)) {
		block = (struct block_meta *)((char *)best_fit_block + size + META_SIZE);

		block->size = remaining_size - META_SIZE;
		block->status = 0;
		block->next = best_fit_block->next;
		block->prev = best_fit_block;

		best_fit_block->size = size;
		best_fit_block->status = 1;

		if (best_fit_block->next)
			best_fit_block->next->prev = block;

		best_fit_block->next = block;

		if (block->next == NULL)
			last = block;

		return (void *)(best_fit_block + 1);
	}

	best_fit_block->status = 1;
	return (void *)(best_fit_block + 1);
}

// expand the size of the last block to be equal to the given parameter
void *expand_last(size_t size)
{
	size_t real_size = (size_t)sbrk(0) - (size_t)last;
	size_t new_size = size - real_size + META_SIZE;

	new_size = (new_size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);

	void *request = sbrk(new_size);

	DIE(request == (void *)-1, "sbrk failed");

	last->size = size;
	last->status = 1;

	return (void *)(last + 1);
}

// creates a new block with size equal to the given parameter
void *create_new_block(size_t size)
{
	void *request = sbrk(size + META_SIZE);

	DIE(request == (void *)-1, "sbrk failed");

	struct block_meta *block = (struct block_meta *)request;

	block->size = size;
	block->status = 1;

	last->next = block;
	block->prev = last;
	block->next = NULL;

	last = block;

	return (void *)(block + 1);
}

void *os_malloc(size_t size)
{
	if (size <= 0)
		return NULL;

	// align the size wanted
	size = (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);

	if (size < MMAP_THRESHOLD) {
		// if the global base is NULL, we have to make the preallocation
		if (!global_base)
			return preallocate(size);
		// we try to find the best free block
		struct block_meta *best_fit_block = find_best_block(size);

		// if we find a free block, we try to split it
		if (best_fit_block)
			return try_split(best_fit_block, size);

		// if the last block is free, we expand it
		if (last->status == 0 && last->size < size)
			return expand_last(size);
		// create a new block and return it
		return create_new_block(size);
	}
	// size is >= MMAP_TRESHOLD, so creates a block using mmap
	return request_mmap(size);
}

void os_free(void *ptr)
{
	if (ptr == NULL)
		return;

	struct block_meta *block = (struct block_meta *)ptr - 1;
	int error;

	if (block->status == 0)
		return;

	if (block->status == 1) {
		// we try to coalesce the previous, current and next block
		struct block_meta *prev_block = block->prev;
		struct block_meta *next_block = block->next;

		if (prev_block && prev_block->status == 0) {
			prev_block->size += block->size + META_SIZE;
			prev_block->next = next_block;

			if (next_block)
				next_block->prev = prev_block;

			if (last == block)
				last = prev_block;

			block = prev_block;
		}

		if (next_block && next_block->status == 0) {
			block->size += next_block->size + META_SIZE;
			block->next = next_block->next;

			if (next_block->next)
				next_block->next->prev = block;

			if (last == next_block)
				last = block;
		}

		block->status = 0;
	} else if (block->status == 2) {
		error = munmap(block, block->size + META_SIZE);
		DIE(error == -1, "munmap failed!");
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (size == 0 || nmemb == 0)
		return NULL;

	long page_size = getpagesize();

	DIE(page_size == -1, "Page size error!");

	// calculate the total memory size required
	size_t total_size = nmemb * size;
	void *ptr;
	// align the size wanted
	total_size = (total_size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);

	// if our size is bigger than the page_size we use mmap to allocate and memset to set the memory to 0
	if (total_size + META_SIZE > (unsigned int) page_size) {
		ptr = mmap(NULL, total_size + META_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		struct block_meta *block = (struct block_meta *)ptr;

		block->size = total_size;
		block->status = 2;

		memset(ptr + META_SIZE, 0, total_size);

		return (void *)(block + 1);
	}
	// else, we use malloc to allocate and memset to set the memory to 0
	ptr = os_malloc(total_size);

	if (ptr != NULL)
		memset(ptr, 0, total_size);

	return ptr;
}

// we coalesce the current block with the next block
void coalesce(struct block_meta *block)
{
	if (block->next->status == 0 && block->next != NULL) {
		struct block_meta *next_block = block->next;

		// update the current block size
		if (next_block->next == NULL)
			block->size += next_block->size + META_SIZE;
		else
			block->size += (size_t)next_block->next - (size_t)next_block;

		if (next_block->next != 0)
			next_block->next->prev = block;
		block->next = next_block->next;
	}
}

// if we can, we split the block, in order for it to have the size equal to the given parameter
void *split_realloc(struct block_meta *block, void *ptr, size_t size)
{
	// align the size wanted
	size = (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
	size_t remaining_size = block->size - size - META_SIZE;

	if ((int)remaining_size <= 0) {
		block->size = size;
		return (void *)(block + 1);
	}

	if (block->status == 2) {
		void *dest = os_malloc(size);

		memcpy(dest, ptr, size);
		os_free(ptr);

		return (void *)dest;
	} else if (block->status == 1) {
		struct block_meta *remaining_block = (struct block_meta *)((char *)block + size + META_SIZE);

		remaining_block->size = remaining_size;
		remaining_block->status = 0;
		remaining_block->next = block->next;

		if (block->next != NULL)
			block->next->prev = remaining_block;

		remaining_block->prev = block;
		block->next = remaining_block;
		block->size = size;
		block->status = 1;

		return (void *)(block + 1);
	}
	return NULL;
}

void *os_realloc(void *ptr, size_t size)
{
	if (ptr == NULL) {
		ptr = os_malloc(size);
		return ptr;
	}

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *block = (struct block_meta *)ptr - 1;
	void *dest;

	if (block->status == 0)
		return NULL;

	if (size == block->size)
		return ptr;


	if (size < block->size) {
		return split_realloc(block, ptr, size);
	} else if (size > block->size) {
		// align the size wanted
		size = (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);

		// if the block is the last one, we expand it
		if (block->next == NULL) {
			last = block;
			return expand_last(size);
		}

		size_t real_size = (size_t)block->next - (size_t)block - META_SIZE;

		coalesce(block);
		if (block->size == size) {
			block->size = size;
			return (void *)(block + 1);
		}
		// we try to split the block
		if (real_size >= size)
			return split_realloc(block, ptr, size);
		if (block->size > size)
			return split_realloc(block, ptr, size);
	}
	// we allocate memory using malloc and move it using memcpy
	dest = os_malloc(size);
	memcpy(dest, ptr, block->size);
	os_free(ptr);

	return (void *)dest;
}
