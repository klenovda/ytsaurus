﻿#include "stdafx.h"

#include "partition_sort_job.h"

#include "job_detail.h"
#include "config.h"
#include "private.h"

#include <ytlib/object_client/helpers.h>

#include <ytlib/table_client/name_table.h>
#include <ytlib/table_client/schemaless_chunk_writer.h>
#include <ytlib/table_client/schemaless_partition_sort_reader.h>

namespace NYT {
namespace NJobProxy {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NScheduler::NProto;
using namespace NTransactionClient;
using namespace NTableClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = JobProxyLogger;

////////////////////////////////////////////////////////////////////////////////

class TPartitionSortJob
    : public TSimpleJobBase
{
public:
    explicit TPartitionSortJob(IJobHost* host)
        : TSimpleJobBase(host)
        , SortJobSpecExt_(JobSpec_.GetExtension(TSortJobSpecExt::sort_job_spec_ext))
    {
        auto config = host->GetConfig();

        auto keyColumns = FromProto<Stroka>(SortJobSpecExt_.key_columns());
        auto nameTable = TNameTable::FromKeyColumns(keyColumns);

        TotalRowCount_ = SchedulerJobSpecExt_.input_row_count();

        YCHECK(SchedulerJobSpecExt_.input_specs_size() == 1);
        const auto& inputSpec = SchedulerJobSpecExt_.input_specs(0);
        std::vector<TChunkSpec> chunkSpecs(inputSpec.chunks().begin(), inputSpec.chunks().end());

        Reader_ = CreateSchemalessPartitionSortReader(
            config->JobIO->TableReader,
            host->GetClient(),
            host->GetBlockCache(),
            host->GetInputNodeDirectory(),
            keyColumns,
            nameTable,
            BIND(&IJobHost::ReleaseNetwork, host),
            chunkSpecs,
            TotalRowCount_,
            SchedulerJobSpecExt_.is_approximate());

        YCHECK(SchedulerJobSpecExt_.output_specs_size() == 1);
        const auto& outputSpec = SchedulerJobSpecExt_.output_specs(0);
        auto transactionId = FromProto<TTransactionId>(SchedulerJobSpecExt_.output_transaction_id());
        auto chunkListId = FromProto<TChunkListId>(outputSpec.chunk_list_id());
        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(outputSpec.table_writer_options()));

        Writer_ = CreateSchemalessMultiChunkWriter(
            config->JobIO->TableWriter,
            options,
            nameTable,
            keyColumns,
            TOwningKey(),
            host->GetClient(),
            CellTagFromId(chunkListId),
            transactionId,
            chunkListId);
    }

    virtual double GetProgress() const override
    {
        auto total = TotalRowCount_;
        if (total == 0) {
            LOG_WARNING("GetProgress: empty total");
            return 0;
        } else {
            // Split progress evenly between reading and writing.
            double progress =
                0.5 * Reader_->GetDataStatistics().row_count() / total +
                0.5 * Writer_->GetDataStatistics().row_count() / total;
            LOG_DEBUG("GetProgress: %lf", progress);
            return progress;
        }
    }

private:
    const TSortJobSpecExt& SortJobSpecExt_;

    virtual void CreateReader() override
    { }

    virtual void CreateWriter() override
    { }
};

IJobPtr CreatePartitionSortJob(IJobHost* host)
{
    return New<TPartitionSortJob>(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
