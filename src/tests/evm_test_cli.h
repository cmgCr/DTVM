// Copyright (C) 2025 the DTVM authors. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#ifndef ZEN_TESTS_EVM_TEST_CLI_H
#define ZEN_TESTS_EVM_TEST_CLI_H

#include "common/enums.h"
#include "runtime/config.h"
#include "zetaengine.h"
#include <CLI/CLI.hpp>
#include <unordered_map>

namespace zen::test {

// Import types from other namespaces
using zen::common::InputFormat;
using zen::common::RunMode;
using zen::runtime::RuntimeConfig;
using zen::utils::LoggerLevel;

// Common mode mappings for CLI argument parsing
inline const std::unordered_map<std::string, RunMode> ModeMap = {
    {"interpreter", RunMode::InterpMode},
#ifdef ZEN_ENABLE_MULTIPASS_JIT
    {"multipass", RunMode::MultipassMode},
#endif
};

// Common format mappings for CLI argument parsing
inline const std::unordered_map<std::string, InputFormat> FormatMap = {
    {"wasm", InputFormat::WASM},
    {"evm", InputFormat::EVM},
};

// Common EVM revision mappings for CLI argument parsing
inline const std::unordered_map<std::string, evmc_revision> RevisionMap = {
    {"ALL", EVMC_MAX_REVISION},
    {"Frontier", EVMC_FRONTIER},
    {"Homestead", EVMC_HOMESTEAD},
    {"TangerineWhistle", EVMC_TANGERINE_WHISTLE},
    {"SpuriousDragon", EVMC_SPURIOUS_DRAGON},
    {"Byzantium", EVMC_BYZANTIUM},
    {"Constantinople", EVMC_CONSTANTINOPLE},
    {"Petersburg", EVMC_PETERSBURG},
    {"Istanbul", EVMC_ISTANBUL},
    {"Berlin", EVMC_BERLIN},
    {"London", EVMC_LONDON},
    {"Paris", EVMC_PARIS},
    {"Shanghai", EVMC_SHANGHAI},
    {"Cancun", EVMC_CANCUN},
    {"Prague", EVMC_PRAGUE},
};

// Common log level mappings for CLI argument parsing
inline const std::unordered_map<std::string, LoggerLevel> LogMap = {
    {"trace", LoggerLevel::Trace}, {"debug", LoggerLevel::Debug},
    {"info", LoggerLevel::Info},   {"warn", LoggerLevel::Warn},
    {"error", LoggerLevel::Error}, {"fatal", LoggerLevel::Fatal},
    {"off", LoggerLevel::Off},
};

/**
 * @brief Add common EVM CLI options to a CLI::App parser
 *
 * This function adds the following common options:
 * - --mode / -m: Running mode (interpreter, multipass)
 * - --format: Input format (wasm, evm)
 * - --log-level: Log level
 * - --gas-limit: Gas limit
 * - --revision / -r: EVM revision
 * - --category / -c: Test category/directory
 * - --enable-evm-gas: Enable EVM gas metering
 * - --disable-multipass-greedyra: Disable greedy RA for multipass JIT
 * - --disable-multipass-multithread: Disable multithread for multipass JIT
 * - --num-multipass-threads: Number of threads for multipass JIT
 * - --enable-multipass-lazy: Enable lazy compilation for multipass JIT
 *
 * @param App CLI application to add options to
 * @param Config Runtime configuration to modify
 * @param LogLevel Log level to set
 * @param GasLimit Gas limit to set
 * @param Revision EVM revision to set
 * @param TestCategory Test category/directory path to set
 */
inline void addCommonEVMOptions(CLI::App &App, RuntimeConfig &Config,
                                LoggerLevel &LogLevel, uint64_t &GasLimit,
                                evmc_revision &Revision,
                                std::string &TestCategory) {
  App.add_option("--format", Config.Format, "Input format (wasm, evm)")
      ->transform(CLI::CheckedTransformer(FormatMap, CLI::ignore_case));
  App.add_option("-m,--mode", Config.Mode,
                 "Running mode (interpreter, multipass)")
      ->transform(CLI::CheckedTransformer(ModeMap, CLI::ignore_case));
  App.add_option("-r,--revision", Revision, "EVM revision to test")
      ->transform(CLI::CheckedTransformer(RevisionMap, CLI::ignore_case));
  App.add_option("--gas-limit", GasLimit, "Gas limit");
  App.add_option("--log-level", LogLevel, "Log level")
      ->transform(CLI::CheckedTransformer(LogMap, CLI::ignore_case));
  App.add_option("-c,--category", TestCategory, "Test category/directory path");

#ifdef ZEN_ENABLE_EVM
  App.add_flag("--enable-evm-gas", Config.EnableEvmGasMetering,
               "Enable EVM gas metering when compiling EVM bytecode");
#endif // ZEN_ENABLE_EVM

#ifdef ZEN_ENABLE_MULTIPASS_JIT
  App.add_flag("--disable-multipass-greedyra", Config.DisableMultipassGreedyRA,
               "Disable greedy register allocation of multipass JIT");
  auto *DMMOption = App.add_flag(
      "--disable-multipass-multithread", Config.DisableMultipassMultithread,
      "Disable multithread compilation of multipass JIT");
  App.add_option("--num-multipass-threads", Config.NumMultipassThreads,
                 "Number of threads for multipass JIT (set 0 for automatic "
                 "determination)")
      ->excludes(DMMOption);
  App.add_flag("--enable-multipass-lazy", Config.EnableMultipassLazy,
               "Enable multipass lazy mode (on request compile)");
#endif // ZEN_ENABLE_MULTIPASS_JIT
}

} // namespace zen::test

#endif // ZEN_TESTS_EVM_TEST_CLI_H
