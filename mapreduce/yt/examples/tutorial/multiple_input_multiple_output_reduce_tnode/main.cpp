#include <mapreduce/yt/interface/client.h>

#include <util/stream/output.h>
#include <util/system/user.h>

using namespace NYT;

class TSplitHumanRobotsReduce
    : public IReducer<TTableReader<TNode>, TTableWriter<TNode>>
{
public:
    void Do(TReader* reader, TWriter* writer) override {
        TNode loginRow;
        bool isRobot = false;
        for (; reader->IsValid(); reader->Next()) {
            const auto& curRow = reader->GetRow();

            // У нас есть информация о том из какой таблицы пришла запись.
            auto tableIndex = reader->GetTableIndex();
            if (tableIndex == 0) {
                // Таблица с логинами.
                loginRow = curRow;
            } else if (tableIndex == 1) {
                isRobot = curRow["is_robot"].AsBool();
            } else {
                // Какая-то фигня, такого индекса быть не может.
                Y_FAIL();
            }
        }

        // Второй аргумент метода `AddRow' указывает, в какую таблицу писать.
        if (isRobot) {
            writer->AddRow(loginRow, 0);
        } else {
            writer->AddRow(loginRow, 1);
        }
    }
};
REGISTER_REDUCER(TSplitHumanRobotsReduce)

int main(int argc, const char** argv) {
    NYT::Initialize(argc, argv);

    auto client = CreateClient("freud");

    const TString sortedLoginTable = "//tmp/" + GetUsername() + "-tutorial-login-sorted";
    const TString sortedIsRobotTable = "//tmp/" + GetUsername() + "-tutorial-is_robot-sorted";
    const TString humanTable = "//tmp/" + GetUsername() + "-tutorial-humans";
    const TString robotTable = "//tmp/" + GetUsername() + "-tutorial-robots";

    client->Sort(
        TSortOperationSpec()
            .AddInput("//home/ermolovd/yt-tutorial/staff_unsorted")
            .Output(sortedLoginTable)
            .SortBy({"uid"}));

    client->Sort(
        TSortOperationSpec()
            .AddInput("//home/ermolovd/yt-tutorial/is_robot_unsorted")
            .Output(sortedIsRobotTable)
            .SortBy({"uid"}));

    client->Reduce(
        TReduceOperationSpec()
            .ReduceBy({"uid"})
            .AddInput<TNode>(sortedLoginTable)
            .AddInput<TNode>(sortedIsRobotTable)
            .AddOutput<TNode>(robotTable)  // выходная таблица номер 0
            .AddOutput<TNode>(humanTable), // выходная таблица номер 1
        new TSplitHumanRobotsReduce);

    Cout << "Robot table: https://yt.yandex-team.ru/freud/#page=navigation&offsetMode=row&path=" << robotTable << Endl;
    Cout << "Human table: https://yt.yandex-team.ru/freud/#page=navigation&offsetMode=row&path=" << humanTable << Endl;

    return 0;
}
