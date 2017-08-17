#include <Server_Common/Common.h>
#include <Server_Common/CommonNetwork.h>
#include <Server_Common/Database.h>
#include <Server_Common/GamePacketNew.h>
#include <Server_Common/Logger.h>
#include <Server_Common/ExdData.h>
#include <Server_Common/PacketContainer.h>

#include <boost/format.hpp>


#include "GameConnection.h"

#include "Session.h"
#include "Zone.h"
#include "ZonePosition.h"
#include "ServerZone.h"
#include "ZoneMgr.h"

#include "InitUIPacket.h"
#include "PingPacket.h"
#include "MoveActorPacket.h"
#include "ChatPacket.h"
#include "ServerNoticePacket.h"
#include "ActorControlPacket142.h"
#include "ActorControlPacket143.h"
#include "ActorControlPacket144.h"
#include "EventStartPacket.h"
#include "EventFinishPacket.h"
#include "PlayerStateFlagsPacket.h"


#include "GameCommandHandler.h"

#include "Player.h"
#include "Inventory.h"

#include "Forwards.h"

#include "EventHelper.h"

#include "Action.h"
#include "ActionTeleport.h"

extern Core::Logger g_log;
extern Core::Db::Database g_database;
extern Core::ServerZone g_serverZone;
extern Core::ZoneMgr g_zoneMgr;
extern Core::Data::ExdData g_exdData;
extern Core::GameCommandHandler g_gameCommandMgr;

using namespace Core::Common;
using namespace Core::Network::Packets;
using namespace Core::Network::Packets::Server;


void Core::Network::GameConnection::actionHandler( Core::Network::Packets::GamePacketPtr pInPacket,
                                                   Core::Entity::PlayerPtr pPlayer )
{
    uint16_t commandId = pInPacket->getValAt< uint16_t >( 0x20 );
    uint64_t param1 = pInPacket->getValAt< uint64_t >( 0x24 );
    uint32_t param11 = pInPacket->getValAt< uint32_t >( 0x24 );
    uint32_t param12 = pInPacket->getValAt< uint32_t >( 0x28 );
    uint32_t param2 = pInPacket->getValAt< uint32_t >( 0x2c );
    uint64_t param3 = pInPacket->getValAt< uint64_t >( 0x38 );

    g_log.debug( "[" + std::to_string( m_pSession->getId() ) + "] Incoming action: " +
                 boost::str( boost::format( "%|04X|" ) % ( uint32_t ) ( commandId & 0xFFFF ) ) +
                 "\nparam1: " + boost::str( boost::format( "%|016X|" ) % ( uint64_t ) ( param1 & 0xFFFFFFFFFFFFFFF ) ) +
                 "\nparam2: " + boost::str( boost::format( "%|08X|" ) % ( uint32_t ) ( param2 & 0xFFFFFFFF ) ) +
                 "\nparam3: " + boost::str( boost::format( "%|016X|" ) % ( uint64_t ) ( param3 & 0xFFFFFFFFFFFFFFF ) )
    );


    //g_log.Log(LoggingSeverity::debug, "[" + std::to_string(m_pSession->getId()) + "] " + pInPacket->toString());

    switch( commandId )
    {
        case 0x01:  // Toggle sheathe
        {
            if ( param11 == 1 )
                pPlayer->setStance( Entity::Actor::Stance::Active );
            else
            {
                pPlayer->setStance( Entity::Actor::Stance::Passive );
                pPlayer->setAutoattack( false );
            }

            pPlayer->sendToInRangeSet( ActorControlPacket142( pPlayer->getId(), 0, param11, 1 ) );

            break;
        }
        case 0x02:  // Toggle auto-attack
        {
            if ( param11 == 1 )
            {
                pPlayer->setAutoattack( true );
                pPlayer->setStance( Entity::Actor::Stance::Active );
            }
            else
                pPlayer->setAutoattack( false );

            pPlayer->sendToInRangeSet( ActorControlPacket142( pPlayer->getId(), 1, param11, 1 ) );

            break;
        }
        case 0x03: // Change target
        {

            uint64_t targetId = pInPacket->getValAt< uint64_t >( 0x24 );
            pPlayer->changeTarget( targetId );
            break;
        }

        case 0x133: // Update howtos seen
        {
            uint32_t howToId = static_cast< uint32_t >( param1 );
            pPlayer->updateHowtosSeen( howToId );
            break;
        }
        case 0x1F4: // emote
        {
            uint64_t targetId = pPlayer->getTargetId();
            uint32_t emoteId = pInPacket->getValAt< uint32_t >( 0x24 );

            pPlayer->sendToInRangeSet( ActorControlPacket144( pPlayer->getId(), Emote, emoteId, 0, 0, 0, targetId ) );
            break;
        }
        case 0xC8: // return dead
        {
            pPlayer->returnToHomepoint();
            break;
        }
        case 0xC9: // Finish zoning
        {
            switch( pPlayer->getZoningType() )
            {
                case ZoneingType::None:
                    pPlayer->sendToInRangeSet( ActorControlPacket143( pPlayer->getId(), ZoneIn, 0x01 ), true );
                    break;
                case ZoneingType::Teleport:
                    pPlayer->sendToInRangeSet( ActorControlPacket143( pPlayer->getId(), ZoneIn, 0x01, 0, 0, 110 ), true );
                    break;
                case ZoneingType::Return:
                case ZoneingType::ReturnDead:
                {
                    if( pPlayer->getStatus() == Entity::Actor::ActorStatus::Dead )
                    {
                        pPlayer->resetHp();
                        pPlayer->resetMp();
                        pPlayer->setStatus( Entity::Actor::ActorStatus::Idle );
                        pPlayer->setSyncFlag( Status );
                        pPlayer->sendToInRangeSet( ActorControlPacket143( pPlayer->getId(), ZoneIn, 0x01, 0x01, 0, 111 ), true );
                        pPlayer->sendToInRangeSet( ActorControlPacket142( pPlayer->getId(), SetStatus, static_cast< uint8_t >( Entity::Actor::ActorStatus::Idle ) ), true );
                    }
                    else
                        pPlayer->sendToInRangeSet( ActorControlPacket143( pPlayer->getId(), ZoneIn, 0x01, 0x00, 0, 111 ), true );
                }
                    break;
                case ZoneingType::FadeIn:
                    break;
                default:
                    break;
            }

            pPlayer->setZoningType( Common::ZoneingType::None );

            pPlayer->unsetStateFlag( PlayerStateFlag::BetweenAreas );
            pPlayer->unsetStateFlag( PlayerStateFlag::BetweenAreas1 );
            pPlayer->sendStateFlags();
            break;
        }

        case 0xCA: // Teleport
        {
            // TODO: only register this action if enough gil is in possession
            auto targetAetheryte = g_exdData.getAetheryteInfo( param11 );

            if( targetAetheryte )
            {
                auto fromAetheryte = g_exdData.getAetheryteInfo( g_exdData.m_zoneInfoMap[pPlayer->getZoneId()].aetheryte_index );

                // calculate cost - does not apply for favorite points or homepoints neither checks for aether tickets
                auto cost = ( sqrt( pow( fromAetheryte->map_coord_x - targetAetheryte->map_coord_x, 2 ) +
                                    pow( fromAetheryte->map_coord_y - targetAetheryte->map_coord_y, 2 ) ) / 2 ) + 100;

                // cap at 999 gil
                cost = cost > 999 ? 999 : cost;

                bool insufficientGil = pPlayer->getCurrency( Inventory::CurrencyType::Gil ) < cost;
                // todo: figure out what param1 really does
                pPlayer->queuePacket( ActorControlPacket143( pPlayer->getId(), TeleportStart, insufficientGil ? 2 : 0, param11 ) );

                if( !insufficientGil )
                {
                    Action::ActionTeleportPtr pActionTeleport( new Action::ActionTeleport( pPlayer, param11, cost ) );
                    pPlayer->setCurrentAction( pActionTeleport );
                }
            }
            break;
        }

    }
}