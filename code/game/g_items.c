/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
//
#include "g_local.h"

/*

  Items are any object that a player can touch to gain some effect.

  Pickup will return the number of seconds until they should respawn.

  all items should pop when dropped in lava or slime

  Respawnable items don't actually go away when picked up, they are
  just made invisible and untouchable.  This allows them to ride
  movers and respawn apropriately.
*/


#define RESPAWN_ARMOR		25
#define RESPAWN_HEALTH		35
#define RESPAWN_AMMO		40
#define RESPAWN_HOLDABLE	60
#define RESPAWN_POWERUP		120


static int Pickup_Powerup( const gentity_t* pu, gentity_t* player )
{
	int i;
	int quantity = (pu->count ? pu->count : pu->item->quantity);

	// round timing to seconds to make multiple powerup timers count in sync
	if ( !player->client->ps.powerups[pu->item->giTag] ) {
		player->client->ps.powerups[pu->item->giTag] = level.time - ( level.time % 1000 );
	}

	player->client->ps.powerups[pu->item->giTag] += quantity * 1000;

	// give any nearby players a "denied" anti-reward
	for ( i = 0 ; i < level.maxclients ; i++ ) {
		vec3_t		delta;
		vec3_t		forward;
		trace_t		tr;
		gclient_t* client = &level.clients[i];

		if ( client == player->client ) {
			continue;
		}
		if ( client->pers.connected == CON_DISCONNECTED ) {
			continue;
		}
		if ( client->ps.stats[STAT_HEALTH] <= 0 ) {
			continue;
		}

		// if same team in team game, no sound
		// cannot use OnSameTeam as it expects two g_entities, not clients
		if ( g_gametype.integer >= GT_TEAM && player->client->sess.sessionTeam == client->sess.sessionTeam ) {
			continue;
		}

		// if too far away, no sound
		VectorSubtract( pu->s.pos.trBase, client->ps.origin, delta );
		if ( VectorNormalize( delta ) > 192 ) {
			continue;
		}

		// if not facing, no sound
		AngleVectors( client->ps.viewangles, forward, NULL, NULL );
		if ( DotProduct( delta, forward ) < 0.4 ) {
			continue;
		}

		// if not line of sight, no sound
		trap_Trace( &tr, client->ps.origin, NULL, NULL, pu->s.pos.trBase, ENTITYNUM_NONE, CONTENTS_SOLID );
		if ( tr.fraction != 1.0 ) {
			continue;
		}

		// anti-reward
		client->ps.persistant[PERS_PLAYEREVENTS] ^= PLAYEREVENT_DENIEDREWARD;
	}

	return RESPAWN_POWERUP;
}


static int Pickup_Holdable( const gentity_t* ent, gentity_t* player )
{
	player->client->ps.stats[STAT_HOLDABLE_ITEM] = ent->item - bg_itemlist;

#ifdef MISSIONPACK
	if( ent->item->giTag == HI_KAMIKAZE ) {
		player->client->ps.eFlags |= EF_KAMIKAZE;
	}
#endif

	return RESPAWN_HOLDABLE;
}


///////////////////////////////////////////////////////////////


#ifdef MISSIONPACK

static int Pickup_PersistantPowerup( gentity_t *ent, gentity_t *other ) {
	int		clientNum;
	char	userinfo[MAX_INFO_STRING];
	float	handicap;
	int		max;

	other->client->ps.stats[STAT_PERSISTANT_POWERUP] = ent->item - bg_itemlist;
	other->client->persistantPowerup = ent;

	switch( ent->item->giTag ) {
	case PW_GUARD:
		clientNum = other->client->ps.clientNum;
		trap_GetUserinfo( clientNum, userinfo, sizeof(userinfo) );
		handicap = atof( Info_ValueForKey( userinfo, "handicap" ) );
		if( handicap<=0.0f || handicap>100.0f) {
			handicap = 100.0f;
		}
		max = (int)(2 *  handicap);

		other->health = max;
		other->client->ps.stats[STAT_HEALTH] = max;
		other->client->ps.stats[STAT_MAX_HEALTH] = max;
		other->client->ps.stats[STAT_ARMOR] = max;
		other->client->pers.maxHealth = max;

		break;

	case PW_SCOUT:
		clientNum = other->client->ps.clientNum;
		trap_GetUserinfo( clientNum, userinfo, sizeof(userinfo) );
		handicap = atof( Info_ValueForKey( userinfo, "handicap" ) );
		if( handicap<=0.0f || handicap>100.0f) {
			handicap = 100.0f;
		}
		other->client->pers.maxHealth = handicap;
		other->client->ps.stats[STAT_ARMOR] = 0;
		break;

	case PW_DOUBLER:
		clientNum = other->client->ps.clientNum;
		trap_GetUserinfo( clientNum, userinfo, sizeof(userinfo) );
		handicap = atof( Info_ValueForKey( userinfo, "handicap" ) );
		if( handicap<=0.0f || handicap>100.0f) {
			handicap = 100.0f;
		}
		other->client->pers.maxHealth = handicap;
		break;
	case PW_AMMOREGEN:
		clientNum = other->client->ps.clientNum;
		trap_GetUserinfo( clientNum, userinfo, sizeof(userinfo) );
		handicap = atof( Info_ValueForKey( userinfo, "handicap" ) );
		if( handicap<=0.0f || handicap>100.0f) {
			handicap = 100.0f;
		}
		other->client->pers.maxHealth = handicap;
		memset(other->client->ammoTimes, 0, sizeof(other->client->ammoTimes));
		break;
	default:
		clientNum = other->client->ps.clientNum;
		trap_GetUserinfo( clientNum, userinfo, sizeof(userinfo) );
		handicap = atof( Info_ValueForKey( userinfo, "handicap" ) );
		if( handicap<=0.0f || handicap>100.0f) {
			handicap = 100.0f;
		}
		other->client->pers.maxHealth = handicap;
		break;
	}

	return -1;
}

#endif


///////////////////////////////////////////////////////////////


static void Add_Ammo( gentity_t* ent, int weapon, int count )
{
	ent->client->ps.ammo[weapon] += count;
	if ( ent->client->ps.ammo[weapon] > 200 ) {
		ent->client->ps.ammo[weapon] = 200;
	}
}


static int Pickup_Ammo( const gentity_t* item, gentity_t* player )
{
	int quantity;

	if ( item->count ) {
		quantity = item->count;
	} else {
		quantity = item->item->quantity;
	}

	Add_Ammo( player, item->item->giTag, quantity );

	return RESPAWN_AMMO;
}


///////////////////////////////////////////////////////////////


static int Pickup_Weapon( const gentity_t* ent, gentity_t* player )
{
	int quantity = 0;

	if ( ent->count >= 0 ) {
		if ( ent->count ) {
			quantity = ent->count;
		} else {
			quantity = ent->item->quantity;
		}

		// dropped items and teamplay weapons always have full ammo
		if ( !(ent->flags & FL_DROPPED_ITEM) && g_gametype.integer != GT_TEAM ) {
			// drop the quantity if the already have over the minimum
			if ( player->client->ps.ammo[ ent->item->giTag ] < quantity ) {
				quantity = quantity - player->client->ps.ammo[ ent->item->giTag ];
			} else {
				quantity = 1;		// only add a single shot
			}
		}
	}

	// add the weapon
	player->client->ps.stats[STAT_WEAPONS] |= ( 1 << ent->item->giTag );

	Add_Ammo( player, ent->item->giTag, quantity );

	if (ent->item->giTag == WP_GRAPPLING_HOOK)
		player->client->ps.ammo[ent->item->giTag] = -1; // unlimited ammo

	// team deathmatch has slow weapon respawns
	if ( g_gametype.integer == GT_TEAM ) {
		return g_weaponTeamRespawn.integer;
	}

	return g_weaponRespawn.integer;
}


///////////////////////////////////////////////////////////////


static int Pickup_Health( const gentity_t* ent, gentity_t* player )
{
	int max, quantity;

	// small and mega healths will go over the max
#ifdef MISSIONPACK
	if( player->client && bg_itemlist[player->client->ps.stats[STAT_PERSISTANT_POWERUP]].giTag == PW_GUARD ) {
		max = player->client->ps.stats[STAT_MAX_HEALTH];
	}
	else
#endif
	if ( ent->item->quantity != 5 && ent->item->quantity != 100 ) {
		max = player->client->ps.stats[STAT_MAX_HEALTH];
	} else {
		max = player->client->ps.stats[STAT_MAX_HEALTH] * 2;
	}

	if ( ent->count ) {
		quantity = ent->count;
	} else {
		quantity = ent->item->quantity;
	}

	player->health += quantity;

	if (player->health > max ) {
		player->health = max;
	}

	player->client->ps.stats[STAT_HEALTH] = player->health;

	return RESPAWN_HEALTH;
}


///////////////////////////////////////////////////////////////


static int Pickup_Armor( const gentity_t* ent, gentity_t* player )
{
#ifdef MISSIONPACK
	int		upperBound;

	other->client->ps.stats[STAT_ARMOR] += ent->item->quantity;

	if( other->client && bg_itemlist[other->client->ps.stats[STAT_PERSISTANT_POWERUP]].giTag == PW_GUARD ) {
		upperBound = other->client->ps.stats[STAT_MAX_HEALTH];
	}
	else {
		upperBound = other->client->ps.stats[STAT_MAX_HEALTH] * 2;
	}

	if ( other->client->ps.stats[STAT_ARMOR] > upperBound ) {
		other->client->ps.stats[STAT_ARMOR] = upperBound;
	}
#else
	player->client->ps.stats[STAT_ARMOR] += ent->item->quantity;
	if ( player->client->ps.stats[STAT_ARMOR] > player->client->ps.stats[STAT_MAX_HEALTH] * 2 ) {
		player->client->ps.stats[STAT_ARMOR] = player->client->ps.stats[STAT_MAX_HEALTH] * 2;
	}
#endif

	return RESPAWN_ARMOR;
}


///////////////////////////////////////////////////////////////


void RespawnItem( gentity_t* ent )
{
	// randomly select from teamed entities
	if (ent->team) {
		gentity_t* master = ent->teammaster;
		int count, choice;

		if ( !master ) {
			G_Error( "RespawnItem: bad teammaster");
		}

		for (count = 0, ent = master; ent; ent = ent->teamchain, count++)
			;

		choice = rand() % count;

		for (count = 0, ent = master; count < choice; ent = ent->teamchain, count++)
			;
	}

	ent->r.contents = CONTENTS_TRIGGER;
	ent->s.eFlags &= ~EF_NODRAW;
	ent->r.svFlags &= ~SVF_NOCLIENT;
	trap_LinkEntity (ent);

	if ( ent->item->giType == IT_POWERUP ) {
		// play powerup spawn sound to all clients
		gentity_t* te = G_TempEntity( ent->s.pos.trBase, EV_GLOBAL_SOUND );
		te->s.eventParm = G_SoundIndex( "sound/items/poweruprespawn.wav" );
		te->r.svFlags |= SVF_BROADCAST;
	}

#ifdef MISSIONPACK
	else if ( ent->item->giType == IT_HOLDABLE && ent->item->giTag == HI_KAMIKAZE ) {
		gentity_t* te = G_TempEntity( ent->s.pos.trBase, ent->speed ? EV_GENERAL_SOUND : EV_GLOBAL_SOUND );
		te->s.eventParm = G_SoundIndex( "sound/items/kamikazerespawn.wav" );
		te->r.svFlags |= SVF_BROADCAST;
	}
#endif

	else {
		// play the normal respawn sound only to nearby clients
		G_AddEvent( ent, EV_ITEM_RESPAWN, 0 );
	}

	ent->nextthink = 0;
}


void Touch_Item( gentity_t* ent, gentity_t* player )
{
	int respawn;

	if (!player->client)
		return;
	if (player->health < 1)
		return;		// dead people can't pickup

	// the same pickup rules are used for client side and server side
	if ( !BG_CanItemBeGrabbed( g_gametype.integer, &ent->s, &player->client->ps ) ) {
		return;
	}

	G_LogPrintf( "Item: %i %s\n", player->s.number, ent->item->classname );

	// call the item-specific pickup function
	switch( ent->item->giType ) {
	case IT_WEAPON:
		respawn = Pickup_Weapon(ent, player);
		break;
	case IT_AMMO:
		respawn = Pickup_Ammo(ent, player);
		break;
	case IT_ARMOR:
		respawn = Pickup_Armor(ent, player);
		break;
	case IT_HEALTH:
		respawn = Pickup_Health(ent, player);
		break;
	case IT_POWERUP:
		respawn = Pickup_Powerup(ent, player);
		break;
#ifdef MISSIONPACK
	case IT_PERSISTANT_POWERUP:
		respawn = Pickup_PersistantPowerup(ent, player);
		break;
#endif
	case IT_TEAM:
		respawn = Pickup_Team(ent, player);
		break;
	case IT_HOLDABLE:
		respawn = Pickup_Holdable(ent, player);
		break;
	default:
		return;
	}

	if ( !respawn ) {
		return;
	}

	// fire item targets
	G_UseTargets(ent, player);

	// wait of -1 will not respawn
	if ( ent->wait == -1 ) {
		ent->r.svFlags |= SVF_NOCLIENT;
		ent->s.eFlags |= EF_NODRAW;
		ent->r.contents = 0;
		ent->unlinkAfterEvent = qtrue;
		return;
	}

	// non zero wait overrides respawn time
	if ( ent->wait ) {
		respawn = ent->wait;
	}

	// random can be used to vary the respawn time
	if ( ent->random ) {
		respawn += crandom() * ent->random;
		if ( respawn < 1 ) {
			respawn = 1;
		}
	}

	// dropped items will not respawn
	if ( ent->flags & FL_DROPPED_ITEM ) {
		ent->freeAfterEvent = qtrue;
	}

	// picked up items still stay around, they just don't draw anything
	// this allows respawnable items to be placed on movers
	ent->r.svFlags |= SVF_NOCLIENT;
	ent->s.eFlags |= EF_NODRAW;
	ent->r.contents = 0;

	// powerup pickups are global broadcasts
	if ((ent->item->giType == IT_POWERUP) || (ent->item->giType == IT_TEAM)) {
		// note that this HAS to use a tempent for the sound, because we hide the ent itself immediately
		gentity_t* te = G_TempEntity( vec3_origin, EV_GLOBAL_ITEM_PICKUP );
		te->s.eventParm = ent->s.modelindex;
		te->r.svFlags |= SVF_BROADCAST;
	}
	else {
		// play the normal pickup sound
		if (player->client->pers.predictItemPickup) {
			if (player->client->ps.externalEvent & ~EV_EVENT_BITS) {
				// the exisiting event will override the sound so we play a temp
				gentity_t* te = G_TempEntity( ent->r.currentOrigin, EV_ITEM_PICKUP );
				G_AddEvent( te, EV_ITEM_PICKUP, ent->s.modelindex );
			} else {
				G_AddPredictableEvent( player, EV_ITEM_PICKUP, ent->s.modelindex ); // this will almost always go through
			}
		} else {
			G_AddEvent( player, EV_ITEM_PICKUP, ent->s.modelindex );
		}
	}

	// a negative respawn time means to never respawn this item (but don't delete it)
	// this is used by items that are respawned by third party events such as ctf flags
	if ( respawn <= 0 ) {
		ent->nextthink = 0;
		ent->think = 0;
	} else {
		ent->nextthink = level.time + respawn * 1000;
		ent->think = RespawnItem;
	}

	trap_LinkEntity( ent );
}


///////////////////////////////////////////////////////////////


// spawns an item and tosses it in a given direction

gentity_t* LaunchItem( gitem_t* item, const vec3_t origin, const vec3_t velocity )
{
	gentity_t* dropped = G_Spawn();

	dropped->s.eType = ET_ITEM;
	dropped->s.modelindex = item - bg_itemlist; // store item number in modelindex
	dropped->s.modelindex2 = 1; // this is non-zero if it's a dropped item

	dropped->classname = item->classname;
	dropped->item = item;
	VectorSet( dropped->r.mins, -ITEM_RADIUS, -ITEM_RADIUS, -ITEM_RADIUS );
	VectorSet( dropped->r.maxs, ITEM_RADIUS, ITEM_RADIUS, ITEM_RADIUS );
	dropped->r.contents = CONTENTS_TRIGGER;

	dropped->touch = Touch_Item;

	G_SetOrigin( dropped, origin );
	dropped->s.pos.trType = TR_GRAVITY;
	dropped->s.pos.trTime = level.time;
	VectorCopy( velocity, dropped->s.pos.trDelta );

	dropped->flags = FL_DROPPED_ITEM;

#ifdef MISSIONPACK
	if ((g_gametype.integer == GT_CTF || g_gametype.integer == GT_1FCTF) && item->giType == IT_TEAM) {
#else
	if (g_gametype.integer == GT_CTF && item->giType == IT_TEAM) {
#endif
		// special case for CTF flags to auto-return them
		dropped->think = Team_DroppedFlagThink;
		dropped->nextthink = level.time + 30000;
		Team_CheckDroppedItem( dropped );
	} else { // auto-remove after 30 seconds
		dropped->think = G_FreeEntity;
		dropped->nextthink = level.time + 30000;
	}

	trap_LinkEntity( dropped );
	return dropped;
}


gentity_t* Drop_Item( gentity_t* ent, gitem_t* item, float angle )
{
	vec3_t velocity;
	vec3_t angles;

	VectorCopy( ent->s.apos.trBase, angles );
	angles[YAW] += angle;
	angles[PITCH] = 0;

	AngleVectors( angles, velocity, NULL, NULL );
	VectorScale( velocity, 150, velocity );
	velocity[2] += 200 + crandom() * 50;

	return LaunchItem( item, ent->s.pos.trBase, velocity );
}


///////////////////////////////////////////////////////////////


// trace down to find where an item should rest
// instead of letting them free fall from their spawn points

void FinishSpawningItem( gentity_t* ent )
{
	VectorSet( ent->r.mins, -ITEM_RADIUS, -ITEM_RADIUS, -ITEM_RADIUS );
	VectorSet( ent->r.maxs, ITEM_RADIUS, ITEM_RADIUS, ITEM_RADIUS );

	ent->s.eType = ET_ITEM;
	ent->s.modelindex = ent->item - bg_itemlist;		// store item number in modelindex
	ent->s.modelindex2 = 0; // zero indicates this isn't a dropped item

	ent->r.contents = CONTENTS_TRIGGER;
	ent->touch = Touch_Item;

	if ( ent->spawnflags & 1 ) {
		// suspended
		G_SetOrigin( ent, ent->s.origin );
	} else {
		// drop to floor
		trace_t tr;
		vec3_t dest;

		VectorSet( dest, ent->s.origin[0], ent->s.origin[1], ent->s.origin[2] - 4096 );
		trap_Trace( &tr, ent->s.origin, ent->r.mins, ent->r.maxs, dest, ent->s.number, MASK_SOLID );
		if ( tr.startsolid ) {
			G_Printf( "FinishSpawningItem: %s startsolid at %s\n", ent->classname, vtos(ent->s.origin) );
			G_FreeEntity( ent );
			return;
		}

		// allow to ride movers
		ent->s.groundEntityNum = tr.entityNum;

		G_SetOrigin( ent, tr.endpos );
	}

	// team slaves and targeted items aren't present at start
	if ( ( ent->flags & FL_TEAMSLAVE ) || ent->targetname ) {
		ent->s.eFlags |= EF_NODRAW;
		ent->r.contents = 0;
		return;
	}

	// powerups don't spawn in for a while
	if ( ent->item->giType == IT_POWERUP ) {
		float respawn = 30 + (random() * 15);
		ent->s.eFlags |= EF_NODRAW;
		ent->r.contents = 0;
		ent->nextthink = level.time + respawn * 1000;
		ent->think = RespawnItem;
		return;
	}

	trap_LinkEntity(ent);
}


///////////////////////////////////////////////////////////////


static qboolean itemRegistered[MAX_ITEMS];


void G_CheckTeamItems()
{
	Team_InitGame(); // stupid name, this has NOTHING to do with teamplay, just flag games

	if( g_gametype.integer == GT_CTF ) {
		const gitem_t* item;
		// check for the two flags
		item = BG_FindItem( "Red Flag" );
		if ( !item || !itemRegistered[ item - bg_itemlist ] ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_CTF_redflag in map" );
		}
		item = BG_FindItem( "Blue Flag" );
		if ( !item || !itemRegistered[ item - bg_itemlist ] ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_CTF_blueflag in map" );
		}
	}

#ifdef MISSIONPACK
	if( g_gametype.integer == GT_1FCTF ) {
		gitem_t	*item;

		// check for all three flags
		item = BG_FindItem( "Red Flag" );
		if ( !item || !itemRegistered[ item - bg_itemlist ] ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_CTF_redflag in map" );
		}
		item = BG_FindItem( "Blue Flag" );
		if ( !item || !itemRegistered[ item - bg_itemlist ] ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_CTF_blueflag in map" );
		}
		item = BG_FindItem( "Neutral Flag" );
		if ( !item || !itemRegistered[ item - bg_itemlist ] ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_CTF_neutralflag in map" );
		}
	}

	if( g_gametype.integer == GT_OBELISK ) {
		gentity_t	*ent;

		// check for the two obelisks
		ent = NULL;
		ent = G_Find( ent, FOFS(classname), "team_redobelisk" );
		if( !ent ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_redobelisk in map" );
		}

		ent = NULL;
		ent = G_Find( ent, FOFS(classname), "team_blueobelisk" );
		if( !ent ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_blueobelisk in map" );
		}
	}

	if( g_gametype.integer == GT_HARVESTER ) {
		gentity_t	*ent;

		// check for all three obelisks
		ent = NULL;
		ent = G_Find( ent, FOFS(classname), "team_redobelisk" );
		if( !ent ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_redobelisk in map" );
		}

		ent = NULL;
		ent = G_Find( ent, FOFS(classname), "team_blueobelisk" );
		if( !ent ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_blueobelisk in map" );
		}

		ent = NULL;
		ent = G_Find( ent, FOFS(classname), "team_neutralobelisk" );
		if( !ent ) {
			G_Printf( S_COLOR_YELLOW "WARNING: No team_neutralobelisk in map" );
		}
	}
#endif

}


void ClearRegisteredItems()
{
	memset( itemRegistered, 0, sizeof( itemRegistered ) );

	// players always start with the base weapons
	RegisterItem( BG_FindItemForWeapon( WP_MACHINEGUN ) );
	RegisterItem( BG_FindItemForWeapon( WP_GAUNTLET ) );

#ifdef MISSIONPACK
	if( g_gametype.integer == GT_HARVESTER ) {
		RegisterItem( BG_FindItem( "Red Cube" ) );
		RegisterItem( BG_FindItem( "Blue Cube" ) );
	}
#endif
}


// add an item to the precache list

void RegisterItem( const gitem_t* item )
{
	if ( !item ) {
		G_Error( "RegisterItem: NULL" );
	}
	itemRegistered[ item - bg_itemlist ] = qtrue;
}


// write the map's items to a config string so the client will know which ones to precache

void SaveRegisteredItems()
{
	char string[MAX_ITEMS+1];
	int i, count = 0;

	for ( i = 0 ; i < bg_numItems ; i++ ) {
		if ( itemRegistered[i] ) {
			count++;
			string[i] = '1';
		} else {
			string[i] = '0';
		}
	}
	string[ bg_numItems ] = 0;

	G_Printf( "%i items registered\n", count );
	trap_SetConfigstring( CS_ITEMS, string );
}


///////////////////////////////////////////////////////////////


/*
Sets the clipping size and plants the object on the floor.

Items can't be immediately dropped to floor, because they might
be on an entity that hasn't spawned yet.
*/
void G_SpawnItem( gentity_t* ent, gitem_t* item )
{
	G_SpawnFloat( "random", "0", &ent->random );
	G_SpawnFloat( "wait", "0", &ent->wait );

	if (trap_Cvar_VariableIntegerValue( va("disable_%s", item->classname) ))
		return;

	RegisterItem( item );

	ent->item = item;
	// some movers spawn on the second frame, so delay item
	// spawns until the third frame so they can ride trains
	ent->nextthink = level.time + FRAMETIME * 2;
	ent->think = FinishSpawningItem;

	ent->physicsBounce = 0.50;		// items are bouncy

	if ( item->giType == IT_POWERUP ) {
		G_SoundIndex( "sound/items/poweruprespawn.wav" );
		G_SpawnFloat( "noglobalsound", "0", &ent->speed);
	}

#ifdef MISSIONPACK
	if ( item->giType == IT_PERSISTANT_POWERUP ) {
		ent->s.generic1 = ent->spawnflags;
	}
#endif
}


static void G_BounceItem( gentity_t* ent, trace_t* trace )
{
	vec3_t	velocity;
	float	dot;
	int		hitTime;

	// reflect the velocity on the trace plane
	hitTime = level.previousTime + ( level.time - level.previousTime ) * trace->fraction;
	BG_EvaluateTrajectoryDelta( &ent->s.pos, hitTime, velocity );
	dot = DotProduct( velocity, trace->plane.normal );
	VectorMA( velocity, -2*dot, trace->plane.normal, ent->s.pos.trDelta );

	// cut the velocity to keep from bouncing forever
	VectorScale( ent->s.pos.trDelta, ent->physicsBounce, ent->s.pos.trDelta );

	// check for stop
	if ( trace->plane.normal[2] > 0 && ent->s.pos.trDelta[2] < 40 ) {
		trace->endpos[2] += 1.0;	// make sure it is off ground
		SnapVector( trace->endpos );
		G_SetOrigin( ent, trace->endpos );
		ent->s.groundEntityNum = trace->entityNum;
		return;
	}

	VectorAdd( ent->r.currentOrigin, trace->plane.normal, ent->r.currentOrigin);
	VectorCopy( ent->r.currentOrigin, ent->s.pos.trBase );
	ent->s.pos.trTime = level.time;
}


void G_RunItem( gentity_t* ent )
{
	vec3_t		origin;
	trace_t		tr;
	int			mask;

	// if groundentity has been set to -1, it may have been pushed off an edge
	if ( ent->s.groundEntityNum == -1 ) {
		if ( ent->s.pos.trType != TR_GRAVITY ) {
			ent->s.pos.trType = TR_GRAVITY;
			ent->s.pos.trTime = level.time;
		}
	}

	G_RunThink( ent );

	if ( ent->s.pos.trType == TR_STATIONARY ) {
		return;
	}

	// get current position
	BG_EvaluateTrajectory( &ent->s.pos, level.time, origin );

	// trace a line from the current position to the desired one
	mask = ent->clipmask ? ent->clipmask : (MASK_PLAYERSOLID & ~CONTENTS_BODY);
	trap_Trace( &tr, ent->r.currentOrigin, ent->r.mins, ent->r.maxs, origin, ent->r.ownerNum, mask );

	VectorCopy( tr.endpos, ent->r.currentOrigin );

	if ( tr.startsolid ) {
		tr.fraction = 0;
	}

	trap_LinkEntity( ent );

	if ( tr.fraction == 1 ) {
		return;
	}

	// if it is in a nodrop volume, remove it
	if ( trap_PointContents( ent->r.currentOrigin, -1 ) & CONTENTS_NODROP ) {
		if (ent->item && ent->item->giType == IT_TEAM) {
			Team_FreeEntity(ent);
		} else {
			G_FreeEntity( ent );
		}
		return;
	}

	G_BounceItem( ent, &tr );
}

