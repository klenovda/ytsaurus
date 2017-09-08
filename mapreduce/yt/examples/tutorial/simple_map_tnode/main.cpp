#include <mapreduce/yt/interface/client.h>
#include <util/stream/output.h>
#include <util/system/user.h>

using namespace NYT;

class TComputeEmailsMapper
    : public IMapper<TTableReader<TNode>, TTableWriter<TNode>> // Указываем, что мы хотим использовать TNode
{
public:
    virtual void Do(TReader* reader, TWriter* writer) override
    {
        for (; reader->IsValid(); reader->Next()) {
            const auto& row = reader->GetRow();

            TNode outRow;
            outRow["name"] = row["name"];
            outRow["email"] = row["login"].AsString() + "@yandex-team.ru";

            writer->AddRow(outRow);
        }
    }
};
REGISTER_MAPPER(TComputeEmailsMapper); // Подобное заклинание нужно говорить для каждого mapper'а / reducer'а.

int main(int argc, const char** argv) {
    Initialize(argc, argv); // Инициализируем библиотеку, важно не забывать это делать,
                            // иначе некоторые вещи, например запуск операций не будут работать.

    auto client = CreateClient("freud");

    // Выходная табличка у нас будет лежать в tmp и содержать имя текущего пользователя.
    const TString outputTable = "//tmp/" + GetUsername() + "-tutorial-emails";

    client->Map(
        TMapOperationSpec()
            .AddInput<TNode>("//home/ermolovd/yt-tutorial/staff_unsorted")
            .AddOutput<TNode>(outputTable),
        new TComputeEmailsMapper);

    Cout << "Output table: https://yt.yandex-team.ru/freud/#page=navigation&offsetMode=row&path=" << outputTable << Endl;

    return 0;
}
