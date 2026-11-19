// Purchased drop weight survives campaign save and load.
#include "doctest.h"

#define private public
#include "logisticsmissioninfo.h"
#undef private
#include "inifile.h"

#include <cstdio>
#include <fstream>

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
