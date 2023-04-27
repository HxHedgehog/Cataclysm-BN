#include "veh_utils.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "calendar.h"
#include "craft_command.h"
#include "crafting.h"
#include "map.h"
#include "player.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_range.h"
#include "character.h"
#include "enums.h"
#include "game_constants.h"
#include "inventory.h"
#include "item.h"
#include "requirements.h"
#include "translations.h"
#include "color.h"
#include "point.h"

namespace veh_utils
{

int calc_xp_gain( const vpart_info &vp, const skill_id &sk, const Character &who )
{
    const auto iter = vp.install_skills.find( sk );
    if( iter == vp.install_skills.end() ) {
        return 0;
    }

    // how many levels are we above the requirement?
    const int lvl = std::max( who.get_skill_level( sk ) - iter->second, 1 );

    // scale xp gain per hour according to relative level
    // 0-1: 60 xp /h
    //   2: 15 xp /h
    //   3:  6 xp /h
    //   4:  4 xp /h
    //   5:  3 xp /h
    //   6:  2 xp /h
    //  7+:  1 xp /h
    return std::ceil( static_cast<double>( vp.install_moves ) /
                      to_moves<int>( 1_minutes * std::pow( lvl, 2 ) ) );
}

vehicle_part &most_repairable_part( vehicle &veh, Character &who, bool only_repairable )
{
    const auto &inv = who.crafting_inventory();

    enum class repairable_status {
        not_repairable = 0,
        need_replacement,
        repairable
    };
    std::map<const vehicle_part *, repairable_status> repairable_cache;
    for( const vpart_reference &vpr : veh.get_all_parts() ) {
        const auto &info = vpr.info();
        repairable_cache[ &vpr.part() ] = repairable_status::not_repairable;
        if( vpr.part().removed || vpr.part().damage() <= 0 ) {
            continue;
        }

        if( vpr.part().is_broken() ) {
            if( info.install_requirements().can_make_with_inventory( inv, is_crafting_component ) ) {
                repairable_cache[ &vpr.part()] = repairable_status::need_replacement;
            }

            continue;
        }

        if( info.is_repairable() &&
            ( info.repair_requirements() * vpr.part().damage_level( 4 ) ).can_make_with_inventory( inv,
                    is_crafting_component ) ) {
            repairable_cache[ &vpr.part()] = repairable_status::repairable;
        }
    }

    const auto part_damage_comparison = [&repairable_cache]( const vpart_reference & a,
    const vpart_reference & b ) {
        return ( repairable_cache[ &b.part() ] > repairable_cache[ &a.part()] ) ||
               ( repairable_cache[ &b.part()] == repairable_cache[ &a.part()] &&
                 b.part().damage() > a.part().damage() );
    };
    const vehicle_part_range vp_range = veh.get_all_parts();
    const auto high_damage_iterator = std::max_element( vp_range.begin(),
                                      vp_range.end(),
                                      part_damage_comparison );
    if( high_damage_iterator == vp_range.end() ||
        high_damage_iterator->part().removed ||
        !high_damage_iterator->info().is_repairable() ||
        ( only_repairable &&
          repairable_cache[ &( *high_damage_iterator ).part() ] != repairable_status::not_repairable ) ) {
        static vehicle_part nullpart;
        return nullpart;
    }

    return high_damage_iterator->part();
}

bool repair_part( vehicle &veh, vehicle_part &pt, Character &who_c )
{
    // TODO: Get rid of this cast after moving relevant functions down to Character
    player &who = static_cast<player &>( who_c );
    int part_index = veh.index_of_part( &pt );
    auto &vp = pt.info();

    // TODO: Expose base part damage somewhere, don't recalculate it here
    const auto reqs = pt.is_broken() ?
                      vp.install_requirements() :
                      vp.repair_requirements() * pt.damage_level( 4 );

    const inventory &inv = who.crafting_inventory( who.pos(), PICKUP_RANGE, !who.is_npc() );
    inventory map_inv;
    // allow NPCs to use welding rigs they can't see ( on the other side of a vehicle )
    // as they have the handicap of not being able to use the veh interaction menu
    // or able to drag a welding cart etc.
    map_inv.form_from_map( who.pos(), PICKUP_RANGE, &who_c, false, !who.is_npc() );
    if( !reqs.can_make_with_inventory( inv, is_crafting_component ) ) {
        who.add_msg_if_player( m_info, _( "You don't meet the requirements to repair the %s." ),
                               pt.name() );
        return false;
    }

    // consume items extracting any base item (which we will need if replacing broken part)
    //TODO!: CHECK
    item *base = item::spawn( vp.item );
    for( const auto &e : reqs.get_components() ) {
        for( auto &obj : who.consume_items( who.select_item_component( e, 1, map_inv ), 1,
                                            is_crafting_component ) ) {
            if( obj->typeId() == vp.item ) {
                base = obj;
            }
        }
    }

    for( const auto &e : reqs.get_tools() ) {
        who.consume_tools( crafting::select_tool_component( e, 1, map_inv, &who ), 1 );
    }

    who.invalidate_crafting_inventory();

    for( const auto &sk : pt.is_broken() ? vp.install_skills : vp.repair_skills ) {
        who.practice( sk.first, calc_xp_gain( vp, sk.first, who ) );
    }

    // If part is broken, it will be destroyed and references invalidated
    std::string partname = pt.name( false );
    const std::string startdurability = colorize( pt.get_base().damage_symbol(),
                                        pt.get_base().damage_color() );
    bool wasbroken = pt.is_broken();
    if( wasbroken ) {
        const units::angle dir = pt.direction;
        point loc = pt.mount;
        auto replacement_id = pt.info().get_id();
        get_map().spawn_items( who.pos(), pt.pieces_for_broken_part() );
        veh.remove_part( part_index );
        const int partnum = veh.install_part( loc, replacement_id, std::move( base ) );
        veh.part( partnum ).direction = dir;
        veh.part_removal_cleanup();
    } else {
        veh.set_hp( pt, pt.info().durability );
    }

    // TODO: NPC doing that
    who.add_msg_if_player( m_good,
                           wasbroken ? _( "You replace the %1$s's %2$s. (was %3$s)" ) :
                           _( "You repair the %1$s's %2$s. (was %3$s)" ), veh.name,
                           partname, startdurability );
    return true;
}

} // namespace veh_utils
