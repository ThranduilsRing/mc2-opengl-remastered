// Drop weight: the price curve, the over-limit gate, and the purchased-weight save/load round trip.
#include "doctest.h"

#define private public
#include "logisticsmissioninfo.h"
#undef private
#include "dropweight_rules.h"
#include "inifile.h"

#include <cstdio>
#include <fstream>

TEST_CASE("drop weight price grows log-style per purchase") {
    CHECK(dropweight::priceForNext(0) == 25000);
    CHECK(dropweight::priceForNext(-3) == 25000);
    long prev = 0;
    for (int n = 0; n < 40; ++n) {
        const long p = dropweight::priceForNext(n);
        CHECK(p > prev);
        CHECK(p % 100 == 0);
        prev = p;
    }
    // base * (1 + k * ln(1 + n)): n = 9 -> 25000 * (1 + 0.75 * ln 10) = 68,173 -> 68,200 after rounding
    CHECK(dropweight::priceForNext(9) == 68200);
    // log growth: the step shrinks as n grows
    CHECK(dropweight::priceForNext(20) - dropweight::priceForNext(19) < dropweight::priceForNext(2) - dropweight::priceForNext(1));
}

TEST_CASE("over-limit gate: strictly above the limit only") {
    CHECK_FALSE(dropweight::overLimit(190, 190));
    CHECK_FALSE(dropweight::overLimit(185, 190));
    CHECK(dropweight::overLimit(191, 190));
    // a purchase lifts the limit by 5 t and clears a 5 t overage
    CHECK_FALSE(dropweight::overLimit(195, 190 + dropweight::kTonsPerPurchase));
}

TEST_CASE("purchased drop weight survives campaign save and load") {
    const char* campaign = "dw_roundtrip_campaign.fit";
    const char* save = "dw_roundtrip_save.fit";
    {
        std::ofstream out(campaign, std::ios::binary);
        out << "FITini\r\n[Campaign]\r\nst CampaignName = \"dw\"\r\nl GroupCount = 1\r\nl CBills = 5\r\n"
               "[Group0]\r\nl MissionCount = 0\r\n";
    }
    {
        LogisticsMissionInfo info;
        info.campaignName = campaign;
        info.campaignDisplayName = "dw";
        info.groupCount = 1;
        info.groups = std::make_unique<LogisticsMissionInfo::MissionGroup[]>(1);
        info.groups[0].infos.push_back(std::make_unique<LogisticsMissionInfo::MissionInfo>());
        info.currentMission = 0;
        info.addDropWeightPurchase();
        info.addDropWeightPurchase();
        info.addDropWeightPurchase();
        FitIniFile out;
        REQUIRE(out.create(save) == NO_ERR);
        out.writeBlock("General");
        info.save(out);
        out.close();
    }
    {
        LogisticsMissionInfo info;
        FitIniFile in;
        REQUIRE(in.open(save) == NO_ERR);
        REQUIRE(info.load(in).has_value());
        in.close();
        CHECK(info.getDropWeightPurchases() == 3);
    }
    {   // no purchases: the key is not written and a load reads 0
        LogisticsMissionInfo info;
        info.campaignName = campaign;
        info.groupCount = 1;
        info.groups = std::make_unique<LogisticsMissionInfo::MissionGroup[]>(1);
        info.groups[0].infos.push_back(std::make_unique<LogisticsMissionInfo::MissionInfo>());
        info.currentMission = 0;
        FitIniFile out;
        REQUIRE(out.create(save) == NO_ERR);
        out.writeBlock("General");
        info.save(out);
        out.close();
        LogisticsMissionInfo again;
        FitIniFile in;
        REQUIRE(in.open(save) == NO_ERR);
        REQUIRE(again.load(in).has_value());
        in.close();
        CHECK(again.getDropWeightPurchases() == 0);
    }
    std::remove(campaign);
    std::remove(save);
}
