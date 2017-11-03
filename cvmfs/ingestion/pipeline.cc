/**
 * This file is part of the CernVM File System.
 */

#include "cvmfs_config.h"
#include "pipeline.h"

#include "ingestion/task_chunk.h"
#include "ingestion/task_compress.h"
#include "ingestion/task_hash.h"
#include "ingestion/task_read.h"
#include "ingestion/task_register.h"
#include "ingestion/task_write.h"
#include "upload_facility.h"
#include "upload_spooler_definition.h"


IngestionPipeline::IngestionPipeline(
  upload::AbstractUploader *uploader,
  const upload::SpoolerDefinition &spooler_definition)
  : compression_algorithm_(spooler_definition.compression_alg)
  , hash_algorithm_(spooler_definition.hash_algorithm)
  , generate_legacy_bulk_chunks_(spooler_definition.generate_legacy_bulk_chunks)
  , chunking_enabled_(spooler_definition.use_file_chunking)
  , minimal_chunk_size_(spooler_definition.min_file_chunk_size)
  , average_chunk_size_(spooler_definition.avg_file_chunk_size)
  , maximal_chunk_size_(spooler_definition.max_file_chunk_size)
  , spawned_(false)
  , uploader_(uploader)
{
  // TODO(jakob): depends on number of cores
  unsigned nfork_base = 1;

  for (unsigned i = 0; i < nfork_base * kNforkRegister; ++i) {
    Tube<FileItem> *tube = new Tube<FileItem>();
    tubes_register_.TakeTube(tube);
    TaskRegister *task = new TaskRegister(tube, &tube_counter_);
    task->RegisterListener(&IngestionPipeline::OnFileProcessed, this);
    tasks_register_.TakeConsumer(task);
  }
  tubes_register_.Activate();

  for (unsigned i = 0; i < nfork_base * kNforkWrite; ++i) {
    Tube<BlockItem> *t = new Tube<BlockItem>();
    tubes_write_.TakeTube(t);
    tasks_write_.TakeConsumer(new TaskWrite(t, &tubes_register_, uploader_));
  }
  tubes_write_.Activate();

  for (unsigned i = 0; i < nfork_base * kNforkHash; ++i) {
    Tube<BlockItem> *t = new Tube<BlockItem>();
    tubes_hash_.TakeTube(t);
    tasks_hash_.TakeConsumer(new TaskHash(t, &tubes_write_));
  }
  tubes_hash_.Activate();

  for (unsigned i = 0; i < nfork_base * kNforkCompress; ++i) {
    Tube<BlockItem> *t = new Tube<BlockItem>();
    tubes_compress_.TakeTube(t);
    tasks_compress_.TakeConsumer(new TaskCompress(t, &tubes_hash_));
  }
  tubes_compress_.Activate();

  for (unsigned i = 0; i < nfork_base * kNforkChunk; ++i) {
    Tube<BlockItem> *t = new Tube<BlockItem>();
    tubes_chunk_.TakeTube(t);
    tasks_chunk_.TakeConsumer(new TaskChunk(t, &tubes_compress_));
  }
  tubes_chunk_.Activate();

  for (unsigned i = 0; i < nfork_base * kNforkRead; ++i) {
    tasks_read_.TakeConsumer(new TaskRead(&tube_input_, &tubes_chunk_));
  }
}


IngestionPipeline::~IngestionPipeline() {
  if (spawned_) {
    tasks_read_.Terminate();
    tasks_chunk_.Terminate();
    tasks_compress_.Terminate();
    tasks_hash_.Terminate();
    tasks_write_.Terminate();
    tasks_register_.Terminate();
  }
}


void IngestionPipeline::OnFileProcessed(
  const upload::SpoolerResult &spooler_result)
{
  NotifyListeners(spooler_result);
}


void IngestionPipeline::Process(
  const std::string &path,
  bool allow_chunking,
  shash::Suffix hash_suffix)
{
  FileItem *file_item = new FileItem(
    path,
    minimal_chunk_size_,
    average_chunk_size_,
    maximal_chunk_size_,
    compression_algorithm_,
    hash_algorithm_,
    hash_suffix,
    allow_chunking && chunking_enabled_,
    generate_legacy_bulk_chunks_);
  tube_counter_.Enqueue(file_item);
  tube_input_.Enqueue(file_item);
}


void IngestionPipeline::Spawn() {
  tasks_register_.Spawn();
  tasks_write_.Spawn();
  tasks_hash_.Spawn();
  tasks_compress_.Spawn();
  tasks_chunk_.Spawn();
  tasks_read_.Spawn();
  spawned_ = true;
}


void IngestionPipeline::WaitFor() {
  tube_counter_.Wait();
}
