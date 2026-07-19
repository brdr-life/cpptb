#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "aes_regs.hpp"
#include "cpptb/cpptb.hpp"
#include "cpptb_vc/cpptb_vc.hpp"
#include "dut.hpp"

namespace cpptb::benchmarks::secworks_aes {
namespace {

using cpptb::Dut;
using coro::Delay;
using coro::Task;
using namespace coro;
using namespace cpptb::vc;

using Words4 = std::array<uint32_t, 4>;
using Words8 = std::array<uint32_t, 8>;

enum class AccessMode {
    RegModel,
    Master,
    Fused,
};

constexpr uint32_t kControlAddress = 0x20;
constexpr uint32_t kConfigAddress = 0x28;
constexpr uint32_t kKeyAddress = 0x40;
constexpr uint32_t kBlockAddress = 0x80;
constexpr uint32_t kResultAddress = 0xc0;

constexpr Words8 kAes128Key1{
    0x2b7e'1516u, 0x28ae'd2a6u, 0xabf7'1588u, 0x09cf'4f3cu,
    0, 0, 0, 0,
};
constexpr Words8 kAes128Key2{
    0x0001'0203u, 0x0405'0607u, 0x0809'0a0bu, 0x0c0d'0e0fu,
    0, 0, 0, 0,
};
constexpr Words8 kAes256Key1{
    0x603d'eb10u, 0x15ca'71beu, 0x2b73'aef0u, 0x857d'7781u,
    0x1f35'2c07u, 0x3b61'08d7u, 0x2d98'10a3u, 0x0914'dff4u,
};
constexpr Words8 kAes256Key2{
    0x0001'0203u, 0x0405'0607u, 0x0809'0a0bu, 0x0c0d'0e0fu,
    0x1011'1213u, 0x1415'1617u, 0x1819'1a1bu, 0x1c1d'1e1fu,
};

constexpr std::array<Words4, 5> kPlaintext{{
    {0x6bc1'bee2u, 0x2e40'9f96u, 0xe93d'7e11u, 0x7393'172au},
    {0xae2d'8a57u, 0x1e03'ac9cu, 0x9eb7'6facu, 0x45af'8e51u},
    {0x30c8'1c46u, 0xa35c'e411u, 0xe5fb'c119u, 0x1a0a'52efu},
    {0xf69f'2445u, 0xdf4f'9b17u, 0xad2b'417bu, 0xe66c'3710u},
    {0x0011'2233u, 0x4455'6677u, 0x8899'aabbu, 0xccdd'eeffu},
}};

constexpr std::array<Words4, 5> kAes128Ciphertext{{
    {0x3ad7'7bb4u, 0x0d7a'3660u, 0xa89e'caf3u, 0x2466'ef97u},
    {0xf5d3'd585u, 0x03b9'699du, 0xe785'895au, 0x96fd'baafu},
    {0x43b1'cd7fu, 0x598e'ce23u, 0x881b'00e3u, 0xed03'0688u},
    {0x7b0c'785eu, 0x27e8'ad3fu, 0x8223'2071u, 0x0472'5dd4u},
    {0x69c4'e0d8u, 0x6a7b'0430u, 0xd8cd'b780u, 0x70b4'c55au},
}};

constexpr std::array<Words4, 5> kAes256Ciphertext{{
    {0xf3ee'd1bdu, 0xb5d2'a03cu, 0x064b'5a7eu, 0x3db1'81f8u},
    {0x591c'cb10u, 0xd410'ed26u, 0xdc5b'a74au, 0x3136'2870u},
    {0xb6ed'21b9u, 0x9ca6'f4f9u, 0xf153'e7b1u, 0xbeaf'ed1du},
    {0x2330'4b7au, 0x39f9'f3ffu, 0x067d'8d8fu, 0x9e24'ecc7u},
    {0x8ea2'b7cau, 0x5167'45bfu, 0xeafc'4990u, 0x4b49'6089u},
}};

class AesMaster {
   public:
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    explicit AesMaster(Dut dut) : dut_(dut) {}

    Task<write_response_type> write(write_request_type request) {
        require_word_address(request.address);
        dut_.address.set(request.address >> 2u);
        dut_.write_data.set(request.data);
        dut_.cs.set(1);
        dut_.we.set(1);
        co_await Delay{4_ns};
        dut_.cs.set(0);
        dut_.we.set(0);
        co_return write_response_type{};
    }

    Task<read_response_type> read(read_request_type request) {
        require_word_address(request.address);
        dut_.address.set(request.address >> 2u);
        dut_.cs.set(1);
        dut_.we.set(0);
        co_await Delay{2_ns};
        const uint32_t value = dut_.read_data.get();
        dut_.cs.set(0);
        co_return read_response_type{.data = value};
    }

   private:
    static void require_word_address(uint32_t address) {
        if ((address & 3u) != 0 || address > 0x3fcu) {
            throw std::logic_error(
                "secworks AES frontdoor requires an 8-bit word address");
        }
    }

    Dut dut_;
};

static_assert(MemoryMappedMaster<AesMaster>);
using RegModel = secworks_aes_regs::RegModel<AesMaster>;
using Register = RegisterHandle<AesMaster>;

std::array<Register*, 8> key_registers(RegModel& regs) {
    return {&regs.key0, &regs.key1, &regs.key2, &regs.key3,
            &regs.key4, &regs.key5, &regs.key6, &regs.key7};
}

std::array<Register*, 4> block_registers(RegModel& regs) {
    return {&regs.block0, &regs.block1, &regs.block2, &regs.block3};
}

std::array<Register*, 4> result_registers(RegModel& regs) {
    return {&regs.result0, &regs.result1, &regs.result2, &regs.result3};
}

Task<void> initialize_key(RegModel& regs, const Words8& key, bool aes256) {
    const auto registers = key_registers(regs);
    for (std::size_t index = 0; index < registers.size(); ++index)
        co_await registers[index]->write(key[index]);

    co_await regs.config.write(aes256 ? 2u : 0u);
    co_await regs.control.write(1u);
    co_await Delay{200_ns};
}

Task<Words4> process_block(RegModel& regs, const Words4& input,
                           bool aes256, bool encipher) {
    const auto inputs = block_registers(regs);
    for (std::size_t index = 0; index < inputs.size(); ++index)
        co_await inputs[index]->write(input[index]);

    co_await regs.config.write((aes256 ? 2u : 0u) |
                               (encipher ? 1u : 0u));
    co_await regs.control.write(2u);
    co_await Delay{200_ns};

    Words4 result{};
    const auto outputs = result_registers(regs);
    for (std::size_t index = 0; index < outputs.size(); ++index)
        result[index] = (co_await outputs[index]->read()).data;
    co_return result;
}

Task<void> initialize_key_master(AesMaster& master, const Words8& key,
                                 bool aes256) {
    for (std::size_t index = 0; index < key.size(); ++index) {
        co_await master.write({kKeyAddress + static_cast<uint32_t>(index * 4),
                               key[index]});
    }
    co_await master.write({kConfigAddress, aes256 ? 2u : 0u});
    co_await master.write({kControlAddress, 1u});
    co_await Delay{200_ns};
}

Task<Words4> process_block_master(AesMaster& master, const Words4& input,
                                  bool aes256, bool encipher) {
    for (std::size_t index = 0; index < input.size(); ++index) {
        co_await master.write(
            {kBlockAddress + static_cast<uint32_t>(index * 4), input[index]});
    }
    co_await master.write(
        {kConfigAddress, (aes256 ? 2u : 0u) | (encipher ? 1u : 0u)});
    co_await master.write({kControlAddress, 2u});
    co_await Delay{200_ns};

    Words4 result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] =
            (co_await master.read(
                 {kResultAddress + static_cast<uint32_t>(index * 4)}))
                .data;
    }
    co_return result;
}

inline void begin_write(Dut dut, uint32_t address, uint32_t value) {
    dut.address.set(address >> 2u);
    dut.write_data.set(value);
    dut.cs.set(1);
    dut.we.set(1);
}

inline void end_write(Dut dut) {
    dut.cs.set(0);
    dut.we.set(0);
}

Task<void> initialize_key_fused(Dut dut, const Words8& key, bool aes256) {
    for (std::size_t index = 0; index < key.size(); ++index) {
        begin_write(dut, kKeyAddress + static_cast<uint32_t>(index * 4),
                    key[index]);
        co_await Delay{4_ns};
        end_write(dut);
    }
    begin_write(dut, kConfigAddress, aes256 ? 2u : 0u);
    co_await Delay{4_ns};
    end_write(dut);
    begin_write(dut, kControlAddress, 1u);
    co_await Delay{4_ns};
    end_write(dut);
    co_await Delay{200_ns};
}

Task<Words4> process_block_fused(Dut dut, const Words4& input,
                                 bool aes256, bool encipher) {
    for (std::size_t index = 0; index < input.size(); ++index) {
        begin_write(dut, kBlockAddress + static_cast<uint32_t>(index * 4),
                    input[index]);
        co_await Delay{4_ns};
        end_write(dut);
    }
    begin_write(dut, kConfigAddress,
                (aes256 ? 2u : 0u) | (encipher ? 1u : 0u));
    co_await Delay{4_ns};
    end_write(dut);
    begin_write(dut, kControlAddress, 2u);
    co_await Delay{4_ns};
    end_write(dut);
    co_await Delay{200_ns};

    Words4 result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        dut.address.set((kResultAddress + static_cast<uint32_t>(index * 4)) >>
                        2u);
        dut.cs.set(1);
        dut.we.set(0);
        co_await Delay{2_ns};
        result[index] = dut.read_data.get();
        dut.cs.set(0);
    }
    co_return result;
}

uint32_t fold(uint32_t checksum, const Words4& words) {
    for (const uint32_t word : words)
        checksum = (checksum ^ word) * 0x0100'0193u;
    return checksum;
}

Task<void> run_case(AccessMode mode, RegModel& regs, AesMaster& master, Dut dut,
                    TestContext& test, uint32_t suite,
                    uint32_t case_id, const Words8& key, bool aes256,
                    bool encipher, const Words4& input,
                    const Words4& expected, uint32_t& checksum,
                    uint32_t& cases, bool report_case) {
    Words4 actual{};
    if (mode == AccessMode::RegModel) {
        co_await initialize_key(regs, key, aes256);
        actual = co_await process_block(regs, input, aes256, encipher);
    } else if (mode == AccessMode::Master) {
        co_await initialize_key_master(master, key, aes256);
        actual = co_await process_block_master(master, input, aes256, encipher);
    } else {
        co_await initialize_key_fused(dut, key, aes256);
        actual = co_await process_block_fused(dut, input, aes256, encipher);
    }

    for (std::size_t word = 0; word < actual.size(); ++word)
        test.expect_eq("secworks AES result word", actual[word], expected[word]);
    checksum = fold(checksum, actual);
    ++cases;
    if (report_case) {
        std::printf(
            "AES_CASE suite=%u id=%u result=%08x%08x%08x%08x\n", suite,
            case_id, actual[0], actual[1], actual[2], actual[3]);
    }
}

uint32_t configured_repeats() {
    const char* text = std::getenv("AES_REGMODEL_REPEATS");
    if (!text || !*text) return 1;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 ||
        value > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(
            "AES_REGMODEL_REPEATS must be an integer in [1, 4294967295]");
    }
    return static_cast<uint32_t>(value);
}

AccessMode configured_mode() {
    const char* text = std::getenv("AES_REGMODEL_MODE");
    if (!text || !*text || std::string_view{text} == "regmodel")
        return AccessMode::RegModel;
    if (std::string_view{text} == "master") return AccessMode::Master;
    if (std::string_view{text} == "fused") return AccessMode::Fused;
    throw std::invalid_argument(
        "AES_REGMODEL_MODE must be regmodel, master, or fused");
}

std::string_view mode_name(AccessMode mode) {
    switch (mode) {
        case AccessMode::RegModel:
            return "regmodel";
        case AccessMode::Master:
            return "master";
        case AccessMode::Fused:
            return "fused";
    }
    return "unknown";
}

Task<void> run_suite(AccessMode mode, RegModel& regs, AesMaster& master, Dut dut,
                     TestContext& test, uint32_t suite,
                     uint32_t& checksum, uint32_t& cases,
                     bool report_cases) {
    for (uint32_t vector = 0; vector < 4; ++vector)
        co_await run_case(mode, regs, master, dut, test, suite, 1u + vector,
                          kAes128Key1,
                          false, true, kPlaintext[vector],
                          kAes128Ciphertext[vector], checksum, cases,
                          report_cases);
    for (uint32_t vector = 0; vector < 4; ++vector)
        co_await run_case(mode, regs, master, dut, test, suite, 5u + vector,
                          kAes128Key1,
                          false, false, kAes128Ciphertext[vector],
                          kPlaintext[vector], checksum, cases, report_cases);
    co_await run_case(mode, regs, master, dut, test, suite, 9, kAes128Key2,
                      false, true,
                      kPlaintext[4], kAes128Ciphertext[4], checksum, cases,
                      report_cases);
    co_await run_case(mode, regs, master, dut, test, suite, 10, kAes128Key2,
                      false, false,
                      kAes128Ciphertext[4], kPlaintext[4], checksum, cases,
                      report_cases);

    for (uint32_t vector = 0; vector < 4; ++vector)
        co_await run_case(mode, regs, master, dut, test, suite, 16u + vector,
                          kAes256Key1,
                          true, true, kPlaintext[vector],
                          kAes256Ciphertext[vector], checksum, cases,
                          report_cases);
    for (uint32_t vector = 0; vector < 4; ++vector)
        co_await run_case(mode, regs, master, dut, test, suite, 20u + vector,
                          kAes256Key1,
                          true, false, kAes256Ciphertext[vector],
                          kPlaintext[vector], checksum, cases, report_cases);
    co_await run_case(mode, regs, master, dut, test, suite, 24, kAes256Key2,
                      true, true,
                      kPlaintext[4], kAes256Ciphertext[4], checksum, cases,
                      report_cases);
    co_await run_case(mode, regs, master, dut, test, suite, 25, kAes256Key2,
                      true, false,
                      kAes256Ciphertext[4], kPlaintext[4], checksum, cases,
                      report_cases);
}

Task<void> secworks_aes_regmodel(Dut dut, TestContext& test) {
    dut.clk.set(0);
    dut.reset_n.set(1);
    dut.cs.set(0);
    dut.we.set(0);
    dut.address.set(0);
    dut.write_data.set(0);
    test.start_clock(dut.clk, 2_ns);

    dut.reset_n.set(0);
    co_await Delay{4_ns};
    dut.reset_n.set(1);

    AesMaster master{dut};
    RegModel regs{test, master};
    uint32_t checksum = 0x811c'9dc5u;
    uint32_t cases = 0;
    const uint32_t repeats = configured_repeats();
    const AccessMode mode = configured_mode();
    for (uint32_t suite = 0; suite < repeats; ++suite)
        co_await run_suite(mode, regs, master, dut, test, suite, checksum,
                           cases, repeats == 1);

    std::printf(
        "AES_REGMODEL_RESULT suites=%u cases=%u checks=%u checksum=%08x "
        "mode=%.*s\n",
        repeats, cases, cases * 4u, checksum,
        static_cast<int>(mode_name(mode).size()), mode_name(mode).data());
}

CPPTB_REGISTER_TEST(secworks_aes_regmodel);

}  // namespace
}  // namespace cpptb::benchmarks::secworks_aes
