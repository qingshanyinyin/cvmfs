/**
 * This file is part of the CernVM File System.
 */

#include "catalog_merge_tool.h"

#include "catalog.h"
#include "hash.h"
#include "logging.h"
#include "options.h"
#include "upload.h"
#include "util/posix.h"

namespace {

struct Params {
  std::string spooler_configuration;
  shash::Algorithms hash_alg;
  zlib::Algorithms compression_alg;
  bool use_file_chunking;
  size_t min_chunk_size;
  size_t avg_chunk_size;
  size_t max_chunk_size;
  size_t entry_warn_thresh;
  bool use_autocatalogs;
  size_t max_weight;
  size_t min_weight;
};

bool GetParamsFromFile(const std::string& repo_name, Params* params) {
  std::vector<std::string> tokens = SplitString(repo_name, '/');

  const std::string repo_config_file =
      "/etc/cvmfs/repositories.d/" + tokens.back() + "/server.conf";

  SimpleOptionsParser parser;
  if (!parser.TryParsePath(repo_config_file)) {
    return false;
  }

  bool ret = true;
  ret &=
      parser.GetValue("CVMFS_UPSTREAM_STORAGE", &params->spooler_configuration);

  // Note: if upstream is gateway, we change it to local. This should be made to
  // abort, but it's useful for testing on a single machine
  if (HasPrefix(params->spooler_configuration, "gw", false)) {
    std::vector<std::string> tokens = SplitString(repo_name, '/');
    const std::string rname = tokens.back();
    params->spooler_configuration =
        "local,/srv/cvmfs/" + rname + "/data/txn,/srv/cvmfs/" + rname;
  }

  std::string hash_algorithm_str;
  ret &= parser.GetValue("CVMFS_HASH_ALGORITHM", &hash_algorithm_str);
  params->hash_alg = shash::ParseHashAlgorithm(hash_algorithm_str);

  std::string compression_algorithm_str;
  ret &= parser.GetValue("CVMFS_COMPRESSION_ALGORITHM",
                         &compression_algorithm_str);
  params->compression_alg =
      zlib::ParseCompressionAlgorithm(compression_algorithm_str);

  std::string use_chunking_str;
  ret &= parser.GetValue("CVMFS_USE_FILE_CHUNKING", &use_chunking_str);
  if (use_chunking_str == "true") {
    params->use_file_chunking = true;
  } else if (use_chunking_str == "false") {
    params->use_file_chunking = false;
  } else {
    return false;
  }

  std::string min_chunk_size_str;
  ret &= parser.GetValue("CVMFS_MIN_CHUNK_SIZE", &min_chunk_size_str);
  params->min_chunk_size = String2Uint64(min_chunk_size_str);

  std::string avg_chunk_size_str;
  ret &= parser.GetValue("CVMFS_AVG_CHUNK_SIZE", &avg_chunk_size_str);
  params->avg_chunk_size = String2Uint64(avg_chunk_size_str);

  std::string max_chunk_size_str;
  ret &= parser.GetValue("CVMFS_MAX_CHUNK_SIZE", &max_chunk_size_str);
  params->max_chunk_size = String2Uint64(max_chunk_size_str);

  std::string use_autocatalogs_str;
  ret &= parser.GetValue("CVMFS_AUTOCATALOGS", &use_autocatalogs_str);
  if (use_autocatalogs_str == "true") {
    params->use_autocatalogs = true;
  } else if (use_autocatalogs_str == "false") {
    params->use_autocatalogs = false;
  } else {
    return false;
  }

  std::string max_weight_str;
  if (parser.GetValue("CVMFS_AUTOCATALOGS_MAX_WEIGHT", &max_weight_str)) {
    params->max_weight = String2Uint64(max_weight_str);
  }

  std::string min_weight_str;
  if (parser.GetValue("CVMFS_AUTOCATALOGS_MIN_WEIGHT", &min_weight_str)) {
    params->min_weight = String2Uint64(min_weight_str);
  }

  return ret;
}

}  // namespace

namespace receiver {

CatalogMergeTool::ChangeItem::ChangeItem(ChangeType type,
                                         const PathString& path,
                                         const catalog::DirectoryEntry& entry1)
    : type_(type),
      path_(path),
      entry1_(new catalog::DirectoryEntry(entry1)),
      entry2_(NULL) {}

CatalogMergeTool::ChangeItem::ChangeItem(ChangeType type,
                                         const PathString& path,
                                         const catalog::DirectoryEntry& entry1,
                                         const catalog::DirectoryEntry& entry2)
    : type_(type),
      path_(path),
      entry1_(new catalog::DirectoryEntry(entry1)),
      entry2_(new catalog::DirectoryEntry(entry2)) {}

CatalogMergeTool::ChangeItem::ChangeItem(const ChangeItem& other)
    : type_(other.type_),
      path_(other.path_),
      entry1_(new catalog::DirectoryEntry(*(other.entry1_))),
      entry2_(other.entry2_ ? new catalog::DirectoryEntry(*(other.entry2_))
                            : NULL) {}

CatalogMergeTool::ChangeItem::~ChangeItem() {
  if (entry1_) {
    delete entry1_;
    entry1_ = NULL;
  }
  if (entry2_) {
    delete entry2_;
    entry2_ = NULL;
  }
}

CatalogMergeTool::CatalogMergeTool(const std::string& repo_path,
                                   const std::string& old_root_hash,
                                   const std::string& new_root_hash,
                                   const std::string& base_root_hash,
                                   const std::string& temp_dir_prefix,
                                   download::DownloadManager* download_manager)
    : CatalogDiffTool(repo_path, old_root_hash, new_root_hash, temp_dir_prefix,
                      download_manager),
      repo_path_(repo_path),
      temp_dir_prefix_(temp_dir_prefix),
      base_root_hash_(
          shash::MkFromSuffixedHexPtr(shash::HexPtr(base_root_hash))),
      download_manager_(download_manager) {}

CatalogMergeTool::~CatalogMergeTool() {}

bool CatalogMergeTool::Run(shash::Any* /*resulting_root_hash*/) {
  bool ret = CatalogDiffTool::Run();

  FILE* debug_file = std::fopen("/home/radu/debug.log", "w");
  for (size_t i = 0; i < changes_.size(); ++i) {
    const ChangeItem& change = changes_[i];
    fprintf(debug_file, "Change - type: %d, path: %s, entry1.name: %s",
            change.type_, change.path_.c_str(), change.entry1_->name().c_str());
    if (change.entry2_) {
      fprintf(debug_file, ", entry2.name: %s\n",
              change.entry2_->name().c_str());
    } else {
      fprintf(debug_file, "\n");
    }
  }

  Params params;
  if (!GetParamsFromFile(repo_path_, &params)) {
    return false;
  }

  upload::SpoolerDefinition definition(
      params.spooler_configuration, params.hash_alg, params.compression_alg,
      params.use_file_chunking, params.min_chunk_size, params.avg_chunk_size,
      params.max_chunk_size, "dummy_token", "dummy_key");
  UniquePtr<upload::Spooler> spooler(upload::Spooler::Construct(definition));
  perf::Statistics stats;
  const std::string temp_dir = CreateTempDir(temp_dir_prefix_);
  output_catalog_mgr_ = new catalog::WritableCatalogManager(
      base_root_hash_, repo_path_, temp_dir_prefix_, spooler, download_manager_,
      params.entry_warn_thresh, &stats, params.use_autocatalogs,
      params.max_weight, params.min_weight);

  std::fclose(debug_file);
  return ret;
}

void CatalogMergeTool::ReportAddition(const PathString& path,
                                      const catalog::DirectoryEntry& entry) {
  changes_.push_back(ChangeItem(ChangeItem::kAddition, path, entry));
}

void CatalogMergeTool::ReportRemoval(const PathString& path,
                                     const catalog::DirectoryEntry& entry) {
  changes_.push_back(ChangeItem(ChangeItem::kRemoval, path, entry));
}

void CatalogMergeTool::ReportModification(
    const PathString& path, const catalog::DirectoryEntry& entry1,
    const catalog::DirectoryEntry& entry2) {
  changes_.push_back(
      ChangeItem(ChangeItem::kModification, path, entry1, entry2));
}

}  // namespace receiver
