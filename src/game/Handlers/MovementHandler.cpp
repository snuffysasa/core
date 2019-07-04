/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "Corpse.h"
#include "Player.h"
#include "MapManager.h"
#include "Transport.h"
#include "BattleGround.h"
#include "WaypointMovementGenerator.h"
#include "MapPersistentStateMgr.h"
#include "ObjectMgr.h"

#include "World.h"
#include "Anticheat.h"
#include "packet_builder.h"
#include "MoveSpline.h"
#include "MovementBroadcaster.h"
#include "MovementPacketSender.h"


void WorldSession::HandleMoveWorldportAckOpcode(WorldPacket & /*recv_data*/)
{
    DEBUG_LOG("WORLD: got MSG_MOVE_WORLDPORT_ACK.");
    HandleMoveWorldportAckOpcode();
}

void WorldSession::HandleMoveWorldportAckOpcode()
{
    // ignore unexpected far teleports
    if (!GetPlayer()->IsBeingTeleportedFar())
        return;

    // get start teleport coordinates (will used later in fail case)
    WorldLocation old_loc;
    GetPlayer()->GetPosition(old_loc);

    // get the teleport destination
    WorldLocation &loc = GetPlayer()->GetTeleportDest();

    // possible errors in the coordinate validity check (only cheating case possible)
    if (!MapManager::IsValidMapCoord(loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation))
    {
        sLog.outError("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far to a not valid location "
                      "(map:%u, x:%f, y:%f, z:%f) We port him to his homebind instead..",
                      GetPlayer()->GetGuidStr().c_str(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);
        // stop teleportation else we would try this again and again in LogoutPlayer...
        GetPlayer()->SetSemaphoreTeleportFar(false);
        // and teleport the player to a valid place
        GetPlayer()->TeleportToHomebind();
        return;
    }

    // get the destination map entry, not the current one, this will fix homebind and reset greeting
    MapEntry const* mEntry = sMapStorage.LookupEntry<MapEntry>(loc.mapid);

    Map* map = nullptr;

    // prevent crash at attempt landing to not existed battleground instance
    if (mEntry->IsBattleGround())
    {
        if (GetPlayer()->GetBattleGroundId())
            map = sMapMgr.FindMap(loc.mapid, GetPlayer()->GetBattleGroundId());

        if (!map)
        {
            DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far to nonexisten battleground instance "
                       " (map:%u, x:%f, y:%f, z:%f) Trying to port him to his previous place..",
                       GetPlayer()->GetGuidStr().c_str(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);

            GetPlayer()->SetSemaphoreTeleportFar(false);

            // Teleport to previous place, if cannot be ported back TP to homebind place
            if (!GetPlayer()->TeleportTo(old_loc))
            {
                DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s cannot be ported to his previous place, teleporting him to his homebind place...",
                           GetPlayer()->GetGuidStr().c_str());
                GetPlayer()->TeleportToHomebind();
            }
            return;
        }
    }

    // reset instance validity, except if going to an instance inside an instance
    if (GetPlayer()->m_InstanceValid == false && !mEntry->IsDungeon())
        GetPlayer()->m_InstanceValid = true;

    // relocate the player to the teleport destination
    if (!map)
    {
        if (loc.mapid <= 1)
            GetPlayer()->SetLocationInstanceId(sMapMgr.GetContinentInstanceId(loc.mapid, loc.coord_x, loc.coord_y));
        map = sMapMgr.CreateMap(loc.mapid, GetPlayer());
    }

    GetPlayer()->SetMap(map);
    if (Transport* t = GetPlayer()->GetTransport()) // Transport position may have changed while loading
        t->UpdatePassengerPosition(GetPlayer());
    else
        GetPlayer()->Relocate(loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation);

    GetPlayer()->SendInitialPacketsBeforeAddToMap();
    // the CanEnter checks are done in TeleporTo but conditions may change
    // while the player is in transit, for example the map may get full
    if (!GetPlayer()->GetMap()->Add(GetPlayer()))
    {
        GetPlayer()->SetSemaphoreTeleportFar(false);
        // if player wasn't added to map, reset his map pointer!
        GetPlayer()->ResetMap();

        DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far but couldn't be added to map "
                   " (map:%u, x:%f, y:%f, z:%f) Trying to port him to his previous place..",
                   GetPlayer()->GetGuidStr().c_str(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);

        // Teleport to previous place, if cannot be ported back TP to homebind place
        if (!GetPlayer()->TeleportTo(old_loc))
        {
            DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s cannot be ported to his previous place, teleporting him to his homebind place...",
                       GetPlayer()->GetGuidStr().c_str());
            GetPlayer()->TeleportToHomebind();
        }
        return;
    }
    GetPlayer()->SetSemaphoreTeleportFar(false);

    // battleground state prepare (in case join to BG), at relogin/tele player not invited
    // only add to bg group and object, if the player was invited (else he entered through command)
    if (_player->InBattleGround())
    {
        // cleanup seting if outdated
        if (!mEntry->IsBattleGround())
        {
            // We're not in BG
            _player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
            // reset destination bg team
            _player->SetBGTeam(TEAM_NONE);
        }
        // join to bg case
        else if (BattleGround *bg = _player->GetBattleGround())
        {
            if (_player->IsInvitedForBattleGroundInstance(_player->GetBattleGroundId()))
                bg->AddPlayer(_player);
        }
    }

    GetPlayer()->SendInitialPacketsAfterAddToMap();

    // flight fast teleport case
    if (GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
    {
        // the movement could be expiring so check for destination (ritual of summoning / gm teleport)
        if (!_player->InBattleGround() && !_player->m_taxi.empty())
        {
            // short preparations to continue flight
            FlightPathMovementGenerator* flight = (FlightPathMovementGenerator*)(GetPlayer()->GetMotionMaster()->top());
            flight->Reset(*GetPlayer());
            return;
        }

        // battleground state prepare, stop flight
        GetPlayer()->GetMotionMaster()->MovementExpired();
        GetPlayer()->m_taxi.ClearTaxiDestinations();
    }

    if (mEntry->IsRaid())
    {
        if (time_t timeReset = sMapPersistentStateMgr.GetScheduler().GetResetTimeFor(mEntry->id))
        {
            uint32 timeleft = uint32(timeReset - time(nullptr));
            GetPlayer()->SendInstanceResetWarning(mEntry->id, timeleft);
        }
    }

    // mount allow check
    if (!mEntry->IsMountAllowed())
        _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    // honorless target
    if (!GetPlayer()->pvpInfo.inPvPEnforcedArea)
        GetPlayer()->RemoveDelayedOperation(DELAYED_CAST_HONORLESS_TARGET);

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    //lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();

    // Let the client know its new position by sending a heartbeat!
    // The Windows client figures this out by itself, but the MacOS one does
    // not.
    //
    // On a successful port, the camera of the MacOS client is facing south and
    // ignores any movement from the transport object. Triggering
    // `SMSG_STANDSTATE_UPDATE' with its current state resets the camera
    // (implemented in `WorldSession::HandleZoneUpdateOpcode').
    if (_clientOS == CLIENT_OS_MAC && GetPlayer()->m_movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
    {
        GetPlayer()->SendHeartBeat(true);
    }
}

void WorldSession::HandleMoveTeleportAckOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("MSG_MOVE_TELEPORT_ACK");

    ObjectGuid guid;
    recv_data >> guid;

    uint32 counter = 0;
    uint32 time = 0;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
    recv_data >> counter >> time;
#else
    recv_data >> time;
#endif
    DEBUG_LOG("Guid: %s", guid.GetString().c_str());
    DEBUG_LOG("Counter %u, time %u", counter, time / IN_MILLISECONDS);

    Unit *mover = _player->GetMover();
    Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    if (!plMover || !plMover->IsBeingTeleportedNear())
        return;

    if (guid != plMover->GetObjectGuid())
        return;

    plMover->SetSemaphoreTeleportNear(false);

    WorldLocation const& dest = plMover->GetTeleportDest();
    plMover->TeleportPositionRelocation(dest.coord_x, dest.coord_y, dest.coord_z, dest.orientation);

    // resummon pet, if the destination is in another continent instance, let Player::SwitchInstance do it
    // because the client will request the name for the old pet guid and receive no answer
    // result would be a pet named "unknown"
    if (plMover->GetTemporaryUnsummonedPetNumber())
        if (sWorld.getConfig(CONFIG_BOOL_CONTINENTS_INSTANCIATE) && plMover->GetMap()->IsContinent())
        {
            bool transition = false;
            if (sMapMgr.GetContinentInstanceId(plMover->GetMap()->GetId(), dest.coord_x, dest.coord_y, &transition) == plMover->GetInstanceId())
                plMover->ResummonPetTemporaryUnSummonedIfAny();
        }
        else
            plMover->ResummonPetTemporaryUnSummonedIfAny();

    //lets process all delayed operations on successful teleport
    plMover->ProcessDelayedOperations();

    // Si le joueur est stun, il ne pourra pas envoyer sa position -> Fix desynchro ici.
    if (plMover->hasUnitState(UNIT_STAT_NO_FREE_MOVE))
    {
        plMover->m_movementInfo.moveFlags &= ~MOVEFLAG_MASK_MOVING_OR_TURN;
        plMover->SendHeartBeat(false);
    }
}

void WorldSession::HandleMovementOpcodes(WorldPacket & recv_data)
{
    uint32 opcode = recv_data.GetOpcode();
    DEBUG_LOG("WORLD: Recvd %s (%u, 0x%X) opcode", LookupOpcodeName(opcode), opcode, opcode);

    Unit *mover = _player->GetMover();

    if (mover->GetObjectGuid() != _clientMoverGuid)
        return;
        
    Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if (plMover && plMover->IsBeingTeleported())
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    /* extract packet */
    MovementInfo movementInfo = plMover ? plMover->m_movementInfo : MovementInfo();
    recv_data >> movementInfo;
    movementInfo.UpdateTime(recv_data.GetPacketTime());

    /*----------------*/

    if (!VerifyMovementInfo(movementInfo))
        return;

    if (plMover && !plMover->GetCheatData()->HandleAnticheatTests(movementInfo, this, &recv_data))
        return;

    // this must be called after HandleAnticheatTests because that function will update order counters (for things like slow fall, water walk, etc.)
    if (plMover && !plMover->GetCheatData()->CheckTeleport(opcode, movementInfo))
        return;

    // Interrupt spell cast at move
    if (movementInfo.HasMovementFlag(MOVEFLAG_MASK_MOVING))
        mover->InterruptSpellsWithInterruptFlags(SPELL_INTERRUPT_FLAG_MOVEMENT);

    HandleMoverRelocation(movementInfo);

    // fall damage generation (ignore in flight case that can be triggered also at lags in moment teleportation to another map).
    if (opcode == MSG_MOVE_FALL_LAND && plMover && !plMover->IsTaxiFlying())
        plMover->HandleFall(movementInfo);

    // TODO: remove it
    // reset knockback state when fall to ground or water
    if (plMover)
    {
        if ((opcode == MSG_MOVE_FALL_LAND || opcode == MSG_MOVE_START_SWIM) && plMover->IsLaunched())
        {
            plMover->SetLaunched(false);
            plMover->SetXYSpeed(0.0f);
        }
    }

    if (plMover)
        plMover->UpdateFallInformationIfNeed(movementInfo, opcode);

    WorldPacket data(opcode, recv_data.size());

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
    data << _clientMoverGuid.WriteAsPacked();
#else
    data << _clientMoverGuid.GetRawValue();
#endif
    movementInfo.Write(data);

    mover->SendMovementMessageToSet(std::move(data), true, _player);

    // Fix movement issue on older clients where if the player jumps while running,
    // and then lets go of the key while in the air, he appears to continue moving
    // forward on other people's screen. Once he moves for real, they will see him
    // teleport back to where he was standing after he jumped.
#if SUPPORTED_CLIENT_BUILD == CLIENT_BUILD_1_9_4
    if (opcode == MSG_MOVE_FALL_LAND)
    {
        uint16 opcode2 = 0;
        if (!movementInfo.HasMovementFlag(MOVEFLAG_MASK_MOVING))
            opcode2 = MSG_MOVE_STOP;
        else if (movementInfo.HasMovementFlag(MOVEFLAG_BACKWARD))
            opcode2 = MSG_MOVE_START_BACKWARD;
        else if (movementInfo.HasMovementFlag(MOVEFLAG_FORWARD))
            opcode2 = MSG_MOVE_START_FORWARD;
        else if (movementInfo.HasMovementFlag(MOVEFLAG_STRAFE_LEFT))
            opcode2 = MSG_MOVE_START_STRAFE_LEFT;
        else if (movementInfo.HasMovementFlag(MOVEFLAG_STRAFE_RIGHT))
            opcode2 = MSG_MOVE_START_STRAFE_RIGHT;

        if (opcode2)
        {
            WorldPacket data(opcode2, recv_data.size());
            data << _clientMoverGuid.WriteAsPacked();             // write guid
            movementInfo.Write(data);                             // write data

            mover->SendMovementMessageToSet(std::move(data), true, _player);
        }
    }
#endif
}

void WorldSession::HandleForceSpeedChangeAckOpcodes(WorldPacket &recv_data)
{
    uint32 opcode = recv_data.GetOpcode();
    DEBUG_LOG("WORLD: Recvd %s (%u, 0x%X) opcode", LookupOpcodeName(opcode), opcode, opcode);

    /* extract packet */
    ObjectGuid guid;
    recv_data >> guid;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
    uint32 movementCounter;
    recv_data >> movementCounter;
#endif
    MovementInfo movementInfo;
    recv_data >> movementInfo;
    float  speedReceived;
    recv_data >> speedReceived;
    movementInfo.UpdateTime(recv_data.GetPacketTime());

    // now can skip not our packet
    if (guid != _clientMoverGuid && guid != _player->GetObjectGuid() && guid != _player->GetMover()->GetObjectGuid())
        return;

    if (!VerifyMovementInfo(movementInfo))
        return;

    // Process anticheat checks, remember client-side speed ...
    if (_player->IsSelfMover() && !_player->GetCheatData()->HandleSpeedChangeAck(movementInfo, this, &recv_data, speedReceived))
        return;

    /*----------------*/

    // client ACK send one packet for mounted/run case and need skip all except last from its
    // in other cases anti-cheat check can be fail in false case
    UnitMoveType move_type;
    switch (opcode)
    {
        case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:
            move_type = MOVE_WALK;
            break;
        case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:
            move_type = MOVE_RUN;
            break;
        case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:
            move_type = MOVE_RUN_BACK;
            break;
        case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:
            move_type = MOVE_SWIM;
            break;
        case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:
            move_type = MOVE_SWIM_BACK;
            break;
        case CMSG_FORCE_TURN_RATE_CHANGE_ACK:
            move_type = MOVE_TURN_RATE;
            break;
        default:
            sLog.outError("WorldSession::HandleForceSpeedChangeAck: Unknown move type opcode: %u", opcode);
            return;
    }

    Unit* mover = ObjectAccessor::GetUnit(*_player, guid);

    if (!mover)
        return;

    // verify that indeed the client is replying with the changes that were send to him
    if (!mover->HasPendingMovementChange())
    {
        sLog.outInfo("WorldSession::HandleForceSpeedChangeAck: Player %s from account id %u kicked because no movement change ack was expected from this player",
            _player->GetName(), _player->GetSession()->GetAccountId());
        _player->GetSession()->KickPlayer();
        return;
    }

    PlayerMovementPendingChange pendingChange = mover->PopPendingMovementChange();
    float speedSent = pendingChange.newValue;
    MovementChangeType changeType = pendingChange.movementChangeType;
    UnitMoveType moveTypeSent;
    switch (changeType)
    {
        case SPEED_CHANGE_WALK:                 moveTypeSent = MOVE_WALK; break;
        case SPEED_CHANGE_RUN:                  moveTypeSent = MOVE_RUN; break;
        case SPEED_CHANGE_RUN_BACK:             moveTypeSent = MOVE_RUN_BACK; break;
        case SPEED_CHANGE_SWIM:                 moveTypeSent = MOVE_SWIM; break;
        case SPEED_CHANGE_SWIM_BACK:            moveTypeSent = MOVE_SWIM_BACK; break;
        case RATE_CHANGE_TURN:                  moveTypeSent = MOVE_TURN_RATE; break;
        default:
            sLog.outInfo("WorldSession::HandleForceSpeedChangeAck: Player %s from account id %u kicked for incorrect data returned in an ack",
                _player->GetName(), _player->GetSession()->GetAccountId());
            _player->GetSession()->KickPlayer();
            return;
    }

#if SUPPORTED_CLIENT_BUILD <= CLIENT_BUILD_1_9_4
    uint32 movementCounter = pendingChange.movementCounter;
#endif

    if (pendingChange.movementCounter != movementCounter || std::fabs(speedSent - speedReceived) > 0.01f || moveTypeSent!= move_type)
    {
        sLog.outInfo("WorldSession::HandleForceSpeedChangeAck: Player %s from account id %u kicked for incorrect data returned in an ack",
            _player->GetName(), _player->GetSession()->GetAccountId());
        _player->GetSession()->KickPlayer();
        return;
    }

    // the client data has been verified. let's do the actual change now
    float newSpeedRate = speedSent / baseMoveSpeed[move_type];
    HandleMoverRelocation(movementInfo);
    _player->UpdateFallInformationIfNeed(movementInfo, opcode);
    mover->SetSpeedRateReal(move_type, newSpeedRate);

    MovementPacketSender::SendSpeedChangeToObservers(mover, move_type, speedSent);
}

/*
handles those packets:
CMSG_MOVE_WATER_WALK_ACK
CMSG_MOVE_HOVER_ACK
CMSG_MOVE_FEATHER_FALL_ACK
*/
void WorldSession::HandleMovementFlagChangeToggleAck(WorldPacket& recvData)
{
    /* extract packet */
    ObjectGuid guid;
    recvData >> guid;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
    uint32 movementCounter;
    recvData >> movementCounter;
#endif
    MovementInfo movementInfo;
    recvData >> movementInfo;
    movementInfo.UpdateTime(recvData.GetPacketTime());
    uint32 applyInt;
    recvData >> applyInt;
    bool applyReceived = applyInt == 0u ? false : true;

    // make sure this client is allowed to control the unit which guid is provided
    if (guid != _clientMoverGuid && guid != _player->GetObjectGuid() && guid != _player->GetMover()->GetObjectGuid())
        return;

    if (!VerifyMovementInfo(movementInfo))
        return;

    if (!_player->GetCheatData()->HandleAnticheatTests(movementInfo, this, &recvData))
        return;

    Unit* mover = ObjectAccessor::GetUnit(*_player, guid);

    if (!mover)
        return;

    // verify that indeed the client is replying with the changes that were send to him
    if (!mover->HasPendingMovementChange())
    {
        sLog.outInfo("WorldSession::HandleMovementFlagChangeToggleAck: Player %s from account id %u kicked because no movement change ack was expected from this player",
            _player->GetName(), _player->GetSession()->GetAccountId());
        _player->GetSession()->KickPlayer();
        return;
    }

    PlayerMovementPendingChange pendingChange = mover->PopPendingMovementChange();
    bool applySent = pendingChange.apply;
    MovementChangeType changeTypeSent = pendingChange.movementChangeType;

    MovementFlags mFlag = MOVEFLAG_NONE;
    MovementChangeType changeTypeReceived;
    switch (recvData.GetOpcode())
    {
        case CMSG_MOVE_WATER_WALK_ACK:      changeTypeReceived = WATER_WALK; mFlag = MOVEFLAG_WATERWALKING; break;
        case CMSG_MOVE_HOVER_ACK:           changeTypeReceived = SET_HOVER; mFlag = MOVEFLAG_HOVER; break;
        case CMSG_MOVE_FEATHER_FALL_ACK:    changeTypeReceived = FEATHER_FALL; mFlag = MOVEFLAG_SAFE_FALL; break;
        default:
            sLog.outInfo("WorldSession::HandleMovementFlagChangeToggleAck: Unknown move type opcode: %u", recvData.GetOpcode());
            return;
    }

#if SUPPORTED_CLIENT_BUILD <= CLIENT_BUILD_1_9_4
    uint32 movementCounter = pendingChange.movementCounter;
#endif

    if (pendingChange.movementCounter != movementCounter
        || applySent != applyReceived
        || changeTypeSent != changeTypeReceived)
    {
        sLog.outInfo("WorldSession::HandleMovementFlagChangeToggleAck: Player %s from account id %u kicked for incorrect data returned in an ack",
            _player->GetName(), _player->GetSession()->GetAccountId());
        _player->GetSession()->KickPlayer();
        return;
    }

    // Position change
    HandleMoverRelocation(movementInfo);
    _player->UpdateFallInformationIfNeed(movementInfo, recvData.GetOpcode());

    switch (changeTypeReceived)
    {
        case WATER_WALK:            mover->SetWaterWalkingReal(applyReceived); break;
        case SET_HOVER:             mover->SetHoverReal(applyReceived); break;
        case FEATHER_FALL:          mover->SetFeatherFallReal(applyReceived); break;
        default:
            sLog.outInfo("WorldSession::HandleMovementFlagChangeToggleAck: Unknown move type opcode: %u", recvData.GetOpcode());
            return;
    }

    if(mFlag != MOVEFLAG_NONE)
        MovementPacketSender::SendMovementFlagChangeToObservers(mover, mFlag, applySent);
}

/*
handles those packets:
CMSG_FORCE_MOVE_ROOT_ACK
CMSG_FORCE_MOVE_UNROOT_ACK
*/
void WorldSession::HandleMoveRootAck(WorldPacket& recv_data)
{
    DEBUG_LOG("HandleMoveRootAck");
    
    /* extract packet */
    ObjectGuid guid;
    recv_data >> guid;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
    uint32 movementCounter;
    recv_data >> movementCounter;
#endif
    MovementInfo movementInfo;
    recv_data >> movementInfo;
    movementInfo.UpdateTime(recv_data.GetPacketTime());

    // make sure this client is allowed to control the unit which guid is provided
    if (guid != _clientMoverGuid && guid != _player->GetObjectGuid() && guid != _player->GetMover()->GetObjectGuid())
        return;

    if (!VerifyMovementInfo(movementInfo))
        return;

    if (!_player->GetCheatData()->HandleAnticheatTests(movementInfo, this, &recv_data))
        return;

    Unit* mover = ObjectAccessor::GetUnit(*_player, guid);

    if (!mover)
        return;

    // verify that indeed the client is replying with the changes that were send to him
    if (!mover->HasPendingMovementChange())
    {
        sLog.outInfo("WorldSession::HandleMoveRootAck: Player %s from account id %u kicked because no movement change ack was expected from this player",
            _player->GetName(), _player->GetSession()->GetAccountId());
        _player->GetSession()->KickPlayer();
        return;
    }

    PlayerMovementPendingChange pendingChange = mover->PopPendingMovementChange();
    bool applySent = pendingChange.apply;
    MovementChangeType changeTypeSent = pendingChange.movementChangeType;

    MovementFlags mFlag = MOVEFLAG_ROOT;
    MovementChangeType changeTypeReceived = ROOT;
    bool applyReceived = (recv_data.GetOpcode() == CMSG_FORCE_MOVE_ROOT_ACK);

#if SUPPORTED_CLIENT_BUILD <= CLIENT_BUILD_1_9_4
    uint32 movementCounter = pendingChange.movementCounter;
#endif

    if (pendingChange.movementCounter != movementCounter 
        || applySent != applyReceived
        || changeTypeSent != changeTypeReceived)
    {
        sLog.outInfo("WorldSession::HandleMoveRootAck: Player %s from account id %u kicked for incorrect data returned in an ack",
            _player->GetName(), _player->GetSession()->GetAccountId());
        _player->GetSession()->KickPlayer();
        return;
    }

    // Update position if it has changed
    HandleMoverRelocation(movementInfo);
    _player->UpdateFallInformationIfNeed(movementInfo, recv_data.GetOpcode());
    mover->SetRootedReal(applyReceived);

    MovementPacketSender::SendMovementFlagChangeToObservers(mover, mFlag, applySent);

    // Set unit client state for brevity, though it should not be used
    if (applyReceived)
        mover->addUnitState(UNIT_STAT_CLIENT_ROOT);
    else
        mover->clearUnitState(UNIT_STAT_CLIENT_ROOT);
}

void WorldSession::HandleMoveKnockBackAck(WorldPacket & recv_data)
{
    DEBUG_LOG("CMSG_MOVE_KNOCK_BACK_ACK");

    ObjectGuid guid;
    recv_data >> guid;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
    uint32 movementCounter;
    recv_data >> movementCounter;
#endif
    MovementInfo movementInfo;
    recv_data >> movementInfo;
    movementInfo.UpdateTime(recv_data.GetPacketTime());

    Unit* mover = ObjectAccessor::GetUnit(*_player, guid);

    if (!mover)
        return;

    Player *pPlayerMover = mover->ToPlayer();

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if (pPlayerMover && pPlayerMover->IsBeingTeleported())
    {
        mover->PopPendingMovementChange();
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    if (guid != _clientMoverGuid && guid != _player->GetObjectGuid() && guid != _player->GetMover()->GetObjectGuid())
        return;

    if (!VerifyMovementInfo(movementInfo))
        return;

    if (!_player->GetCheatData()->HandleAnticheatTests(movementInfo, this, &recv_data))
        return;

    // verify that indeed the client is replying with the changes that were send to him
    if (!mover->HasPendingMovementChange())
    {
        sLog.outInfo("WorldSession::HandleMoveKnockBackAck: Player %s from account id %u kicked because no movement change ack was expected from this player",
            _player->GetName(), _player->GetSession()->GetAccountId());
        _player->GetSession()->KickPlayer();
        return;
    }

    PlayerMovementPendingChange pendingChange = mover->PopPendingMovementChange();

#if SUPPORTED_CLIENT_BUILD <= CLIENT_BUILD_1_9_4
    uint32 movementCounter = pendingChange.movementCounter;
#endif

    if (pendingChange.movementCounter != movementCounter || pendingChange.movementChangeType != KNOCK_BACK
        || std::fabs(pendingChange.knockbackInfo.speedXY - movementInfo.jump.xyspeed) > 0.01f
        || std::fabs(pendingChange.knockbackInfo.speedZ - movementInfo.jump.velocity) > 0.01f
        || std::fabs(pendingChange.knockbackInfo.vcos - movementInfo.jump.cosAngle) > 0.01f
        || std::fabs(pendingChange.knockbackInfo.vsin - movementInfo.jump.sinAngle) > 0.01f)
    {
        sLog.outInfo("WorldSession::HandleMoveKnockBackAck: Player %s from account id %u kicked for incorrect data returned in an ack",
            _player->GetName(), _player->GetSession()->GetAccountId());
        _player->GetSession()->KickPlayer();
        return;
    }

    HandleMoverRelocation(movementInfo);

    MovementPacketSender::SendKnockBackToObservers(mover, movementInfo.jump.cosAngle, movementInfo.jump.sinAngle, movementInfo.jump.xyspeed, movementInfo.jump.velocity);
}

void WorldSession::HandleMoveSplineDoneOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received CMSG_MOVE_SPLINE_DONE");

    MovementInfo movementInfo;                              // used only for proper packet read

    recv_data >> movementInfo;
    recv_data >> Unused<uint32>();                          // unk
    recv_data >> Unused<uint32>();                          // unk2

    // Forward packet to near players
    recv_data.SetOpcode(MSG_MOVE_STOP);
    recv_data.rpos(0);
    HandleMovementOpcodes(recv_data);
}

void WorldSession::HandleSetActiveMoverOpcode(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_SET_ACTIVE_MOVER");

    ObjectGuid guid;
    recv_data >> guid;

    ObjectGuid serverMoverGuid = _player->GetMover()->GetObjectGuid();

    // Before 1.10, client sends 0 as guid if it has no control.
#if SUPPORTED_CLIENT_BUILD <= CLIENT_BUILD_1_9_4
    if ((serverMoverGuid == _player->GetObjectGuid()) && !_player->HasSelfMovementControl())
        serverMoverGuid = ObjectGuid();
#endif

    if (serverMoverGuid != guid)
    {
        sLog.outError("HandleSetActiveMoverOpcode: incorrect mover guid: mover is %s and should be %s",
                      _player->GetMover()->GetGuidStr().c_str(), guid.GetString().c_str());
        _clientMoverGuid = _player->GetMover()->GetObjectGuid();
        return;
    }

    // mover swap after Eyes of the Beast, PetAI::UpdateAI handle the pet's return
    // Check if we actually have a pet before looking up
    if (_player->GetPetGuid() && _player->GetPetGuid() == _clientMoverGuid)
    {
        if (Pet* pet = _player->GetPet())
        {
            pet->clearUnitState(UNIT_STAT_POSSESSED);
            pet->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_POSSESSED);
            // out of range pet dismissed
            if (!pet->IsWithinDistInMap(_player, pet->GetMap()->GetGridActivationDistance()))
                _player->RemovePet(PET_SAVE_REAGENTS);
        }
    }

    _clientMoverGuid = guid;
}

void WorldSession::HandleMoveNotActiveMoverOpcode(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_MOVE_NOT_ACTIVE_MOVER");
    recv_data.hexlike();

    MovementInfo mi;

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
    ObjectGuid old_mover_guid;
    recv_data >> old_mover_guid;
    recv_data >> mi;
    _clientMoverGuid = ObjectGuid();

    // Client sent not active mover, but maybe the mover is actually set?
    if (_player->GetMover() && _player->GetMover()->GetObjectGuid() == old_mover_guid)
    {
        DETAIL_LOG("HandleMoveNotActiveMover: incorrect mover guid: mover is %s and should be %s instead of %s",
                       _player->GetMover()->GetGuidStr().c_str(),
                       _player->GetGuidStr().c_str(),
                       old_mover_guid.GetString().c_str());
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }
#else
    recv_data >> mi;
    _clientMoverGuid = ObjectGuid();
#endif

    // Prevent client from removing root flag.
    if (_player->HasUnitMovementFlag(MOVEFLAG_ROOT) && !mi.HasMovementFlag(MOVEFLAG_ROOT))
        mi.AddMovementFlag(MOVEFLAG_ROOT);

    _player->m_movementInfo = mi;
}

void WorldSession::HandleMountSpecialAnimOpcode(WorldPacket& /*recvdata*/)
{
    //DEBUG_LOG("WORLD: Recvd CMSG_MOUNTSPECIAL_ANIM");

    WorldPacket data(SMSG_MOUNTSPECIAL_ANIM, 8);
    data << GetPlayer()->GetObjectGuid();

    GetPlayer()->SendMovementMessageToSet(std::move(data), false);
}

void WorldSession::HandleSummonResponseOpcode(WorldPacket& recv_data)
{
    if (!_player->isAlive() || _player->isInCombat())
        return;

    ObjectGuid summonerGuid;
    recv_data >> summonerGuid;

    _player->SummonIfPossible(true);
}

void WorldSession::HandleMoveTimeSkippedOpcode(WorldPacket & recv_data)
{
    DEBUG_LOG("WORLD: Time Lag/Synchronization Resent/Update");

    ObjectGuid g;
    recv_data >> g;
    uint32 lag;
    recv_data >> lag;

    Player* pl = GetPlayer();

    pl->m_movementInfo.time += lag;
    pl->m_movementInfo.ctime += lag;

    // fix an 1.12 client problem with transports
    Transport* tr = pl->GetTransport();
    if (pl->HasJustBoarded() && tr)
    {
        pl->SetJustBoarded(false);
        tr->SendOutOfRangeUpdateToPlayer(pl);
        tr->SendCreateUpdateToPlayer(pl);
    }
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
    else
    {
        WorldPacket data(MSG_MOVE_TIME_SKIPPED, 12);
        data << pl->GetPackGUID();
        data << lag;
        pl->SendMovementMessageToSet(std::move(data), false);
    }
#endif
}

bool WorldSession::VerifyMovementInfo(MovementInfo const& movementInfo, ObjectGuid const& guid) const
{
    // ignore wrong guid (player attempt cheating own session for not own guid possible...)
    if (guid != _player->GetMover()->GetObjectGuid())
        return false;

    return VerifyMovementInfo(movementInfo);
}

bool WorldSession::VerifyMovementInfo(MovementInfo const& movementInfo) const
{
    if (!MaNGOS::IsValidMapCoord(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o))
        return false;

    if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
    {
        // transports size limited
        // (also received at zeppelin/lift leave by some reason with t_* as absolute in continent coordinates, can be safely skipped)
        if (std::fabs(movementInfo.GetTransportPos()->x) > 250 || std::fabs(movementInfo.GetTransportPos()->y) > 250 || std::fabs(movementInfo.GetTransportPos()->z) > 100)
            return false;

        if (!MaNGOS::IsValidMapCoord(movementInfo.GetPos()->x + movementInfo.GetTransportPos()->x, movementInfo.GetPos()->y + movementInfo.GetTransportPos()->y,
                                     movementInfo.GetPos()->z + movementInfo.GetTransportPos()->z, movementInfo.GetPos()->o + movementInfo.GetTransportPos()->o))
            return false;
    }

    return true;
}

void WorldSession::HandleMoverRelocation(MovementInfo& movementInfo)
{
    Unit *mover = _player->GetMover();
    movementInfo.CorrectData(mover);

    // Prevent client from removing root flag.
    if (mover->HasUnitMovementFlag(MOVEFLAG_ROOT) && !movementInfo.HasMovementFlag(MOVEFLAG_ROOT))
        movementInfo.AddMovementFlag(MOVEFLAG_ROOT);

    if (Player* plMover = mover->ToPlayer())
    {
        // ignore current relocation if needed
        if (plMover->IsNextRelocationIgnored())
        {
            plMover->DoIgnoreRelocation();
            return;
        }

        if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
        {
            GetPlayer()->GetCheatData()->OnTransport(plMover, movementInfo.GetTransportGuid());
            Unit* loadPetOnTransport = nullptr;
            if (!plMover->GetTransport())
            {
                if (Transport* t = plMover->GetMap()->GetTransport(movementInfo.GetTransportGuid()))
                {
                    t->AddPassenger(plMover);
                    if (Pet* pet = plMover->GetPet())
                        if (pet->GetTransport() != t)
                            loadPetOnTransport = pet;
                }
                // fix an 1.12 client problem with transports
                plMover->SetJustBoarded(true);
            }
            else
                plMover->SetJustBoarded(false);
            if (plMover->GetTransport())
            {
                movementInfo.pos.x = movementInfo.GetTransportPos()->x;
                movementInfo.pos.y = movementInfo.GetTransportPos()->y;
                movementInfo.pos.z = movementInfo.GetTransportPos()->z;
                movementInfo.pos.o = movementInfo.GetTransportPos()->o;
                plMover->GetTransport()->CalculatePassengerPosition(movementInfo.pos.x, movementInfo.pos.y, movementInfo.pos.z, &movementInfo.pos.o);
                if (loadPetOnTransport)
                {
                    loadPetOnTransport->NearTeleportTo(movementInfo.pos.x, movementInfo.pos.y, movementInfo.pos.z, movementInfo.pos.o);
                    plMover->GetTransport()->AddPassenger(loadPetOnTransport);
                }
            }
        }
        else if (plMover->GetTransport())
        {
            plMover->GetTransport()->RemovePassenger(plMover);
            if (Pet* pet = plMover->GetPet())
            {
                // If moving on transport, stop it.
                pet->DisableSpline();
                if (pet->GetTransport())
                {
                    pet->GetTransport()->RemovePassenger(pet);
                    pet->NearTeleportTo(movementInfo.pos.x, movementInfo.pos.y, movementInfo.pos.z, movementInfo.pos.o);
                }
            }
        }

        if (movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) != plMover->IsInWater())
        {
            // now client not include swimming flag in case jumping under water
            plMover->SetInWater(!plMover->IsInWater() || plMover->GetTerrain()->IsUnderWater(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z));
        }

        plMover->SetPosition(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o);
        plMover->m_movementInfo = movementInfo;

        // super smart decision; rework required
        if (ObjectGuid lootGuid = plMover->GetLootGuid() && !lootGuid.IsItem())
            plMover->SendLootRelease(lootGuid);

        // Nostalrius - antiundermap1
        if (movementInfo.HasMovementFlag(MOVEFLAG_FALLINGFAR))
        {
            float hauteur = plMover->GetMap()->GetHeight(plMover->GetPositionX(), plMover->GetPositionY(), plMover->GetPositionZ(), true);
            bool undermap = false;
            // Undermap
            if ((plMover->GetPositionZ() + 100.0f) < hauteur)
                undermap = true;
            if (plMover->GetPositionZ() < 250.0f && plMover->GetMapId() == 489)
                undermap = true;

            if (undermap)
                if (plMover->UndermapRecall())
                    sLog.outInfo("[UNDERMAP] %s [GUID %u]. MapId:%u %f %f %f", plMover->GetName(), plMover->GetGUIDLow(), plMover->GetMapId(), plMover->GetPositionX(), plMover->GetPositionY(), plMover->GetPositionZ());
        }
        else if (plMover->CanFreeMove())
            plMover->SaveNoUndermapPosition(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z + 3.0f);
        // Antiundermap2: teleportation au cimetiere
        if (movementInfo.GetPos()->z < -500.0f)
        {
            // NOTE: this is actually called many times while falling
            // even after the player has been teleported away
            // TODO: discard movement packets after the player is rooted
            if (plMover->isAlive())
            {
                // Nostalrius : pas mort quand on chute
                if (plMover->InBattleGround())
                    plMover->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, plMover->GetHealth());
                else
                    plMover->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, plMover->GetHealth() / 2);
                // pl can be alive if GM/etc
                if (!plMover->isAlive())
                {
                    // change the death state to CORPSE to prevent the death timer from
                    // starting in the next player update
                    plMover->KillPlayer();
                    plMover->BuildPlayerRepop();
                }
            }

            // cancel the death timer here if started
            sLog.outInfo("[UNDERMAP/Teleport] Player %s teleported.", plMover->GetName(), plMover->GetGUIDLow(), plMover->GetMapId(), plMover->GetPositionX(), plMover->GetPositionY(), plMover->GetPositionZ());
            plMover->RepopAtGraveyard();
        }
    }
    else                                                    // creature charmed
    {
        if (mover->IsInWorld())
            mover->GetMap()->CreatureRelocation((Creature*)mover, movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o);
    }
}

