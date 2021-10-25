#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/library/error_skeleton/skeleton.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TEST(TSkeletonTest, TestSimple)
{
    auto error =
        TError(1, "foo")
            << TError(2, "bar")
            << TError(3, "baz")
            << TError(2, "bar")
            << (TError(4, "qux")
                << TError(5, "quux"))
            << TError(3, "baz");

    TString expectedSkeleton = "#1: foo @ [#2: bar; #3: baz; #4: qux @ [#5: quux]]";
    EXPECT_EQ(
        expectedSkeleton,
        GetErrorSkeleton(error));
    EXPECT_EQ(
        expectedSkeleton,
        error.GetSkeleton());
}

TEST(TSkeletonTest, TestReplacement)
{
    auto error = TError(42, "foo; bar 123-abc-987654-fed //home some-node.yp-c.yandex.net:1234 0-0-0-0");

    TString expectedSkeleton = "#42: foo bar <guid> <path> <address> <guid>";
    EXPECT_EQ(
        expectedSkeleton,
        GetErrorSkeleton(error));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

