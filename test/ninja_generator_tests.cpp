#include "ninja_generator.hpp"
#include <gtest/gtest.h>

TEST(NinjaGenerator, EscapesPathsForBuildLines)
{
  EXPECT_EQ(yakka::ninja_escape_path("output/app/main.o"), "output/app/main.o");
  EXPECT_EQ(yakka::ninja_escape_path("C:/a b/$x.o"), "C$:/a$ b/$$x.o");
}

TEST(NinjaGenerator, EscapesOnlyDollarsInValues)
{
  EXPECT_EQ(yakka::ninja_escape_value("clang++ -c a.c -o a:b.o"), "clang++ -c a.c -o a:b.o");
  EXPECT_EQ(yakka::ninja_escape_value("echo $HOME"), "echo $$HOME");
}

TEST(NinjaGenerator, QuotesShellArgumentsOnlyWhenNeeded)
{
  EXPECT_EQ(yakka::shell_quote("link!"), "'link!'");
  EXPECT_EQ(yakka::shell_quote("output/app/main.o"), "output/app/main.o");
  EXPECT_EQ(yakka::shell_quote(""), "''");
  EXPECT_EQ(yakka::shell_quote("a b"), "'a b'");
  EXPECT_EQ(yakka::shell_quote("it's"), "'it'\\''s'");
}
