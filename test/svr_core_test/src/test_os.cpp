#include <catch2/catch.hpp>

#include <svr/os.hpp>

struct mmap_data
{
    int a;
    int b;
    int c;
    int d;
};

TEST_CASE("mmap")
{
    using namespace svr;

    auto source = mmap_data { 10, 20, 30, 40 };
    mmap_data dest;

    auto mmap = os_create_mmap("mmap", sizeof(mmap_data));
    REQUIRE(mmap);

    os_write_mmap(mmap, &source, sizeof(mmap_data));

    SECTION("same")
    {
        os_read_mmap(mmap, &dest, sizeof(mmap_data));
    }

    SECTION("open")
    {
        auto mmap2 = os_open_mmap("mmap2", os_get_mmap_handle(mmap), sizeof(mmap_data));
        REQUIRE(mmap2);
        os_read_mmap(mmap2, &dest, sizeof(mmap_data));
    };

    REQUIRE(memcmp(&source, &dest, sizeof(mmap_data)) == 0);

    os_destroy_mmap(mmap);
}
