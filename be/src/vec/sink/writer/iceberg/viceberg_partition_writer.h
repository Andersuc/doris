// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <gen_cpp/DataSinks_types.h>

#include "io/fs/file_writer.h"
#include "vec/columns/column.h"
#include "vec/exec/format/table/iceberg/schema.h"
#include "vec/exprs/vexpr_fwd.h"
#include "vec/runtime/vfile_format_transformer.h"

namespace doris {
namespace io {
class FileSystem;
}

class ObjectPool;
class RuntimeState;
class RuntimeProfile;

namespace iceberg {
class Schema;
}

namespace vectorized {

class Block;
class VFileFormatTransformer;

class VIcebergPartitionWriter {
public:
    struct WriteInfo {
        std::string write_path;
        std::string original_write_path;
        std::string target_path;
        TFileType::type file_type;
        std::vector<TNetworkAddress> broker_addresses;
    };

    VIcebergPartitionWriter(const TDataSink& t_sink, std::vector<std::string> partition_values,
                            const VExprContextSPtrs& write_output_expr_ctxs,
                            const doris::iceberg::Schema& schema,
                            const std::string* iceberg_schema_json,
                            std::vector<std::string> write_column_names, WriteInfo write_info,
                            std::string file_name, int file_name_index,
                            TFileFormatType::type file_format_type,
                            TFileCompressType::type compress_type,
                            const std::map<std::string, std::string>& hadoop_conf);

    Status init_properties(ObjectPool* pool) { return Status::OK(); }

    Status open(RuntimeState* state, RuntimeProfile* profile);

    Status write(vectorized::Block& block);

    Status close(const Status& status);

    inline const std::string& file_name() const { return _file_name; }

    inline int file_name_index() const { return _file_name_index; }

    inline size_t written_len() { return _file_format_transformer->written_len(); }

private:
    std::string _get_target_file_name();

    TIcebergCommitData _build_iceberg_commit_data();

    std::string _get_file_extension(TFileFormatType::type file_format_type,
                                    TFileCompressType::type write_compress_type);

    std::string _path;

    std::vector<std::string> _partition_values;

    size_t _row_count = 0;

    const VExprContextSPtrs& _write_output_expr_ctxs;

    const doris::iceberg::Schema& _schema;
    const std::string* _iceberg_schema_json;
    std::vector<std::string> _write_column_names;
    WriteInfo _write_info;
    std::string _file_name;
    int _file_name_index;
    TFileFormatType::type _file_format_type;
    TFileCompressType::type _compress_type;
    const std::map<std::string, std::string>& _hadoop_conf;

    std::shared_ptr<io::FileSystem> _fs = nullptr;

    // If the result file format is plain text, like CSV, this _file_writer is owned by this FileResultWriter.
    // If the result file format is Parquet, this _file_writer is owned by _parquet_writer.
    std::unique_ptr<doris::io::FileWriter> _file_writer = nullptr;
    // convert block to parquet/orc/csv format
    std::unique_ptr<VFileFormatTransformer> _file_format_transformer = nullptr;

    RuntimeState* _state;
};
} // namespace vectorized
} // namespace doris
