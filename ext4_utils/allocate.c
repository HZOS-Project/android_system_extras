/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "allocate.h"

#include <stdio.h>
#include <stdlib.h>

#include <sparse/sparse.h>

#include "ext4_utils/ext4_utils.h"

struct xattr_list_element {
	struct ext4_inode *inode;
	struct ext4_xattr_header *header;
	struct xattr_list_element *next;
};

struct block_allocation *create_allocation()
{
	struct block_allocation *alloc = malloc(sizeof(struct block_allocation));
	alloc->list.first = NULL;
	alloc->list.last = NULL;
	alloc->oob_list.first = NULL;
	alloc->oob_list.last = NULL;
	alloc->list.iter = NULL;
	alloc->list.partial_iter = 0;
	alloc->oob_list.iter = NULL;
	alloc->oob_list.partial_iter = 0;
	alloc->filename = NULL;
	alloc->next = NULL;
	return alloc;
}

static struct ext4_xattr_header *xattr_list_find(struct ext4_inode *inode)
{
	struct xattr_list_element *element;
	for (element = aux_info.xattrs; element != NULL; element = element->next) {
		if (element->inode == inode)
			return element->header;
	}
	return NULL;
}

static void xattr_list_insert(struct ext4_inode *inode, struct ext4_xattr_header *header)
{
	struct xattr_list_element *element = malloc(sizeof(struct xattr_list_element));
	element->inode = inode;
	element->header = header;
	element->next = aux_info.xattrs;
	aux_info.xattrs = element;
}

static void region_list_remove(struct region_list *list, struct region *reg)
{
	if (reg->prev)
		reg->prev->next = reg->next;

	if (reg->next)
		reg->next->prev = reg->prev;

	if (list->first == reg)
		list->first = reg->next;

	if (list->last == reg)
		list->last = reg->prev;

	reg->next = NULL;
	reg->prev = NULL;
}

void region_list_append(struct region_list *list, struct region *reg)
{
	if (list->first == NULL) {
		list->first = reg;
		list->last = reg;
		list->iter = reg;
		list->partial_iter = 0;
		reg->prev = NULL;
	} else {
		list->last->next = reg;
		reg->prev = list->last;
		list->last = reg;
	}
	reg->next = NULL;
}

void region_list_merge(struct region_list *list1, struct region_list *list2)
{
	if (list1->first == NULL) {
		list1->first = list2->first;
		list1->last = list2->last;
		list1->iter = list2->first;
		list1->partial_iter = 0;
		list1->first->prev = NULL;
	} else {
		list1->last->next = list2->first;
		list2->first->prev = list1->last;
		list1->last = list2->last;
	}
}
#if 0
static void dump_starting_from(struct region *reg)
{
	for (; reg; reg = reg->next) {
		printf("%p: Blocks %d-%d (%d)\n", reg,
			   reg->block, reg->block + reg->len - 1, reg->len)
	}
}

static void dump_region_lists(struct block_allocation *alloc) {

	printf("Main list:\n");
	dump_starting_from(alloc->list.first);

	printf("OOB list:\n");
	dump_starting_from(alloc->oob_list.first);
}
#endif

void print_blocks(FILE* f, struct block_allocation *alloc, char separator)
{
	struct region *reg;
	fputc(' ', f);
	for (reg = alloc->list.first; reg; reg = reg->next) {
		if (reg->len == 1) {
			fprintf(f, "%d", reg->block);
		} else {
			fprintf(f, "%d-%d", reg->block, reg->block + reg->len - 1);
		}
		fputc(separator, f);
	}
	fputc('\n', f);
}

void append_region(struct block_allocation *alloc,
		u32 block, u32 len, int bg_num)
{
	struct region *reg;
	reg = malloc(sizeof(struct region));
	reg->block = block;
	reg->len = len;
	reg->bg = bg_num;
	reg->next = NULL;

	region_list_append(&alloc->list, reg);
}

static void allocate_bg_inode_table(struct block_group_info *bg)
{
	if (bg->inode_table != NULL)
		return;

	u32 block = bg->first_block + 2;

	if (bg->has_superblock)
		block += aux_info.bg_desc_blocks + info.bg_desc_reserve_blocks + 1;

	bg->inode_table = calloc(aux_info.inode_table_blocks, info.block_size);
	if (bg->inode_table == NULL)
		critical_error_errno("calloc");

	sparse_file_add_data(ext4_sparse_file, bg->inode_table,
			aux_info.inode_table_blocks	* info.block_size, block);

	bg->flags &= ~EXT4_BG_INODE_UNINIT;
}

static int bitmap_set_bit(u8 *bitmap, u32 bit)
{
	if (bitmap[bit / 8] & 1 << (bit % 8))
		return 1;

	bitmap[bit / 8] |= 1 << (bit % 8);
	return 0;
}

static int bitmap_set_8_bits(u8 *bitmap, u32 bit)
{
	int ret = bitmap[bit / 8];
	bitmap[bit / 8] = 0xFF;
	return ret;
}

/* Marks a the first num_blocks blocks in a block group as used, and accounts
 for them in the block group free block info. */
static int reserve_blocks(struct block_group_info *bg, u32 bg_num, u32 start, u32 num)
{
	unsigned int i = 0;

	u32 block = start;
	for (i = 0; i < num && block % 8 != 0; i++, block++) {
		if (bitmap_set_bit(bg->block_bitmap, block)) {
			error("attempted to reserve already reserved block %d in block group %d", block, bg_num);
			return -1;
		}
	}

	for (; i + 8 <= (num & ~7); i += 8, block += 8) {
		if (bitmap_set_8_bits(bg->block_bitmap, block)) {
			error("attempted to reserve already reserved block %d in block group %d", block, bg_num);
			return -1;
		}
	}

	for (; i < num; i++, block++) {
		if (bitmap_set_bit(bg->block_bitmap, block)) {
			error("attempted to reserve already reserved block %d in block group %d", block, bg_num);
			return -1;
		}
	}

	bg->free_blocks -= num;

	return 0;
}

static void free_blocks(struct block_group_info *bg, u32 block, u32 num_blocks)
{
	unsigned int i;
	for (i = 0; i < num_blocks; i++, block--)
		bg->block_bitmap[block / 8] &= ~(1 << (block % 8));
	bg->free_blocks += num_blocks;
	for (i = bg->chunk_count; i > 0 ;) {
		--i;
		if (bg->chunks[i].len >= num_blocks && bg->chunks[i].block <= block) {
			if (bg->chunks[i].block == block) {
				bg->chunks[i].block += num_blocks;
				bg->chunks[i].len -= num_blocks;
			} else if (bg->chunks[i].block + bg->chunks[i].len - 1 == block + num_blocks) {
				bg->chunks[i].len -= num_blocks;
			}
			break;
		}
	}
}

/* Reduces an existing allocation by len blocks by return the last blocks
   to the free pool in their block group. Assumes that the blocks being
   returned were the last ones allocated out of the block group */
void reduce_allocation(struct block_allocation *alloc, u32 len)
{
	while (len) {
		struct region *last_reg = alloc->list.last;
		struct block_group_info *bg = &aux_info.bgs[last_reg->bg];

		if (last_reg->len > len) {
			free_blocks(bg, last_reg->block + last_reg->len - bg->first_block - 1, len);
			last_reg->len -= len;
			len = 0;
		} else {
			struct region *reg = alloc->list.last->prev;
			free_blocks(bg, last_reg->block + last_reg->len - bg->first_block - 1, last_reg->len);
			len -= last_reg->len;
			if (reg) {
				reg->next = NULL;
			} else {
				alloc->list.first = NULL;
				alloc->list.last = NULL;
				alloc->list.iter = NULL;
				alloc->list.partial_iter = 0;
			}
			free(last_reg);
		}
	}
}

static void init_bg(struct block_group_info *bg, unsigned int i)
{
	int header_blocks = 2 + aux_info.inode_table_blocks;

	bg->has_superblock = ext4_bg_has_super_block(i);

	if (bg->has_superblock)
		header_blocks += 1 + aux_info.bg_desc_blocks + info.bg_desc_reserve_blocks;

	bg->bitmaps = calloc(info.block_size, 2);
	bg->block_bitmap = bg->bitmaps;
	bg->inode_bitmap = bg->bitmaps + info.block_size;

	bg->header_blocks = header_blocks;
	bg->first_block = aux_info.first_data_block + i * info.blocks_per_group;

	u32 block = bg->first_block;
	if (bg->has_superblock)
		block += 1 + aux_info.bg_desc_blocks +  info.bg_desc_reserve_blocks;
	sparse_file_add_data(ext4_sparse_file, bg->bitmaps, 2 * info.block_size,
			block);

	bg->data_blocks_used = 0;
	bg->free_blocks = info.blocks_per_group;
	bg->free_inodes = info.inodes_per_group;
	bg->first_free_inode = 1;
	bg->flags = EXT4_BG_INODE_UNINIT;

	bg->chunk_count = 0;
	bg->max_chunk_count = 1;
	bg->chunks = (struct region*) calloc(bg->max_chunk_count, sizeof(struct region));

	if (reserve_blocks(bg, i, 0, bg->header_blocks) < 0)
		error("failed to reserve %u blocks in block group %u\n", bg->header_blocks, i);
	// Add empty starting delimiter chunk
	reserve_bg_chunk(i, bg->header_blocks, 0);

	if (bg->first_block + info.blocks_per_group > aux_info.len_blocks) {
		u32 overrun = bg->first_block + info.blocks_per_group - aux_info.len_blocks;
		reserve_blocks(bg, i, info.blocks_per_group - overrun, overrun);
		// Add empty ending delimiter chunk
		reserve_bg_chunk(i, info.blocks_per_group - overrun, 0);
	} else {
		reserve_bg_chunk(i, info.blocks_per_group - 1, 0);
	}

}

void block_allocator_init()
{
	unsigned int i;

	aux_info.bgs = calloc(sizeof(struct block_group_info), aux_info.groups);
	if (aux_info.bgs == NULL)
		critical_error_errno("calloc");

	for (i = 0; i < aux_info.groups; i++)
		init_bg(&aux_info.bgs[i], i);
}

void block_allocator_free()
{
	unsigned int i;

	for (i = 0; i < aux_info.groups; i++) {
		free(aux_info.bgs[i].bitmaps);
		free(aux_info.bgs[i].inode_table);
	}
	free(aux_info.bgs);
}

/* Allocate a single block and return its block number */
u32 allocate_block()
{
	u32 block;
	struct block_allocation *blk_alloc = allocate_blocks(1);
	if (!blk_alloc) {
		return EXT4_ALLOCATE_FAILED;
	}
	block = blk_alloc->list.first->block;
	free_alloc(blk_alloc);
	return block;
}

static struct region *ext4_allocate_best_fit_partial(u32 len)
{
	unsigned int i, j;
	unsigned int found_bg = 0, found_prev_chunk = 0, found_block = 0;
	u32 found_allocate_len = 0;
	bool minimize = false;
	struct block_group_info *bgs = aux_info.bgs;
	struct region *reg;

	for (i = 0; i < aux_info.groups; i++) {
		for (j = 1; j < bgs[i].chunk_count; j++) {
			u32 hole_start, hole_size;
			hole_start = bgs[i].chunks[j-1].block + bgs[i].chunks[j-1].len;
			hole_size =  bgs[i].chunks[j].block - hole_start;
			if (hole_size == len) {
				// Perfect fit i.e. right between 2 chunks no need to keep searching
				found_bg = i;
				found_prev_chunk = j - 1;
				found_block = hole_start;
				found_allocate_len = hole_size;
				goto done;
			} else if (hole_size > len && (found_allocate_len == 0 || (found_allocate_len > hole_size))) {
				found_bg = i;
				found_prev_chunk = j - 1;
				found_block = hole_start;
				found_allocate_len = hole_size;
				minimize = true;
			} else if (!minimize) {
				if (found_allocate_len < hole_size) {
					found_bg = i;
					found_prev_chunk = j - 1;
					found_block = hole_start;
					found_allocate_len = hole_size;
				}
			}
		}
	}

	if (found_allocate_len == 0) {
		error("failed to allocate %u blocks, out of space?", len);
		return NULL;
	}
	if (found_allocate_len > len) found_allocate_len = len;
done:
	// reclaim allocated space in chunk
	bgs[found_bg].chunks[found_prev_chunk].len += found_allocate_len;
	if (reserve_blocks(&bgs[found_bg],
				found_bg,
				found_block,
				found_allocate_len) < 0) {
		error("failed to reserve %u blocks in block group %u\n", found_allocate_len, found_bg);
		return NULL;
	}
	bgs[found_bg].data_blocks_used += found_allocate_len;
	reg = malloc(sizeof(struct region));
	reg->block = found_block + bgs[found_bg].first_block;
	reg->len = found_allocate_len;
	reg->next = NULL;
	reg->prev = NULL;
	reg->bg = found_bg;
	return reg;
}

static struct region *ext4_allocate_best_fit(u32 len)
{
	struct region *first_reg = NULL;
	struct region *prev_reg = NULL;
	struct region *reg;

	while (len > 0) {
		reg = ext4_allocate_best_fit_partial(len);
		if (reg == NULL)
			return NULL;

		if (first_reg == NULL)
			first_reg = reg;

		if (prev_reg) {
			prev_reg->next = reg;
			reg->prev = prev_reg;
		}

		prev_reg = reg;
		len -= reg->len;
	}

	return first_reg;
}

/* Allocate len blocks.  The blocks may be spread across multiple block groups,
   and are returned in a linked list of the blocks in each block group.  The
   allocation algorithm is:
	  1.  If the remaining allocation is larger than any available contiguous region,
		  allocate the largest contiguous region and loop
	  2.  Otherwise, allocate the smallest contiguous region that it fits in
*/
struct block_allocation *allocate_blocks(u32 len)
{
	struct region *reg = ext4_allocate_best_fit(len);

	if (reg == NULL)
		return NULL;

	struct block_allocation *alloc = create_allocation();
	alloc->list.first = reg;
	while (reg->next != NULL)
		reg = reg->next;
	alloc->list.last = reg;
	alloc->list.iter = alloc->list.first;
	alloc->list.partial_iter = 0;
	return alloc;
}

/* Returns the number of discontiguous regions used by an allocation */
int block_allocation_num_regions(struct block_allocation *alloc)
{
	unsigned int i;
	struct region *reg = alloc->list.first;

	for (i = 0; reg != NULL; reg = reg->next)
		i++;

	return i;
}

int block_allocation_len(struct block_allocation *alloc)
{
	unsigned int i;
	struct region *reg = alloc->list.first;

	for (i = 0; reg != NULL; reg = reg->next)
		i += reg->len;

	return i;
}

/* Returns the block number of the block'th block in an allocation */
u32 get_block(struct block_allocation *alloc, u32 block)
{
	struct region *reg = alloc->list.iter;
	block += alloc->list.partial_iter;

	for (; reg; reg = reg->next) {
		if (block < reg->len)
			return reg->block + block;
		block -= reg->len;
	}
	return EXT4_ALLOCATE_FAILED;
}

u32 get_oob_block(struct block_allocation *alloc, u32 block)
{
	struct region *reg = alloc->oob_list.iter;
	block += alloc->oob_list.partial_iter;

	for (; reg; reg = reg->next) {
		if (block < reg->len)
			return reg->block + block;
		block -= reg->len;
	}
	return EXT4_ALLOCATE_FAILED;
}

/* Gets the starting block and length in blocks of the first region
   of an allocation */
void get_region(struct block_allocation *alloc, u32 *block, u32 *len)
{
	*block = alloc->list.iter->block;
	*len = alloc->list.iter->len - alloc->list.partial_iter;
}

/* Move to the next region in an allocation */
void get_next_region(struct block_allocation *alloc)
{
	alloc->list.iter = alloc->list.iter->next;
	alloc->list.partial_iter = 0;
}

/* Returns the number of free blocks in a block group */
u32 get_free_blocks(u32 bg)
{
	return aux_info.bgs[bg].free_blocks;
}

int last_region(struct block_allocation *alloc)
{
	return (alloc->list.iter == NULL);
}

void rewind_alloc(struct block_allocation *alloc)
{
	alloc->list.iter = alloc->list.first;
	alloc->list.partial_iter = 0;
}

static struct region *do_split_allocation(struct block_allocation *alloc, u32 len)
{
	struct region *reg = alloc->list.iter;
	struct region *new;
	struct region *tmp;

	while (reg && len >= reg->len) {
		len -= reg->len;
		reg = reg->next;
	}

	if (reg == NULL && len > 0)
		return NULL;

	if (len > 0) {
		new = malloc(sizeof(struct region));

		new->bg = reg->bg;
		new->block = reg->block + len;
		new->len = reg->len - len;
		new->next = reg->next;
		new->prev = reg;

		reg->next = new;
		reg->len = len;

		tmp = alloc->list.iter;
		alloc->list.iter = new;
		return tmp;
	} else {
		return reg;
	}
}

/* Splits an allocation into two allocations.  The returned allocation will
   point to the first half, and the original allocation ptr will point to the
   second half. */
static struct region *split_allocation(struct block_allocation *alloc, u32 len)
{
	/* First make sure there is a split at the current ptr */
	do_split_allocation(alloc, alloc->list.partial_iter);

	/* Then split off len blocks */
	struct region *middle = do_split_allocation(alloc, len);
	alloc->list.partial_iter = 0;
	return middle;
}

/* Reserve the next blocks for oob data (indirect or extent blocks) */
int reserve_oob_blocks(struct block_allocation *alloc, int blocks)
{
	struct region *oob = split_allocation(alloc, blocks);
	struct region *next;

	if (oob == NULL)
		return -1;

	while (oob && oob != alloc->list.iter) {
		next = oob->next;
		region_list_remove(&alloc->list, oob);
		region_list_append(&alloc->oob_list, oob);
		oob = next;
	}

	return 0;
}

static int advance_list_ptr(struct region_list *list, int blocks)
{
	struct region *reg = list->iter;

	while (reg != NULL && blocks > 0) {
		if (reg->len > list->partial_iter + blocks) {
			list->partial_iter += blocks;
			return 0;
		}

		blocks -= (reg->len - list->partial_iter);
		list->partial_iter = 0;
		reg = reg->next;
	}

	if (blocks > 0)
		return -1;

	return 0;
}

/* Move the allocation pointer forward */
int advance_blocks(struct block_allocation *alloc, int blocks)
{
	return advance_list_ptr(&alloc->list, blocks);
}

int advance_oob_blocks(struct block_allocation *alloc, int blocks)
{
	return advance_list_ptr(&alloc->oob_list, blocks);
}

int append_oob_allocation(struct block_allocation *alloc, u32 len)
{
	struct region *reg = ext4_allocate_best_fit(len);

	if (reg == NULL) {
		error("failed to allocate %d blocks", len);
		return -1;
	}

	for (; reg; reg = reg->next)
		region_list_append(&alloc->oob_list, reg);

	return 0;
}

/* Returns an ext4_inode structure for an inode number */
struct ext4_inode *get_inode(u32 inode)
{
	inode -= 1;
	int bg = inode / info.inodes_per_group;
	inode %= info.inodes_per_group;

	allocate_bg_inode_table(&aux_info.bgs[bg]);
	return (struct ext4_inode *)(aux_info.bgs[bg].inode_table + inode *
		info.inode_size);
}

struct ext4_xattr_header *get_xattr_block_for_inode(struct ext4_inode *inode)
{
	struct ext4_xattr_header *block = xattr_list_find(inode);
	if (block != NULL)
		return block;

	u32 block_num = allocate_block();
	block = calloc(info.block_size, 1);
	if (block == NULL) {
		error("get_xattr: failed to allocate %d", info.block_size);
		return NULL;
	}

	block->h_magic = cpu_to_le32(EXT4_XATTR_MAGIC);
	block->h_refcount = cpu_to_le32(1);
	block->h_blocks = cpu_to_le32(1);
	inode->i_blocks_lo = cpu_to_le32(le32_to_cpu(inode->i_blocks_lo) + (info.block_size / 512));
	inode->i_file_acl_lo = cpu_to_le32(block_num);

	int result = sparse_file_add_data(ext4_sparse_file, block, info.block_size, block_num);
	if (result != 0) {
		error("get_xattr: sparse_file_add_data failure %d", result);
		free(block);
		return NULL;
	}
	xattr_list_insert(inode, block);
	return block;
}

/* Mark the first len inodes in a block group as used */
u32 reserve_inodes(int bg, u32 num)
{
	unsigned int i;
	u32 inode;

	if (get_free_inodes(bg) < num)
		return EXT4_ALLOCATE_FAILED;

	for (i = 0; i < num; i++) {
		inode = aux_info.bgs[bg].first_free_inode + i - 1;
		aux_info.bgs[bg].inode_bitmap[inode / 8] |= 1 << (inode % 8);
	}

	inode = aux_info.bgs[bg].first_free_inode;

	aux_info.bgs[bg].first_free_inode += num;
	aux_info.bgs[bg].free_inodes -= num;

	return inode;
}

/* Returns the first free inode number
   TODO: Inodes should be allocated in the block group of the data? */
u32 allocate_inode()
{
	unsigned int bg;
	u32 inode;

	for (bg = 0; bg < aux_info.groups; bg++) {
		inode = reserve_inodes(bg, 1);
		if (inode != EXT4_ALLOCATE_FAILED)
			return bg * info.inodes_per_group + inode;
	}

	return EXT4_ALLOCATE_FAILED;
}

/* Returns the number of free inodes in a block group */
u32 get_free_inodes(u32 bg)
{
	return aux_info.bgs[bg].free_inodes;
}

/* Increments the directory count of the block group that contains inode */
void add_directory(u32 inode)
{
	int bg = (inode - 1) / info.inodes_per_group;
	aux_info.bgs[bg].used_dirs += 1;
}

/* Returns the number of inodes in a block group that are directories */
u16 get_directories(int bg)
{
	return aux_info.bgs[bg].used_dirs;
}

/* Returns the flags for a block group */
u16 get_bg_flags(int bg)
{
	return aux_info.bgs[bg].flags;
}

/* Frees the memory used by a linked list of allocation regions */
void free_alloc(struct block_allocation *alloc)
{
	struct region *reg;

	reg = alloc->list.first;
	while (reg) {
		struct region *next = reg->next;
		free(reg);
		reg = next;
	}

	reg = alloc->oob_list.first;
	while (reg) {
		struct region *next = reg->next;
		free(reg);
		reg = next;
	}

	free(alloc);
}

void reserve_bg_chunk(int bg, u32 start_block, u32 size) {
	struct block_group_info *bgs = aux_info.bgs;
	int chunk_count;
	if (bgs[bg].chunk_count == bgs[bg].max_chunk_count) {
		bgs[bg].max_chunk_count *= 2;
		bgs[bg].chunks = realloc(bgs[bg].chunks, bgs[bg].max_chunk_count * sizeof(struct region));
		if (!bgs[bg].chunks)
			critical_error("realloc failed");
	}
	chunk_count = bgs[bg].chunk_count;
	bgs[bg].chunks[chunk_count].block = start_block;
	bgs[bg].chunks[chunk_count].len = size;
	bgs[bg].chunks[chunk_count].bg = bg;
	bgs[bg].chunk_count++;
}

int reserve_blocks_for_allocation(struct block_allocation *alloc) {
	struct region *reg;
	struct block_group_info *bgs = aux_info.bgs;

	if (!alloc) return 0;
	reg = alloc->list.first;
	while (reg != NULL) {
		if (reserve_blocks(&bgs[reg->bg], reg->bg, reg->block - bgs[reg->bg].first_block, reg->len) < 0) {
			return -1;
		}
		reg = reg->next;
	}
	return 0;
}

