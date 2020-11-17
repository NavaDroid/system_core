/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <csignal>

#include <libsnapshot/snapuserd.h>
#include <libsnapshot/snapuserd_client.h>
#include <libsnapshot/snapuserd_daemon.h>
#include <libsnapshot/snapuserd_server.h>

namespace android {
namespace snapshot {

using namespace android;
using namespace android::dm;
using android::base::unique_fd;

static constexpr size_t PAYLOAD_SIZE = (1UL << 16);

static_assert(PAYLOAD_SIZE >= BLOCK_SIZE);

void BufferSink::Initialize(size_t size) {
    buffer_size_ = size;
    buffer_offset_ = 0;
    buffer_ = std::make_unique<uint8_t[]>(size);
}

void* BufferSink::GetPayloadBuffer(size_t size) {
    if ((buffer_size_ - buffer_offset_) < size) return nullptr;

    char* buffer = reinterpret_cast<char*>(GetBufPtr());
    struct dm_user_message* msg = (struct dm_user_message*)(&(buffer[0]));
    return (char*)msg->payload.buf + buffer_offset_;
}

void* BufferSink::GetBuffer(size_t requested, size_t* actual) {
    void* buf = GetPayloadBuffer(requested);
    if (!buf) {
        *actual = 0;
        return nullptr;
    }
    *actual = requested;
    return buf;
}

struct dm_user_header* BufferSink::GetHeaderPtr() {
    CHECK(sizeof(struct dm_user_header) <= buffer_size_);
    char* buf = reinterpret_cast<char*>(GetBufPtr());
    struct dm_user_header* header = (struct dm_user_header*)(&(buf[0]));
    return header;
}

Snapuserd::Snapuserd(const std::string& misc_name, const std::string& cow_device,
                     const std::string& backing_device) {
    misc_name_ = misc_name;
    cow_device_ = cow_device;
    backing_store_device_ = backing_device;
    control_device_ = "/dev/dm-user/" + misc_name;
}

// Construct kernel COW header in memory
// This header will be in sector 0. The IO
// request will always be 4k. After constructing
// the header, zero out the remaining block.
void Snapuserd::ConstructKernelCowHeader() {
    void* buffer = bufsink_.GetPayloadBuffer(BLOCK_SIZE);
    CHECK(buffer != nullptr);

    memset(buffer, 0, BLOCK_SIZE);

    struct disk_header* dh = reinterpret_cast<struct disk_header*>(buffer);

    dh->magic = SNAP_MAGIC;
    dh->valid = SNAPSHOT_VALID;
    dh->version = SNAPSHOT_DISK_VERSION;
    dh->chunk_size = CHUNK_SIZE;
}

// Start the replace operation. This will read the
// internal COW format and if the block is compressed,
// it will be de-compressed.
bool Snapuserd::ProcessReplaceOp(const CowOperation* cow_op) {
    if (!reader_->ReadData(*cow_op, &bufsink_)) {
        LOG(ERROR) << "ReadData failed for chunk: " << cow_op->new_block;
        return false;
    }

    return true;
}

// Start the copy operation. This will read the backing
// block device which is represented by cow_op->source.
bool Snapuserd::ProcessCopyOp(const CowOperation* cow_op) {
    void* buffer = bufsink_.GetPayloadBuffer(BLOCK_SIZE);
    CHECK(buffer != nullptr);

    // Issue a single 4K IO. However, this can be optimized
    // if the successive blocks are contiguous.
    if (!android::base::ReadFullyAtOffset(backing_store_fd_, buffer, BLOCK_SIZE,
                                          cow_op->source * BLOCK_SIZE)) {
        LOG(ERROR) << "Copy-op failed. Read from backing store at: " << cow_op->source;
        return false;
    }

    return true;
}

bool Snapuserd::ProcessZeroOp() {
    // Zero out the entire block
    void* buffer = bufsink_.GetPayloadBuffer(BLOCK_SIZE);
    CHECK(buffer != nullptr);

    memset(buffer, 0, BLOCK_SIZE);
    return true;
}

/*
 * Read the data of size bytes from a given chunk.
 *
 * Kernel can potentially merge the blocks if the
 * successive chunks are contiguous. For chunk size of 8,
 * there can be 256 disk exceptions; and if
 * all 256 disk exceptions are contiguous, kernel can merge
 * them into a single IO.
 *
 * Since each chunk in the disk exception
 * mapping represents a 4k block, kernel can potentially
 * issue 256*4k = 1M IO in one shot.
 *
 * Even though kernel assumes that the blocks are
 * contiguous, we need to split the 1M IO into 4k chunks
 * as each operation represents 4k and it can either be:
 *
 * 1: Replace operation
 * 2: Copy operation
 * 3: Zero operation
 *
 */
bool Snapuserd::ReadData(chunk_t chunk, size_t size) {
    size_t read_size = size;
    bool ret = true;
    chunk_t chunk_key = chunk;
    uint32_t stride;
    lldiv_t divresult;

    // Size should always be aligned
    CHECK((read_size & (BLOCK_SIZE - 1)) == 0);

    while (read_size > 0) {
        const CowOperation* cow_op = chunk_map_[chunk_key];
        CHECK(cow_op != nullptr);

        switch (cow_op->type) {
            case kCowReplaceOp: {
                ret = ProcessReplaceOp(cow_op);
                break;
            }

            case kCowZeroOp: {
                ret = ProcessZeroOp();
                break;
            }

            case kCowCopyOp: {
                ret = ProcessCopyOp(cow_op);
                break;
            }

            default: {
                LOG(ERROR) << "Unknown operation-type found: " << cow_op->type;
                ret = false;
                break;
            }
        }

        if (!ret) {
            LOG(ERROR) << "ReadData failed for operation: " << cow_op->type;
            return false;
        }

        // Update the buffer offset
        bufsink_.UpdateBufferOffset(BLOCK_SIZE);

        read_size -= BLOCK_SIZE;

        // Start iterating the chunk incrementally; Since while
        // constructing the metadata, we know that the chunk IDs
        // are contiguous
        chunk_key += 1;

        if (cow_op->type == kCowCopyOp) CHECK(read_size == 0);

        // This is similar to the way when chunk IDs were assigned
        // in ReadMetadata().
        //
        // Skip if the chunk id represents a metadata chunk.
        stride = exceptions_per_area_ + 1;
        divresult = lldiv(chunk_key, stride);
        if (divresult.rem == NUM_SNAPSHOT_HDR_CHUNKS) {
            // Crossing exception boundary. Kernel will never
            // issue IO which is spanning between a data chunk
            // and a metadata chunk. This should be perfectly aligned.
            //
            // Since the input read_size is 4k aligned, we will
            // always end up reading all 256 data chunks in one area.
            // Thus, every multiple of 4K IO represents 256 data chunks
            CHECK(read_size == 0);
            break;
        }
    }

    // Reset the buffer offset
    bufsink_.ResetBufferOffset();
    return ret;
}

/*
 * dm-snap does prefetch reads while reading disk-exceptions.
 * By default, prefetch value is set to 12; this means that
 * dm-snap will issue 12 areas wherein each area is a 4k page
 * of disk-exceptions.
 *
 * If during prefetch, if the chunk-id seen is beyond the
 * actual number of metadata page, fill the buffer with zero.
 * When dm-snap starts parsing the buffer, it will stop
 * reading metadata page once the buffer content is zero.
 */
bool Snapuserd::ZerofillDiskExceptions(size_t read_size) {
    size_t size = exceptions_per_area_ * sizeof(struct disk_exception);

    if (read_size > size) {
        return false;
    }

    void* buffer = bufsink_.GetPayloadBuffer(size);
    CHECK(buffer != nullptr);

    memset(buffer, 0, size);
    return true;
}

/*
 * A disk exception is a simple mapping of old_chunk to new_chunk.
 * When dm-snapshot device is created, kernel requests these mapping.
 *
 * Each disk exception is of size 16 bytes. Thus a single 4k page can
 * have:
 *
 * exceptions_per_area_ = 4096/16 = 256. This entire 4k page
 * is considered a metadata page and it is represented by chunk ID.
 *
 * Convert the chunk ID to index into the vector which gives us
 * the metadata page.
 */
bool Snapuserd::ReadDiskExceptions(chunk_t chunk, size_t read_size) {
    uint32_t stride = exceptions_per_area_ + 1;
    size_t size;

    // ChunkID to vector index
    lldiv_t divresult = lldiv(chunk, stride);

    if (divresult.quot < vec_.size()) {
        size = exceptions_per_area_ * sizeof(struct disk_exception);

        if (read_size > size) {
            return false;
        }

        void* buffer = bufsink_.GetPayloadBuffer(size);
        CHECK(buffer != nullptr);

        memcpy(buffer, vec_[divresult.quot].get(), size);
    } else {
        return ZerofillDiskExceptions(read_size);
    }

    return true;
}

loff_t Snapuserd::GetMergeStartOffset(void* merged_buffer, void* unmerged_buffer,
                                      int* unmerged_exceptions) {
    loff_t offset = 0;
    *unmerged_exceptions = 0;

    while (*unmerged_exceptions <= exceptions_per_area_) {
        struct disk_exception* merged_de =
                reinterpret_cast<struct disk_exception*>((char*)merged_buffer + offset);
        struct disk_exception* cow_de =
                reinterpret_cast<struct disk_exception*>((char*)unmerged_buffer + offset);

        // Unmerged op by the kernel
        if (merged_de->old_chunk != 0) {
            CHECK(merged_de->new_chunk != 0);
            CHECK(merged_de->old_chunk == cow_de->old_chunk);
            CHECK(merged_de->new_chunk == cow_de->new_chunk);

            offset += sizeof(struct disk_exception);
            *unmerged_exceptions += 1;
            continue;
        }

        // Merge complete on this exception. However, we don't know how many
        // merged in this cycle; hence break here.
        CHECK(merged_de->new_chunk == 0);
        CHECK(merged_de->old_chunk == 0);

        break;
    }

    CHECK(!(*unmerged_exceptions == exceptions_per_area_));

    LOG(DEBUG) << "Unmerged_Exceptions: " << *unmerged_exceptions << " Offset: " << offset;
    return offset;
}

int Snapuserd::GetNumberOfMergedOps(void* merged_buffer, void* unmerged_buffer, loff_t offset,
                                    int unmerged_exceptions) {
    int merged_ops_cur_iter = 0;

    // Find the operations which are merged in this cycle.
    while ((unmerged_exceptions + merged_ops_cur_iter) <= exceptions_per_area_) {
        struct disk_exception* merged_de =
                reinterpret_cast<struct disk_exception*>((char*)merged_buffer + offset);
        struct disk_exception* cow_de =
                reinterpret_cast<struct disk_exception*>((char*)unmerged_buffer + offset);

        CHECK(merged_de->new_chunk == 0);
        CHECK(merged_de->old_chunk == 0);

        if (cow_de->new_chunk != 0) {
            merged_ops_cur_iter += 1;
            offset += sizeof(struct disk_exception);
            // zero out to indicate that operation is merged.
            cow_de->old_chunk = 0;
            cow_de->new_chunk = 0;
        } else if (cow_de->old_chunk == 0) {
            // Already merged op in previous iteration or
            // This could also represent a partially filled area.
            //
            // If the op was merged in previous cycle, we don't have
            // to count them.
            CHECK(cow_de->new_chunk == 0);
            break;
        } else {
            LOG(ERROR) << "Error in merge operation. Found invalid metadata";
            LOG(ERROR) << "merged_de-old-chunk: " << merged_de->old_chunk;
            LOG(ERROR) << "merged_de-new-chunk: " << merged_de->new_chunk;
            LOG(ERROR) << "cow_de-old-chunk: " << cow_de->old_chunk;
            LOG(ERROR) << "cow_de-new-chunk: " << cow_de->new_chunk;
            return -1;
        }
    }

    return merged_ops_cur_iter;
}

bool Snapuserd::AdvanceMergedOps(int merged_ops_cur_iter) {
    // Advance the merge operation pointer in the
    // vector.
    //
    // cowop_iter_ is already initialized in ReadMetadata(). Just resume the
    // merge process
    while (!cowop_iter_->Done() && merged_ops_cur_iter) {
        const CowOperation* cow_op = &cowop_iter_->Get();
        CHECK(cow_op != nullptr);

        if (cow_op->type == kCowFooterOp || cow_op->type == kCowLabelOp) {
            cowop_iter_->Next();
            continue;
        }

        if (!(cow_op->type == kCowReplaceOp || cow_op->type == kCowZeroOp ||
              cow_op->type == kCowCopyOp)) {
            LOG(ERROR) << "Unknown operation-type found during merge: " << cow_op->type;
            return false;
        }

        merged_ops_cur_iter -= 1;
        LOG(DEBUG) << "Merge op found of type " << cow_op->type
                   << "Pending-merge-ops: " << merged_ops_cur_iter;
        cowop_iter_->Next();
    }

    if (cowop_iter_->Done()) {
        CHECK(merged_ops_cur_iter == 0);
        LOG(DEBUG) << "All cow operations merged successfully in this cycle";
    }

    return true;
}

bool Snapuserd::ProcessMergeComplete(chunk_t chunk, void* buffer) {
    uint32_t stride = exceptions_per_area_ + 1;
    CowHeader header;

    if (!reader_->GetHeader(&header)) {
        LOG(ERROR) << "Failed to get header";
        return false;
    }

    // ChunkID to vector index
    lldiv_t divresult = lldiv(chunk, stride);
    CHECK(divresult.quot < vec_.size());
    LOG(DEBUG) << "ProcessMergeComplete: chunk: " << chunk << " Metadata-Index: " << divresult.quot;

    int unmerged_exceptions = 0;
    loff_t offset = GetMergeStartOffset(buffer, vec_[divresult.quot].get(), &unmerged_exceptions);

    int merged_ops_cur_iter =
            GetNumberOfMergedOps(buffer, vec_[divresult.quot].get(), offset, unmerged_exceptions);

    // There should be at least one operation merged in this cycle
    CHECK(merged_ops_cur_iter > 0);
    if (!AdvanceMergedOps(merged_ops_cur_iter)) return false;

    header.num_merge_ops += merged_ops_cur_iter;
    reader_->UpdateMergeProgress(merged_ops_cur_iter);
    if (!writer_->CommitMerge(merged_ops_cur_iter)) {
        LOG(ERROR) << "CommitMerge failed...";
        return false;
    }

    LOG(DEBUG) << "Merge success";
    return true;
}

bool Snapuserd::IsChunkIdMetadata(chunk_t chunk) {
    uint32_t stride = exceptions_per_area_ + 1;
    lldiv_t divresult = lldiv(chunk, stride);

    return (divresult.rem == NUM_SNAPSHOT_HDR_CHUNKS);
}

// Find the next free chunk-id to be assigned. Check if the next free
// chunk-id represents a metadata page. If so, skip it.
chunk_t Snapuserd::GetNextAllocatableChunkId(chunk_t chunk) {
    chunk_t next_chunk = chunk + 1;

    if (IsChunkIdMetadata(next_chunk)) {
        next_chunk += 1;
    }
    return next_chunk;
}

/*
 * Read the metadata from COW device and
 * construct the metadata as required by the kernel.
 *
 * Please see design on kernel COW format
 *
 * 1: Read the metadata from internal COW device
 * 2: There are 3 COW operations:
 *     a: Replace op
 *     b: Copy op
 *     c: Zero op
 * 3: For each of the 3 operations, op->new_block
 *    represents the block number in the base device
 *    for which one of the 3 operations have to be applied.
 *    This represents the old_chunk in the kernel COW format
 * 4: We need to assign new_chunk for a corresponding old_chunk
 * 5: The algorithm is similar to how kernel assigns chunk number
 *    while creating exceptions. However, there are few cases
 *    which needs to be addressed here:
 *      a: During merge process, kernel scans the metadata page
 *      from backwards when merge is initiated. Since, we need
 *      to make sure that the merge ordering follows our COW format,
 *      we read the COW operation from backwards and populate the
 *      metadata so that when kernel starts the merging from backwards,
 *      those ops correspond to the beginning of our COW format.
 *      b: Kernel can merge successive operations if the two chunk IDs
 *      are contiguous. This can be problematic when there is a crash
 *      during merge; specifically when the merge operation has dependency.
 *      These dependencies can only happen during copy operations.
 *
 *      To avoid this problem, we make sure that no two copy-operations
 *      do not have contiguous chunk IDs. Additionally, we make sure
 *      that each copy operation is merged individually.
 * 6: Use a monotonically increasing chunk number to assign the
 *    new_chunk
 * 7: Each chunk-id represents either a: Metadata page or b: Data page
 * 8: Chunk-id representing a data page is stored in a map.
 * 9: Chunk-id representing a metadata page is converted into a vector
 *    index. We store this in vector as kernel requests metadata during
 *    two stage:
 *       a: When initial dm-snapshot device is created, kernel requests
 *          all the metadata and stores it in its internal data-structures.
 *       b: During merge, kernel once again requests the same metadata
 *          once-again.
 *    In both these cases, a quick lookup based on chunk-id is done.
 * 10: When chunk number is incremented, we need to make sure that
 *    if the chunk is representing a metadata page and skip.
 * 11: Each 4k page will contain 256 disk exceptions. We call this
 *    exceptions_per_area_
 * 12: Kernel will stop issuing metadata IO request when new-chunk ID is 0.
 */
bool Snapuserd::ReadMetadata() {
    reader_ = std::make_unique<CowReader>();
    CowHeader header;
    CowOptions options;
    bool prev_copy_op = false;
    bool metadata_found = false;

    LOG(DEBUG) << "ReadMetadata Start...";

    if (!reader_->Parse(cow_fd_)) {
        LOG(ERROR) << "Failed to parse";
        return false;
    }

    if (!reader_->GetHeader(&header)) {
        LOG(ERROR) << "Failed to get header";
        return false;
    }

    CHECK(header.block_size == BLOCK_SIZE);

    LOG(DEBUG) << "Merge-ops: " << header.num_merge_ops;

    writer_ = std::make_unique<CowWriter>(options);
    writer_->InitializeMerge(cow_fd_.get(), &header);

    // Initialize the iterator for reading metadata
    cowop_riter_ = reader_->GetRevOpIter();

    exceptions_per_area_ = (CHUNK_SIZE << SECTOR_SHIFT) / sizeof(struct disk_exception);

    // Start from chunk number 2. Chunk 0 represents header and chunk 1
    // represents first metadata page.
    chunk_t next_free = NUM_SNAPSHOT_HDR_CHUNKS + 1;

    loff_t offset = 0;
    std::unique_ptr<uint8_t[]> de_ptr =
            std::make_unique<uint8_t[]>(exceptions_per_area_ * sizeof(struct disk_exception));

    // This memset is important. Kernel will stop issuing IO when new-chunk ID
    // is 0. When Area is not filled completely with all 256 exceptions,
    // this memset will ensure that metadata read is completed.
    memset(de_ptr.get(), 0, (exceptions_per_area_ * sizeof(struct disk_exception)));
    size_t num_ops = 0;

    while (!cowop_riter_->Done()) {
        const CowOperation* cow_op = &cowop_riter_->Get();
        struct disk_exception* de =
                reinterpret_cast<struct disk_exception*>((char*)de_ptr.get() + offset);

        if (cow_op->type == kCowFooterOp || cow_op->type == kCowLabelOp) {
            cowop_riter_->Next();
            continue;
        }

        if (!(cow_op->type == kCowReplaceOp || cow_op->type == kCowZeroOp ||
              cow_op->type == kCowCopyOp)) {
            LOG(ERROR) << "Unknown operation-type found: " << cow_op->type;
            return false;
        }

        metadata_found = true;
        if ((cow_op->type == kCowCopyOp || prev_copy_op)) {
            next_free = GetNextAllocatableChunkId(next_free);
        }

        prev_copy_op = (cow_op->type == kCowCopyOp);

        // Construct the disk-exception
        de->old_chunk = cow_op->new_block;
        de->new_chunk = next_free;

        LOG(DEBUG) << "Old-chunk: " << de->old_chunk << "New-chunk: " << de->new_chunk;

        // Store operation pointer.
        chunk_map_[next_free] = cow_op;
        num_ops += 1;

        offset += sizeof(struct disk_exception);

        cowop_riter_->Next();

        if (num_ops == exceptions_per_area_) {
            // Store it in vector at the right index. This maps the chunk-id to
            // vector index.
            vec_.push_back(std::move(de_ptr));
            offset = 0;
            num_ops = 0;

            // Create buffer for next area
            de_ptr = std::make_unique<uint8_t[]>(exceptions_per_area_ *
                                                 sizeof(struct disk_exception));
            memset(de_ptr.get(), 0, (exceptions_per_area_ * sizeof(struct disk_exception)));

            if (cowop_riter_->Done()) {
                vec_.push_back(std::move(de_ptr));
                LOG(DEBUG) << "ReadMetadata() completed; Number of Areas: " << vec_.size();
            }
        }

        next_free = GetNextAllocatableChunkId(next_free);
    }

    // Partially filled area or there is no metadata
    // If there is no metadata, fill with zero so that kernel
    // is aware that merge is completed.
    if (num_ops || !metadata_found) {
        vec_.push_back(std::move(de_ptr));
        LOG(DEBUG) << "ReadMetadata() completed. Partially filled area num_ops: " << num_ops
                   << "Areas : " << vec_.size();
    }

    LOG(DEBUG) << "ReadMetadata() completed. chunk_id: " << next_free
               << "Num Sector: " << ChunkToSector(next_free);

    // Initialize the iterator for merging
    cowop_iter_ = reader_->GetOpIter();

    // Total number of sectors required for creating dm-user device
    num_sectors_ = ChunkToSector(next_free);
    metadata_read_done_ = true;
    return true;
}

void MyLogger(android::base::LogId, android::base::LogSeverity severity, const char*, const char*,
              unsigned int, const char* message) {
    if (severity == android::base::ERROR) {
        fprintf(stderr, "%s\n", message);
    } else {
        fprintf(stdout, "%s\n", message);
    }
}

// Read Header from dm-user misc device. This gives
// us the sector number for which IO is issued by dm-snapshot device
bool Snapuserd::ReadDmUserHeader() {
    if (!android::base::ReadFully(ctrl_fd_, bufsink_.GetBufPtr(), sizeof(struct dm_user_header))) {
        PLOG(ERROR) << "ReadDmUserHeader failed";
        return false;
    }

    return true;
}

// Send the payload/data back to dm-user misc device.
bool Snapuserd::WriteDmUserPayload(size_t size) {
    if (!android::base::WriteFully(ctrl_fd_, bufsink_.GetBufPtr(),
                                   sizeof(struct dm_user_header) + size)) {
        PLOG(ERROR) << "Write to dm-user failed";
        return false;
    }

    return true;
}

bool Snapuserd::ReadDmUserPayload(void* buffer, size_t size) {
    if (!android::base::ReadFully(ctrl_fd_, buffer, size)) {
        PLOG(ERROR) << "ReadDmUserPayload failed";
        return false;
    }

    return true;
}

bool Snapuserd::InitCowDevice() {
    cow_fd_.reset(open(cow_device_.c_str(), O_RDWR));
    if (cow_fd_ < 0) {
        PLOG(ERROR) << "Open Failed: " << cow_device_;
        return false;
    }

    // Allocate the buffer which is used to communicate between
    // daemon and dm-user. The buffer comprises of header and a fixed payload.
    // If the dm-user requests a big IO, the IO will be broken into chunks
    // of PAYLOAD_SIZE.
    size_t buf_size = sizeof(struct dm_user_header) + PAYLOAD_SIZE;
    bufsink_.Initialize(buf_size);

    return ReadMetadata();
}

bool Snapuserd::InitBackingAndControlDevice() {
    backing_store_fd_.reset(open(backing_store_device_.c_str(), O_RDONLY));
    if (backing_store_fd_ < 0) {
        PLOG(ERROR) << "Open Failed: " << backing_store_device_;
        return false;
    }

    ctrl_fd_.reset(open(control_device_.c_str(), O_RDWR));
    if (ctrl_fd_ < 0) {
        PLOG(ERROR) << "Unable to open " << control_device_;
        return false;
    }

    return true;
}

bool Snapuserd::Run() {
    struct dm_user_header* header = bufsink_.GetHeaderPtr();

    bufsink_.Clear();

    if (!ReadDmUserHeader()) {
        LOG(ERROR) << "ReadDmUserHeader failed";
        return false;
    }

    LOG(DEBUG) << "msg->seq: " << std::hex << header->seq;
    LOG(DEBUG) << "msg->type: " << std::hex << header->type;
    LOG(DEBUG) << "msg->flags: " << std::hex << header->flags;
    LOG(DEBUG) << "msg->sector: " << std::hex << header->sector;
    LOG(DEBUG) << "msg->len: " << std::hex << header->len;

    switch (header->type) {
        case DM_USER_REQ_MAP_READ: {
            size_t remaining_size = header->len;
            loff_t offset = 0;
            do {
                size_t read_size = std::min(PAYLOAD_SIZE, remaining_size);
                header->type = DM_USER_RESP_SUCCESS;

                // Request to sector 0 is always for kernel
                // representation of COW header. This IO should be only
                // once during dm-snapshot device creation. We should
                // never see multiple IO requests. Additionally this IO
                // will always be a single 4k.
                if (header->sector == 0) {
                    CHECK(metadata_read_done_ == true);
                    CHECK(read_size == BLOCK_SIZE);
                    ConstructKernelCowHeader();
                    LOG(DEBUG) << "Kernel header constructed";
                } else {
                    // Convert the sector number to a chunk ID.
                    //
                    // Check if the chunk ID represents a metadata
                    // page. If the chunk ID is not found in the
                    // vector, then it points to a metadata page.
                    chunk_t chunk = SectorToChunk(header->sector);

                    if (chunk_map_.find(chunk) == chunk_map_.end()) {
                        if (!ReadDiskExceptions(chunk, read_size)) {
                            LOG(ERROR) << "ReadDiskExceptions failed for chunk id: " << chunk
                                       << "Sector: " << header->sector;
                            header->type = DM_USER_RESP_ERROR;
                        } else {
                            LOG(DEBUG) << "ReadDiskExceptions success for chunk id: " << chunk
                                       << "Sector: " << header->sector;
                        }
                    } else {
                        chunk_t num_chunks_read = (offset >> BLOCK_SHIFT);
                        if (!ReadData(chunk + num_chunks_read, read_size)) {
                            LOG(ERROR) << "ReadData failed for chunk id: " << chunk
                                       << "Sector: " << header->sector;
                            header->type = DM_USER_RESP_ERROR;
                        } else {
                            LOG(DEBUG) << "ReadData success for chunk id: " << chunk
                                       << "Sector: " << header->sector;
                        }
                    }
                }

                // Daemon will not be terminated if there is any error. We will
                // just send the error back to dm-user.
                if (!WriteDmUserPayload(read_size)) {
                    return false;
                }

                remaining_size -= read_size;
                offset += read_size;
            } while (remaining_size);

            break;
        }

        case DM_USER_REQ_MAP_WRITE: {
            // device mapper has the capability to allow
            // targets to flush the cache when writes are completed. This
            // is controlled by each target by a flag "flush_supported".
            // This flag is set by dm-user. When flush is supported,
            // a number of zero-length bio's will be submitted to
            // the target for the purpose of flushing cache. It is the
            // responsibility of the target driver - which is dm-user in this
            // case, to remap these bio's to the underlying device. Since,
            // there is no underlying device for dm-user, this zero length
            // bio's gets routed to daemon.
            //
            // Flush operations are generated post merge by dm-snap by having
            // REQ_PREFLUSH flag set. Snapuser daemon doesn't have anything
            // to flush per se; hence, just respond back with a success message.
            if (header->sector == 0) {
                CHECK(header->len == 0);
                header->type = DM_USER_RESP_SUCCESS;
                if (!WriteDmUserPayload(0)) {
                    return false;
                }
                break;
            }

            size_t remaining_size = header->len;
            size_t read_size = std::min(PAYLOAD_SIZE, remaining_size);
            CHECK(read_size == BLOCK_SIZE);

            CHECK(header->sector > 0);
            chunk_t chunk = SectorToChunk(header->sector);
            CHECK(chunk_map_.find(chunk) == chunk_map_.end());

            void* buffer = bufsink_.GetPayloadBuffer(read_size);
            CHECK(buffer != nullptr);
            header->type = DM_USER_RESP_SUCCESS;

            if (!ReadDmUserPayload(buffer, read_size)) {
                LOG(ERROR) << "ReadDmUserPayload failed for chunk id: " << chunk
                           << "Sector: " << header->sector;
                header->type = DM_USER_RESP_ERROR;
            }

            if (header->type == DM_USER_RESP_SUCCESS && !ProcessMergeComplete(chunk, buffer)) {
                LOG(ERROR) << "ProcessMergeComplete failed for chunk id: " << chunk
                           << "Sector: " << header->sector;
                header->type = DM_USER_RESP_ERROR;
            } else {
                LOG(DEBUG) << "ProcessMergeComplete success for chunk id: " << chunk
                           << "Sector: " << header->sector;
            }

            if (!WriteDmUserPayload(0)) {
                return false;
            }

            break;
        }
    }

    return true;
}

}  // namespace snapshot
}  // namespace android

int main([[maybe_unused]] int argc, char** argv) {
    android::base::InitLogging(argv, &android::base::KernelLogger);

    android::snapshot::Daemon& daemon = android::snapshot::Daemon::Instance();

    std::string socket = android::snapshot::kSnapuserdSocket;
    if (argc >= 2) {
        socket = argv[1];
    }
    daemon.StartServer(socket);
    daemon.Run();

    return 0;
}
