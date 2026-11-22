#include "litbook/journal/setup.hpp"

#include "litbook/journal/journal_reader.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "journal/journal_test_support.hpp"
#include "support/temp_dir.hpp"
#include "support/unwrap.hpp"

namespace litbook::journal {
namespace {

using testing::sample_listing;

protocol::OrderEntryConfig sample_config() {
    protocol::OrderEntryConfig config;
    config.max_owners = 3;
    config.max_live_orders = 100;
    config.max_symbols = 2;
    return config;
}

std::vector<JournalRecord> sample_setup() {
    return {session_start_record(sample_config()),
            risk_limits_record(1, protocol::RiskLimits{.max_order_qty = 500, .max_open_orders = 9}),
            add_symbol_record(sample_listing(0, "AAPL")),
            add_symbol_record(sample_listing(1, "MSFT"))};
}

TEST(SetupTest, RecordsCarryTheirTypeOwnerAndContents) {
    const std::vector<JournalRecord> setup = sample_setup();
    EXPECT_EQ(setup[0].type, RecordType::SessionStart);
    EXPECT_EQ(litbook::testing::unwrap(session_config_of(setup[0])).max_live_orders, 100U);
    EXPECT_EQ(setup[1].type, RecordType::RiskLimits);
    EXPECT_EQ(setup[1].owner, 1U);
    EXPECT_EQ(litbook::testing::unwrap(risk_limits_of(setup[1])).max_order_qty, 500U);
    EXPECT_EQ(litbook::testing::unwrap(symbol_listing_of(setup[3])).ticker.name(), "MSFT");
    EXPECT_EQ(litbook::testing::unwrap(symbol_listing_of(setup[3])).id, 1U);
    for (const JournalRecord& r : setup) {
        EXPECT_TRUE(is_setup_record(r.type));
        EXPECT_EQ(r.now, 0U);
        EXPECT_EQ(r.sequence, 0U);
    }
}

TEST(SetupTest, ValuesTheJournalCannotStoreAreRefused) {
    protocol::OrderEntryConfig bad = sample_config();
    bad.max_owners = 0;
    EXPECT_THROW((void)session_start_record(bad), std::invalid_argument);
    EXPECT_THROW((void)risk_limits_record(kNoOwner, {}), std::invalid_argument);
    SymbolListing listing = sample_listing(0, "AAPL");
    listing.config.tick_size = 0;
    EXPECT_THROW((void)add_symbol_record(listing), std::invalid_argument);
}

TEST(SetupTest, OnlyTheThreeSetupTypesAreSetup) {
    EXPECT_FALSE(is_setup_record(RecordType::Frame));
    EXPECT_FALSE(is_setup_record(RecordType::CancelAll));
    EXPECT_FALSE(is_setup_record(RecordType::Stop));
    EXPECT_FALSE(is_setup_record(RecordType::Restart));
}

TEST(SetupTest, AppendedSetupRecordsReadBackTheSameWithTheirTime) {
    const litbook::testing::TempDir dir;
    const std::string path = dir.file("journal");
    const std::vector<JournalRecord> setup = sample_setup();
    {
        JournalWriter writer = JournalWriter::create(path, JournalWriterConfig{});
        for (const JournalRecord& r : setup) {
            ASSERT_EQ(append_setup_record(writer, 77, r), JournalStatus::Ok);
        }
        JournalRecord frame;
        frame.type = RecordType::Frame;
        EXPECT_EQ(append_setup_record(writer, 77, frame), JournalStatus::Refused);
        ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    }
    const MappedFile file(path);
    RecordReader reader(file.bytes().subspan(kFileHeaderLength));
    std::vector<JournalRecord> read;
    JournalRecord r;
    while (reader.next(r)) {
        read.push_back(r);
    }
    ASSERT_EQ(read.size(), setup.size());
    for (std::size_t i = 0; i < read.size(); ++i) {
        EXPECT_EQ(read[i].now, 77U);
        EXPECT_EQ(read[i].sequence, i + 1);
    }
    EXPECT_EQ(setup_difference(read, setup), "");
}

TEST(SetupTest, TheSameSetupHasNoDifference) {
    const std::vector<JournalRecord> a = sample_setup();
    EXPECT_EQ(setup_difference(a, a), "");
    EXPECT_EQ(setup_difference({}, {}), "");
}

TEST(SetupTest, ADifferentStockIsNamedWithItsPosition) {
    const std::vector<JournalRecord> journal = sample_setup();
    std::vector<JournalRecord> now = sample_setup();
    now[3] = add_symbol_record(sample_listing(1, "ZVZZT"));
    const std::string d = setup_difference(journal, now);
    EXPECT_NE(d.find("Setup record 4"), std::string::npos) << d;
    EXPECT_NE(d.find("MSFT"), std::string::npos) << d;
    EXPECT_NE(d.find("ZVZZT"), std::string::npos) << d;
}

TEST(SetupTest, DifferentLimitsOwnersAndConfigsAreFound) {
    const std::vector<JournalRecord> journal = sample_setup();
    std::vector<JournalRecord> now = sample_setup();
    now[1] =
        risk_limits_record(1, protocol::RiskLimits{.max_order_qty = 501, .max_open_orders = 9});
    EXPECT_NE(setup_difference(journal, now), "");
    now = sample_setup();
    now[1] =
        risk_limits_record(2, protocol::RiskLimits{.max_order_qty = 500, .max_open_orders = 9});
    EXPECT_NE(setup_difference(journal, now).find("account 2"), std::string::npos);
    now = sample_setup();
    protocol::OrderEntryConfig config = sample_config();
    config.max_live_orders = 101;
    now[0] = session_start_record(config);
    EXPECT_NE(setup_difference(journal, now).find("Setup record 1"), std::string::npos);
    now = sample_setup();
    SymbolListing listing = sample_listing(1, "MSFT");
    listing.config.lot_size = 100;
    now[3] = add_symbol_record(listing);
    EXPECT_NE(setup_difference(journal, now), "");
}

TEST(SetupTest, ExtraAndMissingRecordsAreNamed) {
    const std::vector<JournalRecord> full = sample_setup();
    const std::vector<JournalRecord> shorter(full.begin(), full.end() - 1);
    const std::string extra = setup_difference(full, shorter);
    EXPECT_NE(extra.find("first extra one"), std::string::npos) << extra;
    EXPECT_NE(extra.find("MSFT"), std::string::npos) << extra;
    const std::string missing = setup_difference(shorter, full);
    EXPECT_NE(missing.find("missing from the journal"), std::string::npos) << missing;
    EXPECT_NE(missing.find("MSFT"), std::string::npos) << missing;
}

TEST(SetupTest, DescriptionsNameWhatARecordHolds) {
    const std::vector<JournalRecord> setup = sample_setup();
    EXPECT_NE(describe_record(setup[0]).find("max live orders 100"), std::string::npos);
    EXPECT_NE(describe_record(setup[1]).find("max order quantity 500"), std::string::npos);
    EXPECT_NE(describe_record(setup[2]).find("AAPL (id 0"), std::string::npos);
    JournalRecord stop;
    stop.type = RecordType::Stop;
    EXPECT_EQ(describe_record(stop), "Stop");
}

}  // namespace
}  // namespace litbook::journal
