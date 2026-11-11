// B7.6 Contacts value records (design: .claude/cleanup/cxx23-step4-save-schema-design.md, Contacts / Sensor / TeamSensor /
// ContactInfo / Ecm rows). Field list is taken from code/contact.h (ContactInfo :51, SensorSystem :133, TeamSensorSystem :258,
// SensorSystemManager :345) and code/team.h:40 (SystemTracker). Private codec only: Mission::save does not write contacts
// today (they are rescanned on load) and nothing calls this.
// Mover ids are the MAX_MOVERS pool indexes; the sparse ContactInfo::sensors array stores only slots != NO_SENSOR_CONTACT.
#pragma once
#include "save_sections.h"
#include "tactical_order_queue.h"

#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace mc2::save {

inline constexpr auto tagContacts = fourcc('C', 'N', 'T', 'C');
// Mirrors of MAX_TEAMS (mclib/dstd.h), MAX_SENSORS/MAX_ECMS (code/dcontact.h), MAX_MOVERS (mclib/dmovemgr.h),
// NUM_CONTACT_STATUSES (code/dcontact.h); kept local so this header does not pull the game headers.
inline constexpr std::uint32_t contactMaxTeams = 8;
inline constexpr std::uint32_t contactMaxSensors = 512;
inline constexpr std::uint32_t contactMaxEcms = 50;
inline constexpr std::uint32_t contactMaxMovers = 1024;
inline constexpr std::uint32_t contactStatusCount = 6;
inline constexpr std::uint16_t contactNoSensorSlot = 0xFFFF;

struct ContactSensorSlot {
    std::uint16_t sensor = 0, contactIndex = 0; // ContactInfo::sensors[sensor] = index into that sensor's contact list
    void serialize(Archive& a) {
        a(sensor, contactIndex);
        if (!a.good()) return;
        if (sensor >= contactMaxSensors) a.reject("contact_slot_sensor");
        else if (contactIndex >= contactMaxMovers) a.reject("contact_slot_index");
    }
    bool operator==(const ContactSensorSlot&) const = default;
};

struct ContactInfoRecord {
    std::uint32_t moverId = 0;
    std::array<std::uint8_t, contactMaxTeams> contactStatus{};
    std::array<std::uint16_t, contactMaxTeams> contactCount{}, teams{};
    std::array<std::uint32_t, contactMaxTeams> teamSpotter{};
    std::vector<ContactSensorSlot> sensors; // ascending sensor, unique; absent slots are NO_SENSOR_CONTACT

    void serialize(Archive& a) {
        a(moverId, contactStatus, contactCount, teams, teamSpotter);
        a.sequence(sensors, contactMaxSensors);
        if (!a.good()) return;
        if (moverId >= contactMaxMovers) { a.reject("contact_mover_id"); return; }
        for (auto s : contactStatus)
            if (s >= contactStatusCount) { a.reject("contact_status"); return; }
        for (std::size_t i = 1; i < sensors.size(); ++i)
            if (sensors[i].sensor <= sensors[i - 1].sensor) { a.reject("contact_slot_order"); return; }
    }
    bool operator==(const ContactInfoRecord&) const = default;
};

struct SensorRecord {
    std::uint32_t id = 0;
    std::int32_t masterTeamId = -1, masterIndex = -1; // -1 = no master / not in a team list
    std::optional<mc2::sim::ObjectRef> owner;
    float range = 0;
    std::int32_t skill = 0;
    bool broken = false, notShutdown = true, hasLOSCapability = false;
    float ecmEffect = 0, nextScanUpdate = 0, lastScanUpdate = 0;
    std::vector<std::uint32_t> contacts; // mover ids in contact-list order (numContacts = size)
    std::int32_t numExclusives = 0, totalContacts = 0;

    void serialize(Archive& a) {
        a(id, masterTeamId, masterIndex, owner, range, skill, broken, notShutdown, hasLOSCapability, ecmEffect,
          nextScanUpdate, lastScanUpdate);
        a.sequence(contacts, contactMaxMovers);
        a(numExclusives, totalContacts);
        if (!a.good()) return;
        if (id >= contactMaxSensors) { a.reject("sensor_id"); return; }
        if (masterTeamId < -1 || masterTeamId >= static_cast<std::int32_t>(contactMaxTeams)) { a.reject("sensor_team"); return; }
        if (masterIndex < -1 || masterIndex >= static_cast<std::int32_t>(contactMaxSensors)) { a.reject("sensor_master_index"); return; }
        if (ecmEffect < 0.0f) { a.reject("sensor_ecm_effect"); return; }
        if (numExclusives < 0 || numExclusives > static_cast<std::int32_t>(contacts.size()) || totalContacts < 0) {
            a.reject("sensor_counts");
            return;
        }
        for (auto m : contacts)
            if (m >= contactMaxMovers) { a.reject("sensor_contact_mover"); return; }
    }
    bool operator==(const SensorRecord&) const = default;
};

struct EcmRecord { // ECMInfo (manager) and SystemTracker (team lists); masterId is -1 for the manager copy
    std::optional<mc2::sim::ObjectRef> owner;
    std::int32_t masterId = -1;
    float range = 0, effect = 0;

    void serialize(Archive& a) {
        a(owner, masterId, range, effect);
        if (!a.good()) return;
        if (masterId < -1 || masterId >= static_cast<std::int32_t>(contactMaxSensors)) a.reject("ecm_master");
        else if (range < 0.0f || effect < 0.0f) a.reject("ecm_value");
    }
    bool operator==(const EcmRecord&) const = default;
};

struct TeamSensorRecord {
    std::uint32_t teamId = 0;
    std::int32_t nextContactId = 0, numContactUpdatesPerPass = 0, curContactUpdate = 0, numEnemyContacts = 0;
    std::vector<std::uint32_t> contacts;  // mover ids, scan order
    std::vector<std::uint32_t> sensorIds; // sensor pool ids, list order
    std::vector<EcmRecord> ecms, jammers; // jammers are never populated by the engine today; kept for the design row

    void serialize(Archive& a) {
        a(teamId, nextContactId, numContactUpdatesPerPass, curContactUpdate, numEnemyContacts);
        a.sequence(contacts, contactMaxMovers);
        a.sequence(sensorIds, contactMaxSensors);
        a.sequence(ecms, contactMaxEcms);
        a.sequence(jammers, contactMaxEcms);
        if (!a.good()) return;
        if (teamId >= contactMaxTeams) { a.reject("team_sensor_id"); return; }
        if (nextContactId < 0 || numContactUpdatesPerPass < 0 || curContactUpdate < 0 || numEnemyContacts < 0 ||
            numEnemyContacts > static_cast<std::int32_t>(contacts.size())) {
            a.reject("team_sensor_counts");
            return;
        }
        for (auto m : contacts)
            if (m >= contactMaxMovers) { a.reject("team_sensor_contact"); return; }
        for (auto s : sensorIds)
            if (s >= contactMaxSensors) { a.reject("team_sensor_sensor"); return; }
    }
    bool operator==(const TeamSensorRecord&) const = default;
};

struct ContactsState {
    std::vector<SensorRecord> sensors;          // ascending id, unique (live pool entries)
    std::vector<std::uint32_t> freeSensorIds;   // allocation order, unique, disjoint from sensors
    std::vector<TeamSensorRecord> teamSensors;  // ascending teamId, unique
    std::vector<EcmRecord> ecms;                // manager ECM list in insertion order
    std::int32_t teamToUpdate = 0;
    float scanFrequency = 0, updateFrequency = 0;
    bool homeTeamInContact = false, enemyInLOS = false;
    std::vector<ContactInfoRecord> contactInfos; // ascending moverId, unique

    void serialize(Archive& a) {
        a.sequence(sensors, contactMaxSensors);
        a.sequence(freeSensorIds, contactMaxSensors);
        a.sequence(teamSensors, contactMaxTeams);
        a.sequence(ecms, contactMaxEcms);
        a(teamToUpdate, scanFrequency, updateFrequency, homeTeamInContact, enemyInLOS);
        a.sequence(contactInfos, contactMaxMovers);
        if (!a.good()) return;
        if (teamToUpdate < 0 || teamToUpdate >= static_cast<std::int32_t>(contactMaxTeams)) { a.reject("contacts_team_to_update"); return; }
        if (scanFrequency < 0.0f || updateFrequency < 0.0f) { a.reject("contacts_frequency"); return; }
        for (std::size_t i = 1; i < sensors.size(); ++i)
            if (sensors[i].id <= sensors[i - 1].id) { a.reject("contacts_sensor_order"); return; }
        for (std::size_t i = 1; i < teamSensors.size(); ++i)
            if (teamSensors[i].teamId <= teamSensors[i - 1].teamId) { a.reject("contacts_team_order"); return; }
        for (std::size_t i = 1; i < contactInfos.size(); ++i)
            if (contactInfos[i].moverId <= contactInfos[i - 1].moverId) { a.reject("contacts_info_order"); return; }
        std::array<bool, contactMaxSensors> live{}, seen{};
        for (const auto& s : sensors) live[s.id] = true;
        for (auto f : freeSensorIds) {
            if (f >= contactMaxSensors || live[f] || seen[f]) { a.reject("contacts_free_sensor"); return; }
            seen[f] = true;
        }
        for (const auto& t : teamSensors)
            for (auto s : t.sensorIds)
                if (!live[s]) { a.reject("contacts_team_sensor_ref"); return; }
    }
    bool operator==(const ContactsState&) const = default;
};

} // namespace mc2::save
