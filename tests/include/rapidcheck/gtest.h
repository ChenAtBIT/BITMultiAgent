#pragma once

#include <gtest/gtest.h>
#include <rapidcheck.h>

// Minimal compatibility wrapper for installations that provide RapidCheck
// core headers but not the optional GTest integration header.
#define RC_GTEST_PROP(TestSuite, TestName, Args)                               \
    static void rc_gtest_prop_##TestSuite##_##TestName Args;                   \
    TEST(TestSuite, TestName) {                                                \
        EXPECT_TRUE(::rc::check(#TestSuite "." #TestName,                      \
                                rc_gtest_prop_##TestSuite##_##TestName));      \
    }                                                                          \
    static void rc_gtest_prop_##TestSuite##_##TestName Args

// Fixture-aware compatibility wrapper matching the zero-argument usage pattern
// in this repository's property tests.
#define RC_GTEST_FIXTURE_PROP(Fixture, TestName, Args)                         \
    class rc_gtest_fixture_prop_##Fixture##_##TestName : public Fixture {      \
    public:                                                                    \
        void rapidcheck_property Args;                                         \
    };                                                                         \
    TEST_F(rc_gtest_fixture_prop_##Fixture##_##TestName, TestName) {           \
        EXPECT_TRUE(::rc::check(#Fixture "." #TestName, [this] Args {          \
            /* Reset the fixture for each generated sample to avoid           \
             * cross-iteration state leakage inside a single RapidCheck run. */\
            this->TearDown();                                                  \
            this->SetUp();                                                     \
            this->rapidcheck_property();                                       \
        }));                                                                   \
    }                                                                          \
    void rc_gtest_fixture_prop_##Fixture##_##TestName::rapidcheck_property Args
