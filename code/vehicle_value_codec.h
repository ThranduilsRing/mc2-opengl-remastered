// Packet F preparation; native GroundVehicleData writes remain active until flip.
#pragma once
#include "gvehicl.h"
#include "mover_value_codec.h"

namespace mc2::save {
inline constexpr auto vehicleTag = fourcc('V', 'E', 'H', 'I');
struct VehicleRecord {
    GroundVehicleData value{};
    void serialize(Archive& a) {
        auto& v = value;
        moverFields(a, v);
        a(v.accel, v.velocityMag, v.suspension, v.movementEnabled, v.turretEnabled,
          v.turretRotation, v.turretBlownThisFrame, v.captureable, v.deadByCrushed,
          v.canRefit, v.canRecover, v.refitting, v.recovering, v.mineSweeper,
          v.sweepTime, v.mineLayer, v.aerospaceSpotter, v.cellColToMine, v.cellRowToMine,
          v.notMineYet, v.battleRating, v.descID, v.timeInCurrent);
        a(v.dVel.x, v.dVel.y, v.dVel.z, v.dRot.x, v.dRot.y, v.dRot.z,
          v.dAcc.x, v.dAcc.y, v.dAcc.z, v.dRVel.x, v.dRVel.y, v.dRVel.z,
          v.dRacc.x, v.dRacc.y, v.dRacc.z, v.dTime, v.sensorOK, v.isVehiclePilot);
    }
};
// No GroundVehicleData value fields derive. sensorOK persists sensor health;
// Load recreates the sensor owner/range/team registration, without contacts.
// Runtime exitTimer remains transient (restarts after load, as before).
}
