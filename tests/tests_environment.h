/**
 * @file tests/tests_environment.h
 * @brief Declarations for SunshineEnvironment.
 */
#pragma once

// standard includes
#include <filesystem>

// test includes
#include "tests_common.h"

// local includes
#include "src/config.h"

struct SunshineEnvironment: testing::Environment {
  void SetUp() override {
    mail::man = std::make_shared<safe::mail_raw_t>();
    deinit_log = logging::init(0, "test_sunshine.log");

    // Paired-client state and credentials default to paths below the user's Sunshine
    // configuration directory once a test applies configuration. Point them into the build tree
    // so no test can overwrite the host's real pairing or server certificate.
    const auto state_directory = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "test_state";
    std::filesystem::create_directories(state_directory);
    config::nvhttp.file_state = (state_directory / "sunshine_state.json").string();
    config::sunshine.credentials_file = config::nvhttp.file_state;
    config::nvhttp.cert = (state_directory / "cacert.pem").string();
    config::nvhttp.pkey = (state_directory / "cakey.pem").string();
  }

  void TearDown() override {
    deinit_log = {};
    mail::man = {};
  }

  std::unique_ptr<logging::deinit_t> deinit_log;
};
