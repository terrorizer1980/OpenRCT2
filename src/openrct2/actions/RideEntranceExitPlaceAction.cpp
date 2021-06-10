/*****************************************************************************
 * Copyright (c) 2014-2020 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "RideEntranceExitPlaceAction.h"

#include "../actions/RideEntranceExitRemoveAction.h"
#include "../management/Finance.h"
#include "../ride/Ride.h"
#include "../ride/Station.h"
#include "../world/MapAnimation.h"

RideEntranceExitPlaceAction::RideEntranceExitPlaceAction(
    const CoordsXY& loc, Direction direction, ride_id_t rideIndex, StationIndex stationNum, bool isExit)
    : _loc(loc)
    , _direction(direction)
    , _rideIndex(rideIndex)
    , _stationNum(stationNum)
    , _isExit(isExit)
{
}

void RideEntranceExitPlaceAction::AcceptParameters(GameActionParameterVisitor& visitor)
{
    visitor.Visit(_loc);
    visitor.Visit("direction", _direction);
    visitor.Visit("ride", _rideIndex);
    visitor.Visit("station", _stationNum);
    visitor.Visit("isExit", _isExit);
}

uint16_t RideEntranceExitPlaceAction::GetActionFlags() const
{
    return GameAction::GetActionFlags();
}

void RideEntranceExitPlaceAction::Serialise(DataSerialiser& stream)
{
    GameAction::Serialise(stream);

    stream << DS_TAG(_loc) << DS_TAG(_direction) << DS_TAG(_rideIndex) << DS_TAG(_stationNum) << DS_TAG(_isExit);
}

GameActions::Result::Ptr RideEntranceExitPlaceAction::Query() const
{
    auto errorTitle = _isExit ? STR_CANT_BUILD_MOVE_EXIT_FOR_THIS_RIDE_ATTRACTION
                              : STR_CANT_BUILD_MOVE_ENTRANCE_FOR_THIS_RIDE_ATTRACTION;
    if (!MapCheckCapacityAndReorganise(_loc))
    {
        return MakeResult(GameActions::Status::NoFreeElements, errorTitle);
    }

    auto ride = get_ride(_rideIndex);
    if (ride == nullptr)
    {
        log_warning("Invalid game command for ride %d", static_cast<int32_t>(_rideIndex));
        return MakeResult(GameActions::Status::InvalidParameters, errorTitle);
    }

    if (_stationNum >= MAX_STATIONS)
    {
        log_warning("Invalid station number for ride. stationNum: %u", _stationNum);
        return MakeResult(GameActions::Status::InvalidParameters, errorTitle);
    }

    if (ride->status != RideStatus::Closed && ride->status != RideStatus::Simulating)
    {
        return MakeResult(GameActions::Status::NotClosed, errorTitle, STR_MUST_BE_CLOSED_FIRST);
    }

    if (ride->lifecycle_flags & RIDE_LIFECYCLE_INDESTRUCTIBLE_TRACK)
    {
        return MakeResult(GameActions::Status::Disallowed, errorTitle, STR_NOT_ALLOWED_TO_MODIFY_STATION);
    }

    const auto location = _isExit ? ride_get_exit_location(ride, _stationNum) : ride_get_entrance_location(ride, _stationNum);

    if (!location.isNull())
    {
        auto rideEntranceExitRemove = RideEntranceExitRemoveAction(location.ToCoordsXY(), _rideIndex, _stationNum, _isExit);
        rideEntranceExitRemove.SetFlags(GetFlags());

        auto result = GameActions::QueryNested(&rideEntranceExitRemove);
        if (result->Error != GameActions::Status::Ok)
        {
            return result;
        }
    }

    auto z = ride->stations[_stationNum].GetBaseZ();
    if (!LocationValid(_loc) || (!gCheatsSandboxMode && !map_is_location_owned({ _loc, z })))
    {
        return MakeResult(GameActions::Status::NotOwned, errorTitle);
    }

    auto clear_z = z + (_isExit ? RideExitHeight : RideEntranceHeight);
    auto cost = MONEY32_UNDEFINED;
    if (!map_can_construct_with_clear_at(
            { _loc, z, clear_z }, &map_place_non_scenery_clear_func, { 0b1111, 0 }, GetFlags(), &cost,
            CREATE_CROSSING_MODE_NONE))
    {
        return MakeResult(GameActions::Status::NoClearance, errorTitle, gGameCommandErrorText, gCommonFormatArgs);
    }

    if (gMapGroundFlags & ELEMENT_IS_UNDERWATER)
    {
        return MakeResult(GameActions::Status::Disallowed, errorTitle, STR_RIDE_CANT_BUILD_THIS_UNDERWATER);
    }

    if (z > MaxRideEntranceOrExitHeight)
    {
        return MakeResult(GameActions::Status::Disallowed, errorTitle, STR_TOO_HIGH);
    }

    auto res = MakeResult();
    res->Position = { _loc.ToTileCentre(), z };
    res->Expenditure = ExpenditureType::RideConstruction;
    return res;
}

GameActions::Result::Ptr RideEntranceExitPlaceAction::Execute() const
{
    // Remember when in unknown station num mode rideIndex is unknown and z is set
    // When in known station num mode rideIndex is known and z is unknown
    auto errorTitle = _isExit ? STR_CANT_BUILD_MOVE_EXIT_FOR_THIS_RIDE_ATTRACTION
                              : STR_CANT_BUILD_MOVE_ENTRANCE_FOR_THIS_RIDE_ATTRACTION;
    auto ride = get_ride(_rideIndex);
    if (ride == nullptr)
    {
        log_warning("Invalid game command for ride %d", static_cast<int32_t>(_rideIndex));
        return MakeResult(GameActions::Status::InvalidParameters, errorTitle);
    }

    if (!(GetFlags() & GAME_COMMAND_FLAG_GHOST))
    {
        ride_clear_for_construction(ride);
        ride_remove_peeps(ride);
    }

    const auto location = _isExit ? ride_get_exit_location(ride, _stationNum) : ride_get_entrance_location(ride, _stationNum);
    if (!location.isNull())
    {
        auto rideEntranceExitRemove = RideEntranceExitRemoveAction(location.ToCoordsXY(), _rideIndex, _stationNum, _isExit);
        rideEntranceExitRemove.SetFlags(GetFlags());

        auto result = GameActions::ExecuteNested(&rideEntranceExitRemove);
        if (result->Error != GameActions::Status::Ok)
        {
            return result;
        }
    }

    auto z = ride->stations[_stationNum].GetBaseZ();
    if (!(GetFlags() & GAME_COMMAND_FLAG_ALLOW_DURING_PAUSED) && !(GetFlags() & GAME_COMMAND_FLAG_GHOST))
    {
        footpath_remove_litter({ _loc, z });
        wall_remove_at_z({ _loc, z });
    }

    auto clear_z = z + (_isExit ? RideExitHeight : RideEntranceHeight);
    auto cost = MONEY32_UNDEFINED;
    if (!map_can_construct_with_clear_at(
            { _loc, z, clear_z }, &map_place_non_scenery_clear_func, { 0b1111, 0 }, GetFlags() | GAME_COMMAND_FLAG_APPLY, &cost,
            CREATE_CROSSING_MODE_NONE))
    {
        return MakeResult(GameActions::Status::NoClearance, errorTitle, gGameCommandErrorText, gCommonFormatArgs);
    }

    auto res = MakeResult();
    res->Position = { _loc.ToTileCentre(), z };
    res->Expenditure = ExpenditureType::RideConstruction;

    auto* entranceElement = TileElementInsert<EntranceElement>(CoordsXYZ{ _loc, z }, 0b1111);
    Guard::Assert(entranceElement != nullptr);

    entranceElement->SetDirection(_direction);
    entranceElement->SetClearanceZ(clear_z);
    entranceElement->SetEntranceType(_isExit ? ENTRANCE_TYPE_RIDE_EXIT : ENTRANCE_TYPE_RIDE_ENTRANCE);
    entranceElement->SetStationIndex(_stationNum);
    entranceElement->SetRideIndex(_rideIndex);
    entranceElement->SetGhost(GetFlags() & GAME_COMMAND_FLAG_GHOST);

    if (_isExit)
    {
        ride_set_exit_location(ride, _stationNum, TileCoordsXYZD(CoordsXYZD{ _loc, z, entranceElement->GetDirection() }));
    }
    else
    {
        ride_set_entrance_location(ride, _stationNum, TileCoordsXYZD(CoordsXYZD{ _loc, z, entranceElement->GetDirection() }));
        ride->stations[_stationNum].LastPeepInQueue = SPRITE_INDEX_NULL;
        ride->stations[_stationNum].QueueLength = 0;

        map_animation_create(MAP_ANIMATION_TYPE_RIDE_ENTRANCE, { _loc, z });
    }

    footpath_queue_chain_reset();

    if (!(GetFlags() & GAME_COMMAND_FLAG_GHOST))
    {
        maze_entrance_hedge_removal({ _loc, entranceElement->as<TileElement>() });
    }

    footpath_connect_edges(_loc, entranceElement->as<TileElement>(), GetFlags());
    footpath_update_queue_chains();

    map_invalidate_tile_full(_loc);

    return res;
}

GameActions::Result::Ptr RideEntranceExitPlaceAction::TrackPlaceQuery(const CoordsXYZ& loc, const bool isExit)
{
    auto errorTitle = isExit ? STR_CANT_BUILD_MOVE_EXIT_FOR_THIS_RIDE_ATTRACTION
                             : STR_CANT_BUILD_MOVE_ENTRANCE_FOR_THIS_RIDE_ATTRACTION;
    if (!MapCheckCapacityAndReorganise(loc))
    {
        return MakeResult(GameActions::Status::NoFreeElements, errorTitle);
    }

    if (!gCheatsSandboxMode && !map_is_location_owned(loc))
    {
        return MakeResult(GameActions::Status::NotOwned, errorTitle);
    }

    int16_t baseZ = loc.z;
    int16_t clearZ = baseZ + (isExit ? RideExitHeight : RideEntranceHeight);
    auto cost = MONEY32_UNDEFINED;
    if (!map_can_construct_with_clear_at(
            { loc, baseZ, clearZ }, &map_place_non_scenery_clear_func, { 0b1111, 0 }, 0, &cost, CREATE_CROSSING_MODE_NONE))
    {
        return MakeResult(GameActions::Status::NoClearance, errorTitle, gGameCommandErrorText, gCommonFormatArgs);
    }

    if (gMapGroundFlags & ELEMENT_IS_UNDERWATER)
    {
        return MakeResult(GameActions::Status::Disallowed, errorTitle, STR_RIDE_CANT_BUILD_THIS_UNDERWATER);
    }

    if (baseZ > MaxRideEntranceOrExitHeight)
    {
        return MakeResult(GameActions::Status::Disallowed, errorTitle, STR_TOO_HIGH);
    }
    auto res = MakeResult();
    res->Position = { loc.ToTileCentre(), tile_element_height(loc) };
    res->Expenditure = ExpenditureType::RideConstruction;
    return res;
}
