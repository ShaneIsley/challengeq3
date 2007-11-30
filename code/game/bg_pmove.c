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
// bg_pmove.c -- both games player movement code
// takes a playerstate and a usercmd as input and returns a modifed playerstate

#include "../qcommon/q_shared.h"
#include "bg_public.h"

// all of these locals will be zeroed before each pmove
// just to make damn sure we don't have any differences when running on client or server

typedef struct {
	vec3_t		forward, right, up;
	float		frametime;

	int			msec;

	qboolean	walking;
	qboolean	groundPlane;
	trace_t		groundTrace;

	float		impactSpeed;

	vec3_t		previous_origin;
	vec3_t		previous_velocity;
	int			previous_waterlevel;
} pml_t;

static pml_t pml;
static pmove_t* pm;
static int c_pmove = 0;

const vec3_t playerMins = { -15, -15, -24 };
const vec3_t playerMaxs = { 15, 15, 32 };


// movement parameters
static const float pm_stopspeed = 100.0f;
static const float pm_duckScale = 0.25f;
static const float pm_swimScale = 0.50f;
static const float pm_wadeScale = 0.70f;

static const float WALK_ACCELERATE = 10.0f;
static const float pm_airaccelerate = 1.0f;
static const float pm_flyaccelerate = 8.0f;

static const float pm_friction = 6.0f;
static const float pm_waterfriction = 1.0f;
static const float pm_flightfriction = 3.0f;
static const float pm_spectatorfriction = 5.0f;

static const float MIN_WALK_NORMAL = 0.7f; // can't walk on slopes steeper than this
static const float STEPSIZE = 18;
static const float JUMP_VELOCITY = 275;

static const float WATER_ACCELERATE = 4.0f;
static const float WATER_SINK_SPEED = 60.0f;
static const float WATER_SWIMSCALE = 0.5;
static const float WATER_WADESCALE = 0.75;

static const float WEAPON_DROP_TIME = 200;
static const float WEAPON_RAISE_TIME = 250;
static const float WEAPON_EMPTY_TIME = 500;

static const float TIMER_LAND = 130;
static const float TIMER_GESTURE = (34*66+50);

static const float OVERCLIP = 1.001f;


static void PM_AddEvent( int newEvent )
{
	BG_AddPredictableEventToPlayerstate( newEvent, 0, pm->ps );
}


static void PM_AddTouchEnt( int entityNum )
{
	int i;

	if ( entityNum == ENTITYNUM_WORLD ) {
		return;
	}
	if ( pm->numtouch == MAXTOUCH ) {
		return;
	}

	// see if it is already added
	for ( i = 0 ; i < pm->numtouch ; i++ ) {
		if ( pm->touchents[ i ] == entityNum ) {
			return;
		}
	}

	pm->touchents[pm->numtouch] = entityNum;
	pm->numtouch++;
}


// slide off of the impacting surface

static void PM_ClipVelocity( const vec3_t in, const vec3_t normal, vec3_t out, float overbounce )
{
	float backoff = DotProduct(in, normal);

	if (backoff < 0) {
		backoff *= overbounce;
	} else {
		backoff /= overbounce;
	}

	VectorMA( in, -backoff, normal, out );
}


static void PM_StartTorsoAnim( int anim )
{
	if ( pm->ps->pm_type >= PM_DEAD ) {
		return;
	}
	pm->ps->torsoAnim = ( ( pm->ps->torsoAnim & ANIM_TOGGLEBIT ) ^ ANIM_TOGGLEBIT ) | anim;
}

static void PM_ContinueTorsoAnim( int anim )
{
	if ( ( pm->ps->torsoAnim & ~ANIM_TOGGLEBIT ) == anim ) {
		return;
	}
	if ( pm->ps->torsoTimer > 0 ) {
		return;		// a high priority animation is running
	}
	PM_StartTorsoAnim( anim );
}

static void PM_TorsoAnimation()
{
	if (pm->ps->weaponstate == WEAPON_READY) {
		PM_ContinueTorsoAnim( (pm->ps->weapon == WP_GAUNTLET) ? TORSO_STAND2 : TORSO_STAND );
	}
}


static void PM_StartLegsAnim( int anim )
{
	if ( pm->ps->pm_type >= PM_DEAD ) {
		return;
	}
	if ( pm->ps->legsTimer > 0 ) {
		return;		// a high priority animation is running
	}
	pm->ps->legsAnim = ( ( pm->ps->legsAnim & ANIM_TOGGLEBIT ) ^ ANIM_TOGGLEBIT ) | anim;
}

static void PM_ContinueLegsAnim( int anim )
{
	if ( ( pm->ps->legsAnim & ~ANIM_TOGGLEBIT ) == anim ) {
		return;
	}
	if ( pm->ps->legsTimer > 0 ) {
		return;		// a high priority animation is running
	}
	PM_StartLegsAnim( anim );
}

static void PM_ForceLegsAnim( int anim )
{
	pm->ps->legsTimer = 0;
	PM_StartLegsAnim( anim );
}


// handles both ground friction and water friction

static void PM_Friction()
{
	vec3_t vel;
	float speed, newspeed;
	float drop = 0;

	VectorCopy( pm->ps->velocity, vel );
	if (pml.walking)
		vel[2] = 0; // ignore slope movement when determining speed
	speed = VectorLength(vel);

	if (speed < 1) {
		// reset X and Y velocity, but leave Z alone so that gravity accumulates
		pm->ps->velocity[0] = 0;
		pm->ps->velocity[1] = 0;
		return;
	}

	// apply ground friction
	if (pm->waterlevel <= WATERLEVEL_SHALLOW) {
		if (pml.walking && !(pml.groundTrace.surfaceFlags & SURF_SLICK)) {
			// if getting knocked back, no friction
			if (!(pm->ps->pm_flags & PMF_TIME_KNOCKBACK)) {
				float control = speed < pm_stopspeed ? pm_stopspeed : speed;
				drop += control * pml.frametime * pm_friction;
			}
		}
	}

	// apply water friction even if just paddling
	if (pm->waterlevel) {
		drop += speed * pml.frametime * pm_waterfriction * pm->waterlevel;
	}

	if (pm->ps->powerups[PW_FLIGHT]) {
		drop += speed * pml.frametime * pm_flightfriction;
	}

	if (pm->ps->pm_type == PM_SPECTATOR) {
		drop += speed * pml.frametime * pm_spectatorfriction;
	}

	// scale the velocity
	newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;

	VectorScale( pm->ps->velocity, newspeed, pm->ps->velocity );
}


static void PM_Accelerate( const vec3_t wishdir, float wishspeed, float accel )
{
	float addspeed = wishspeed - DotProduct(pm->ps->velocity, wishdir);

	if (addspeed > 0) {
		accel *= pml.frametime * wishspeed;
		if (accel > addspeed)
			accel = addspeed;
		VectorMA(pm->ps->velocity, accel, wishdir, pm->ps->velocity);
	}
}


/*
Returns the scale factor to apply to cmd movements
This allows the clients to use axial -127 to 127 values for all directions
without getting a sqrt(2) distortion in speed.
*/
static float PM_CmdScale( const usercmd_t* cmd )
{
	int max;
	float total, scale;

	max = abs( cmd->forwardmove );
	if (abs( cmd->rightmove ) > max)
		max = abs( cmd->rightmove );
	if (abs( cmd->upmove ) > max)
		max = abs( cmd->upmove );

	if (!max)
		return 0;

	total = sqrt( cmd->forwardmove * cmd->forwardmove
			+ cmd->rightmove * cmd->rightmove + cmd->upmove * cmd->upmove );
	scale = (float)pm->ps->speed * max / ( 127.0 * total );

	return scale;
}


static qboolean PM_UserIntentions( const usercmd_t* cmd, float* wishspeed, vec3_t wishdir )
{
	int i;
	float scale = PM_CmdScale( cmd );
	float fmove = pm->cmd.forwardmove;
	float smove = pm->cmd.rightmove;

	if (!scale) {
		VectorClear( wishdir );
		*wishspeed = 0;
		return qfalse;
	}

	for (i = 0; i < 3; ++i)
		wishdir[i] = (pml.forward[i] * fmove) + (pml.right[i] * smove);
	wishdir[2] += pm->cmd.upmove;
	*wishspeed = scale * VectorNormalize( wishdir );

	return qtrue;
}


// determine the rotation of the legs relative to the facing dir

static void PM_SetMovementDir()
{
	if ( pm->cmd.forwardmove || pm->cmd.rightmove ) {
		if ( pm->cmd.rightmove == 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 0;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 1;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove == 0 ) {
			pm->ps->movementDir = 2;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 3;
		} else if ( pm->cmd.rightmove == 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 4;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 5;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove == 0 ) {
			pm->ps->movementDir = 6;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 7;
		}
	} else {
		// if they aren't actively going directly sideways,
		// change the animation to the diagonal so they
		// don't stop too crooked
		if ( pm->ps->movementDir == 2 ) {
			pm->ps->movementDir = 1;
		} else if ( pm->ps->movementDir == 6 ) {
			pm->ps->movementDir = 7;
		} 
	}
}


static qboolean PM_CheckJump()
{
	if ( pm->ps->pm_flags & PMF_RESPAWNED ) {
		return qfalse;		// don't allow jump until all buttons are up
	}

	if ( pm->cmd.upmove < 10 ) {
		// not holding jump
		return qfalse;
	}

	// must wait for jump to be released
	if ( pm->ps->pm_flags & PMF_JUMP_HELD ) {
		// clear upmove so cmdscale doesn't lower running speed
		pm->cmd.upmove = 0;
		return qfalse;
	}

	pml.groundPlane = qfalse;		// jumping away
	pml.walking = qfalse;
	pm->ps->pm_flags |= PMF_JUMP_HELD;

	pm->ps->groundEntityNum = ENTITYNUM_NONE;
	pm->ps->velocity[2] = JUMP_VELOCITY;
	PM_AddEvent( EV_JUMP );

	if ( pm->cmd.forwardmove >= 0 ) {
		PM_ForceLegsAnim( LEGS_JUMP );
		pm->ps->pm_flags &= ~PMF_BACKWARDS_JUMP;
	} else {
		PM_ForceLegsAnim( LEGS_JUMPB );
		pm->ps->pm_flags |= PMF_BACKWARDS_JUMP;
	}

	return qtrue;
}


///////////////////////////////////////////////////////////////


#define MAX_CLIP_PLANES 5

// returns true if the velocity was clipped in some way

static qboolean PM_SlideMove( qboolean gravity )
{
	int			bumpcount;
	vec3_t		dir;
	float		d;
	int			numplanes = 0;
	vec3_t		planes[MAX_CLIP_PLANES];
	vec3_t		primal_velocity;
	vec3_t		clipVelocity;
	int			i, j, k;
	trace_t	trace;
	vec3_t		end;
	float		time_left;
	float		into;
	vec3_t		endVelocity;
	vec3_t		endClipVelocity;

	VectorCopy (pm->ps->velocity, primal_velocity);

	if ( gravity ) {
		VectorCopy( pm->ps->velocity, endVelocity );
		endVelocity[2] -= pm->ps->gravity * pml.frametime;
		pm->ps->velocity[2] = ( pm->ps->velocity[2] + endVelocity[2] ) * 0.5;
		primal_velocity[2] = endVelocity[2];
		if ( pml.groundPlane ) {
			// slide along the ground plane
			PM_ClipVelocity (pm->ps->velocity, pml.groundTrace.plane.normal, pm->ps->velocity, OVERCLIP );
		}
	} else {
		VectorClear(endVelocity);
	}

	time_left = pml.frametime;

	// never turn against the ground plane
	if ( pml.groundPlane ) {
		VectorCopy( pml.groundTrace.plane.normal, planes[0] );
		numplanes = 1;
	}

	// never turn against original velocity
	VectorNormalize2( pm->ps->velocity, planes[numplanes] );
	numplanes++;

	for ( bumpcount=0 ; bumpcount < 4; bumpcount++ ) {

		// calculate position we are trying to move to
		VectorMA( pm->ps->origin, time_left, pm->ps->velocity, end );

		// see if we can make it there
		pm->trace ( &trace, pm->ps->origin, pm->mins, pm->maxs, end, pm->ps->clientNum, pm->tracemask);

		if (trace.allsolid) {
			// entity is completely trapped in another solid
			pm->ps->velocity[2] = 0;	// don't build up falling damage, but allow sideways acceleration
			return qtrue;
		}

		if (trace.fraction > 0) {
			// actually covered some distance
			VectorCopy (trace.endpos, pm->ps->origin);
		}

		if (trace.fraction == 1) {
			 break;		// moved the entire distance
		}

		// save entity for contact
		PM_AddTouchEnt( trace.entityNum );

		time_left -= time_left * trace.fraction;

		if (numplanes >= MAX_CLIP_PLANES) {
			// this shouldn't really happen
			VectorClear( pm->ps->velocity );
			return qtrue;
		}

		//
		// if this is the same plane we hit before, nudge velocity
		// out along it, which fixes some epsilon issues with
		// non-axial planes
		//
		for ( i = 0 ; i < numplanes ; i++ ) {
			if ( DotProduct( trace.plane.normal, planes[i] ) > 0.99 ) {
				VectorAdd( trace.plane.normal, pm->ps->velocity, pm->ps->velocity );
				break;
			}
		}
		if ( i < numplanes ) {
			continue;
		}
		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

		//
		// modify velocity so it parallels all of the clip planes
		//

		// find a plane that it enters
		for ( i = 0 ; i < numplanes ; i++ ) {
			into = DotProduct( pm->ps->velocity, planes[i] );
			if ( into >= 0.1 ) {
				continue;		// move doesn't interact with the plane
			}

			// see how hard we are hitting things
			if ( -into > pml.impactSpeed ) {
				pml.impactSpeed = -into;
			}

			// slide along the plane
			PM_ClipVelocity (pm->ps->velocity, planes[i], clipVelocity, OVERCLIP );

			// slide along the plane
			PM_ClipVelocity (endVelocity, planes[i], endClipVelocity, OVERCLIP );

			// see if there is a second plane that the new move enters
			for ( j = 0 ; j < numplanes ; j++ ) {
				if ( j == i ) {
					continue;
				}
				if ( DotProduct( clipVelocity, planes[j] ) >= 0.1 ) {
					continue;		// move doesn't interact with the plane
				}

				// try clipping the move to the plane
				PM_ClipVelocity( clipVelocity, planes[j], clipVelocity, OVERCLIP );
				PM_ClipVelocity( endClipVelocity, planes[j], endClipVelocity, OVERCLIP );

				// see if it goes back into the first clip plane
				if ( DotProduct( clipVelocity, planes[i] ) >= 0 ) {
					continue;
				}

				// slide the original velocity along the crease
				CrossProduct (planes[i], planes[j], dir);
				VectorNormalize( dir );
				d = DotProduct( dir, pm->ps->velocity );
				VectorScale( dir, d, clipVelocity );

				CrossProduct (planes[i], planes[j], dir);
				VectorNormalize( dir );
				d = DotProduct( dir, endVelocity );
				VectorScale( dir, d, endClipVelocity );

				// see if there is a third plane the the new move enters
				for ( k = 0 ; k < numplanes ; k++ ) {
					if ( k == i || k == j ) {
						continue;
					}
					if ( DotProduct( clipVelocity, planes[k] ) >= 0.1 ) {
						continue;		// move doesn't interact with the plane
					}

					// stop dead at a tripple plane interaction
					VectorClear( pm->ps->velocity );
					return qtrue;
				}
			}

			// if we have fixed all interactions, try another move
			VectorCopy( clipVelocity, pm->ps->velocity );
			VectorCopy( endClipVelocity, endVelocity );
			break;
		}
	}

	if ( gravity ) {
		VectorCopy( endVelocity, pm->ps->velocity );
	}

	// don't change velocity if in a timer (FIXME: is this correct?)
	if ( pm->ps->pm_time ) {
		VectorCopy( primal_velocity, pm->ps->velocity );
	}

	return ( bumpcount != 0 );
}


static void PM_StepSlideMove( qboolean gravity )
{
	vec3_t		start_o, start_v;
	vec3_t		down_o, down_v;
	trace_t		trace;
	vec3_t		up, down;
	float		delta;

	VectorCopy(pm->ps->origin, start_o);
	VectorCopy(pm->ps->velocity, start_v);

	if (PM_SlideMove(gravity) == qfalse)	// stupid backwards return code
		return;		// we got exactly where we wanted to go first try	

	VectorCopy(start_o, down);
	down[2] -= STEPSIZE;
	pm->trace (&trace, start_o, pm->mins, pm->maxs, down, pm->ps->clientNum, pm->tracemask);
	VectorSet(up, 0, 0, 1);
	// never step up when you still have up velocity
	if ((pm->ps->velocity[2] > 0) && ((trace.fraction == 1.0)
			|| (DotProduct(trace.plane.normal, up) < MIN_WALK_NORMAL))) {
		return;
	}

	VectorCopy (pm->ps->origin, down_o);
	VectorCopy (pm->ps->velocity, down_v);

	VectorCopy (start_o, up);
	up[2] += STEPSIZE;

	// test the player position if they were a stepheight higher
	pm->trace (&trace, start_o, pm->mins, pm->maxs, up, pm->ps->clientNum, pm->tracemask);
	if ( trace.allsolid ) {
		if ( pm->debugLevel ) {
			Com_Printf("%i:bend can't step\n", c_pmove);
		}
		return;		// can't step up
	}

	delta = trace.endpos[2] - start_o[2];
	// try slidemove from this position
	VectorCopy (trace.endpos, pm->ps->origin);
	VectorCopy (start_v, pm->ps->velocity);

	PM_SlideMove( gravity );

	// push down the final amount
	VectorCopy (pm->ps->origin, down);
	down[2] -= delta;
	pm->trace (&trace, pm->ps->origin, pm->mins, pm->maxs, down, pm->ps->clientNum, pm->tracemask);
	if ( !trace.allsolid ) {
		VectorCopy (trace.endpos, pm->ps->origin);
	}
	if ( trace.fraction < 1.0 ) {
		PM_ClipVelocity( pm->ps->velocity, trace.plane.normal, pm->ps->velocity, OVERCLIP );
	}

	// use the step move
	delta = pm->ps->origin[2] - start_o[2];
	if ( delta > 2 ) {
		if ( delta < 7 ) {
			PM_AddEvent( EV_STEP_4 );
		} else if ( delta < 11 ) {
			PM_AddEvent( EV_STEP_8 );
		} else if ( delta < 15 ) {
			PM_AddEvent( EV_STEP_12 );
		} else {
			PM_AddEvent( EV_STEP_16 );
		}
		if ( pm->debugLevel ) {
			Com_Printf("%i:stepped\n", c_pmove);
		}
	}
}


///////////////////////////////////////////////////////////////


// send the player flying onto dry land automatically at water's edge (DM12 etc)

static qboolean PM_CheckWaterJump()
{
	vec3_t v;

	if (pm->ps->pm_time)
		return qfalse;

	// only for vaulting out of waist-deep water, not puddles
	if (pm->waterlevel != WATERLEVEL_DEEP)
		return qfalse;

	VectorSet( v, pml.forward[0], pml.forward[1], 0 );
	VectorNormalize( v );

	VectorMA( pm->ps->origin, 30, v, v );
	v[2] += 4;
	if (!(pm->pointcontents( v, pm->ps->clientNum ) & CONTENTS_SOLID))
		return qfalse;

	v[2] += 16;
	if (pm->pointcontents( v, pm->ps->clientNum ))
		return qfalse;

	// jump out of water
	VectorScale( pml.forward, 200, pm->ps->velocity );
	pm->ps->velocity[2] = 350;

	pm->ps->pm_flags |= PMF_TIME_WATERJUMP;
	pm->ps->pm_time = 2000;

	return qtrue;
}


static void PM_WaterJumpMove()
{
	// waterjump has no control, but has gravity
	PM_StepSlideMove( qtrue );

	pm->ps->velocity[2] -= pm->ps->gravity * pml.frametime;
	if (pm->ps->velocity[2] < 0) {
		// cancel as soon as we are falling down again
		pm->ps->pm_flags &= ~PMF_ALL_TIMES;
		pm->ps->pm_time = 0;
	}
}


static void PM_WaterMove()
{
	vec3_t	wishdir;
	float	wishspeed;

	if (PM_CheckWaterJump()) {
		PM_WaterJumpMove();
		return;
	}

	PM_Friction();

	if (!PM_UserIntentions( &pm->cmd, &wishspeed, wishdir )) {
		VectorSet( wishdir, 0, 0, -WATER_SINK_SPEED );
		wishspeed = VectorNormalize( wishdir );
	}

	if (pm->waterlevel == WATERLEVEL_SHALLOW) {
		if (wishspeed > pm->ps->speed * WATER_WADESCALE) {
			wishspeed = pm->ps->speed * WATER_WADESCALE;
		}
	} else {
		if (wishspeed > pm->ps->speed * WATER_SWIMSCALE) {
			wishspeed = pm->ps->speed * WATER_SWIMSCALE;
		}
	}

	PM_Accelerate( wishdir, wishspeed, WATER_ACCELERATE );

	// make sure we can go up slopes easily under water
	if (pml.groundPlane && DotProduct( pm->ps->velocity, pml.groundTrace.plane.normal ) < 0) {
		PM_ClipVelocity( pm->ps->velocity, pml.groundTrace.plane.normal, pm->ps->velocity, OVERCLIP );
	}

	PM_SlideMove( qfalse );
}


#ifdef MISSIONPACK
static void PM_InvulnerabilityMove()
{
	pm->cmd.forwardmove = 0;
	pm->cmd.rightmove = 0;
	pm->cmd.upmove = 0;
	VectorClear(pm->ps->velocity);
}
#endif


// for the flight powerup only, NOT all air movement

static void PM_FlyMove()
{
	float	wishspeed;
	vec3_t	wishdir;

	PM_Friction();

	PM_UserIntentions( &pm->cmd, &wishspeed, wishdir );

	PM_Accelerate( wishdir, wishspeed, pm_flyaccelerate );

	PM_StepSlideMove( qfalse );
}


static void PM_AirMove()
{
	vec3_t	wishdir;
	float	wishspeed;

	PM_Friction();
	PM_SetMovementDir();

	// project the moves down onto an imaginary plane
	pml.forward[2] = 0;
	pml.right[2] = 0;
	VectorNormalize(pml.forward);
	VectorNormalize(pml.right);

	wishdir[0] = (pml.forward[0] * pm->cmd.forwardmove) + (pml.right[0] * pm->cmd.rightmove);
	wishdir[1] = (pml.forward[1] * pm->cmd.forwardmove) + (pml.right[1] * pm->cmd.rightmove);
	wishdir[2] = 0;
	wishspeed = VectorNormalize(wishdir) * PM_CmdScale(&pm->cmd);

	PM_Accelerate(wishdir, wishspeed, pm_airaccelerate);

	// we may have a ground plane that is very steep, even though we don't have a groundentity
	// slide along the steep plane
	if (pml.groundPlane)
		PM_ClipVelocity(pm->ps->velocity, pml.groundTrace.plane.normal, pm->ps->velocity, OVERCLIP);

	PM_StepSlideMove(qtrue);
}


static void PM_GrappleMove()
{
	vec3_t vel, v;
	float vlen;

	VectorScale(pml.forward, -16, v);
	VectorAdd(pm->ps->grapplePoint, v, v);
	VectorSubtract(v, pm->ps->origin, vel);
	vlen = VectorLength(vel);
	VectorNormalize( vel );

	if (vlen <= 100)
		VectorScale(vel, 10 * vlen, vel);
	else
		VectorScale(vel, 800, vel);

	VectorCopy(vel, pm->ps->velocity);

	pml.groundPlane = qfalse;
}


static void PM_WalkMove()
{
	int		i;
	vec3_t	wishdir;
	float	wishspeed;
	float	accelerate = WALK_ACCELERATE;

	if ((pm->waterlevel > WATERLEVEL_DEEP) && DotProduct( pml.forward, pml.groundTrace.plane.normal ) > 0) {
		// begin swimming
		PM_WaterMove();
		return;
	}

	if (PM_CheckJump()) {
		if (pm->waterlevel > WATERLEVEL_SHALLOW) {
			PM_WaterMove();
		} else {
			PM_AirMove();
		}
		return;
	}

	PM_Friction();

	// set the movementDir so clients can rotate the legs for strafing
	PM_SetMovementDir();

	// project moves down to flat plane
	pml.forward[2] = 0;
	pml.right[2] = 0;

	// project the forward and right directions onto the ground plane
	PM_ClipVelocity( pml.forward, pml.groundTrace.plane.normal, pml.forward, OVERCLIP );
	PM_ClipVelocity( pml.right, pml.groundTrace.plane.normal, pml.right, OVERCLIP );

	VectorNormalize( pml.forward );
	VectorNormalize( pml.right );

	for (i = 0; i < 3; ++i) {
		wishdir[i] = (pml.forward[i] * pm->cmd.forwardmove) + (pml.right[i] * pm->cmd.rightmove);
	}
	// wish velocity Z should NOT be zeroed, because of slopes
	//wishdir[2] = 0;

	wishspeed = VectorNormalize(wishdir) * PM_CmdScale(&pm->cmd);

	// clamp the speed lower if ducking
	if ((pm->ps->pm_flags & PMF_DUCKED) && (wishspeed > pm->ps->speed * pm_duckScale))
		wishspeed = pm->ps->speed * pm_duckScale;

	// clamp the speed lower if wading or walking on the bottom
	if (pm->waterlevel) {
		float waterScale = pm->waterlevel / 3.0;
		float waterScale2 = (pm->waterlevel == WATERLEVEL_SHALLOW) ? WATER_WADESCALE : WATER_SWIMSCALE;
		waterScale = 1.0 - (1.0 - waterScale2) * waterScale;
		if (wishspeed > pm->ps->speed * waterScale) {
			wishspeed = pm->ps->speed * waterScale;
		}
	}

	// lower acceleration (control) when on slippery stuff or being smacked around
	if ((pml.groundTrace.surfaceFlags & SURF_SLICK) || (pm->ps->pm_flags & PMF_TIME_KNOCKBACK))
		accelerate = 1.0f;

	PM_Accelerate(wishdir, wishspeed, accelerate);

	//Com_Printf("velocity = %1.1f %1.1f %1.1f\n", pm->ps->velocity[0], pm->ps->velocity[1], pm->ps->velocity[2]);
	//Com_Printf("velocity1 = %1.1f\n", VectorLength(pm->ps->velocity));

	if ((pml.groundTrace.surfaceFlags & SURF_SLICK) || (pm->ps->pm_flags & PMF_TIME_KNOCKBACK)) {
		pm->ps->velocity[2] -= pm->ps->gravity * pml.frametime;
	} else {
		// don't reset the z velocity for slopes
		//pm->ps->velocity[2] = 0;
	}

	// don't do anything if standing still
	if (!pm->ps->velocity[0] && !pm->ps->velocity[1])
		return;

	PM_StepSlideMove( qfalse );
}


static void PM_DeadMove()
{
	float speed;

	if (!pml.walking)
		return;

	// extra friction
	speed = VectorLength( pm->ps->velocity ) - 20;
	if (speed <= 0) {
		VectorClear( pm->ps->velocity );
	} else {
		VectorNormalize( pm->ps->velocity );
		VectorScale( pm->ps->velocity, speed, pm->ps->velocity );
	}
}


static void PM_NoclipMove()
{
	float	speed;
	vec3_t	wishdir;
	float	wishspeed;

	pm->ps->viewheight = DEFAULT_VIEWHEIGHT;

	speed = VectorLength( pm->ps->velocity );
	if (speed < 1) {
		VectorCopy(vec3_origin, pm->ps->velocity);
	} else {
		// extra friction for noclip
		float drop = pml.frametime * 10 * (speed < pm_stopspeed ? pm_stopspeed : speed);
		float newspeed = speed - drop;
		if (newspeed < 0)
			newspeed = 0;
		newspeed /= speed;
		VectorScale(pm->ps->velocity, newspeed, pm->ps->velocity);
	}

	PM_UserIntentions( &pm->cmd, &wishspeed, wishdir );

	PM_Accelerate( wishdir, wishspeed, WALK_ACCELERATE );

	VectorMA( pm->ps->origin, pml.frametime, pm->ps->velocity, pm->ps->origin );
}


///////////////////////////////////////////////////////////////


// returns an event number apropriate for the groundsurface

static int PM_FootstepForSurface()
{
	if ( pml.groundTrace.surfaceFlags & SURF_NOSTEPS ) {
		return 0;
	}
	if ( pml.groundTrace.surfaceFlags & SURF_METALSTEPS ) {
		return EV_FOOTSTEP_METAL;
	}
	return EV_FOOTSTEP;
}


// check for hard landings that generate sound events

static void PM_CrashLand()
{
	float		delta;
	float		dist;
	float		vel, acc;
	float		t;
	float		a, b, c, den;

	// decide which landing animation to use
	if ( pm->ps->pm_flags & PMF_BACKWARDS_JUMP ) {
		PM_ForceLegsAnim( LEGS_LANDB );
	} else {
		PM_ForceLegsAnim( LEGS_LAND );
	}

	pm->ps->legsTimer = TIMER_LAND;

	// calculate the exact velocity on landing
	dist = pm->ps->origin[2] - pml.previous_origin[2];
	vel = pml.previous_velocity[2];
	acc = -pm->ps->gravity;

	a = acc / 2;
	b = vel;
	c = -dist;

	den =  b * b - 4 * a * c;
	if ( den < 0 ) {
		return;
	}
	t = (-b - sqrt( den ) ) / ( 2 * a );

	delta = vel + t * acc;
	delta = delta*delta * 0.0001;

	// ducking while falling doubles damage
	if (pm->ps->pm_flags & PMF_DUCKED)
		delta *= 2;

	// reduce falling damage if there is standing water, and take none if completely underwater
	switch (pm->waterlevel) {
	case WATERLEVEL_SHALLOW:
		delta *= 0.5;
		break;
	case WATERLEVEL_DEEP:
		delta *= 0.25;
		break;
	case WATERLEVEL_DROWN:
		delta = 0;
		break;
	}

	if (delta < 1)
		return;

	// create a local entity event to play the sound

	// SURF_NODAMAGE is used for bounce pads where you don't ever
	// want to take damage or play a crunch sound
	if ( !(pml.groundTrace.surfaceFlags & SURF_NODAMAGE) )  {
		if ( delta > 60 ) {
			PM_AddEvent( EV_FALL_FAR );
		} else if ( delta > 42 ) {
			// this is a pain grunt, so don't play it if dead
			if ( pm->ps->stats[STAT_HEALTH] > 0 ) {
				PM_AddEvent( EV_FALL_MEDIUM );
			}
		} else if ( delta > 7 ) {
			PM_AddEvent( EV_FALL_SHORT );
		} else if (!pm->noFootsteps) {
			PM_AddEvent( PM_FootstepForSurface() );
		}
	}

	// start footstep cycle over
	pm->ps->bobCycle = 0;
}


static qboolean PM_CorrectAllSolid( trace_t *trace )
{
	int			i, j, k;
	vec3_t		point;

	if ( pm->debugLevel ) {
		Com_Printf("%i:allsolid\n", c_pmove);
	}

	// jitter around
	for (i = -1; i <= 1; i++) {
		for (j = -1; j <= 1; j++) {
			for (k = -1; k <= 1; k++) {
				VectorSet( point, pm->ps->origin[0] + i, pm->ps->origin[0] + j, pm->ps->origin[0] + k );
				pm->trace( trace, point, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask );
				if ( !trace->allsolid ) {
					point[0] = pm->ps->origin[0];
					point[1] = pm->ps->origin[1];
					point[2] = pm->ps->origin[2] - 0.25;
					pm->trace( trace, pm->ps->origin, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask );
					pml.groundTrace = *trace;
					return qtrue;
				}
			}
		}
	}

	pm->ps->groundEntityNum = ENTITYNUM_NONE;
	pml.groundPlane = qfalse;
	pml.walking = qfalse;

	return qfalse;
}


// the ground trace didn't hit a surface, so we are in freefall

static void PM_GroundTraceMissed()
{
	trace_t		trace;
	vec3_t		point;

	if ( pm->ps->groundEntityNum != ENTITYNUM_NONE ) {
		// we just transitioned into freefall
		if ( pm->debugLevel ) {
			Com_Printf("%i:lift\n", c_pmove);
		}

		// if they aren't in a jumping animation and the ground is a ways away, force into it
		// if we didn't do the trace, the player would be backflipping down staircases
		VectorCopy( pm->ps->origin, point );
		point[2] -= 64;

		pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask );
		if ( trace.fraction == 1.0 ) {
			if ( pm->cmd.forwardmove >= 0 ) {
				PM_ForceLegsAnim( LEGS_JUMP );
				pm->ps->pm_flags &= ~PMF_BACKWARDS_JUMP;
			} else {
				PM_ForceLegsAnim( LEGS_JUMPB );
				pm->ps->pm_flags |= PMF_BACKWARDS_JUMP;
			}
		}
	}

	pm->ps->groundEntityNum = ENTITYNUM_NONE;
	pml.groundPlane = qfalse;
	pml.walking = qfalse;
}


static void PM_GroundTrace()
{
	vec3_t		point;
	trace_t		trace;

	point[0] = pm->ps->origin[0];
	point[1] = pm->ps->origin[1];
	point[2] = pm->ps->origin[2] - 0.25;

	pm->trace (&trace, pm->ps->origin, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask);
	pml.groundTrace = trace;

	// do something corrective if the trace starts in a solid...
	if ( trace.allsolid ) {
		if ( !PM_CorrectAllSolid(&trace) )
			return;
	}

	// if the trace didn't hit anything, we are in free fall
	if ( trace.fraction == 1.0 ) {
		PM_GroundTraceMissed();
		pml.groundPlane = qfalse;
		pml.walking = qfalse;
		return;
	}

	// check if getting thrown off the ground
	if ( pm->ps->velocity[2] > 0 && DotProduct( pm->ps->velocity, trace.plane.normal ) > 10 ) {
		if ( pm->debugLevel ) {
			Com_Printf("%i:kickoff\n", c_pmove);
		}
		// go into jump animation
		if ( pm->cmd.forwardmove >= 0 ) {
			PM_ForceLegsAnim( LEGS_JUMP );
			pm->ps->pm_flags &= ~PMF_BACKWARDS_JUMP;
		} else {
			PM_ForceLegsAnim( LEGS_JUMPB );
			pm->ps->pm_flags |= PMF_BACKWARDS_JUMP;
		}

		pm->ps->groundEntityNum = ENTITYNUM_NONE;
		pml.groundPlane = qfalse;
		pml.walking = qfalse;
		return;
	}
	
	// slopes that are too steep will not be considered onground
	if ( trace.plane.normal[2] < MIN_WALK_NORMAL ) {
		if ( pm->debugLevel ) {
			Com_Printf("%i:steep\n", c_pmove);
		}
		// FIXME: if they can't slide down the slope, let them
		// walk (sharp crevices)
		pm->ps->groundEntityNum = ENTITYNUM_NONE;
		pml.groundPlane = qtrue;
		pml.walking = qfalse;
		return;
	}

	pml.groundPlane = qtrue;
	pml.walking = qtrue;

	// hitting solid ground will end a waterjump
	if (pm->ps->pm_flags & PMF_TIME_WATERJUMP)
	{
		pm->ps->pm_flags &= ~(PMF_TIME_WATERJUMP | PMF_TIME_LAND);
		pm->ps->pm_time = 0;
	}

	if ( pm->ps->groundEntityNum == ENTITYNUM_NONE ) {
		// just hit the ground
		if ( pm->debugLevel ) {
			Com_Printf("%i:Land\n", c_pmove);
		}
		
		PM_CrashLand();

		// don't do landing time if we were just going down a slope
		if ( pml.previous_velocity[2] < -200 ) {
			// don't allow another jump for a little while
			pm->ps->pm_flags |= PMF_TIME_LAND;
			pm->ps->pm_time = 250;
		}
	}

	pm->ps->groundEntityNum = trace.entityNum;

	// don't reset the z velocity for slopes
//	pm->ps->velocity[2] = 0;

	PM_AddTouchEnt( trace.entityNum );
}


// FIXME: avoid doing this twice?  certainly if not moving

static void PM_SetWaterLevel()
{
	vec3_t point;

	// get waterlevel, accounting for ducking
	pm->waterlevel = WATERLEVEL_NONE;

	point[0] = pm->ps->origin[0];
	point[1] = pm->ps->origin[1];

	point[2] = pm->ps->origin[2] + playerMins[2] + 1;
	pm->watertype = pm->pointcontents( point, pm->ps->clientNum );
	if (!(pm->watertype & MASK_WATER))
		return;
	pm->waterlevel = WATERLEVEL_SHALLOW;

	point[2] = pm->ps->origin[2];
	if (!(pm->pointcontents( point, pm->ps->clientNum ) & MASK_WATER))
		return;
	pm->waterlevel = WATERLEVEL_DEEP;

	point[2] = pm->ps->origin[2] + pm->ps->viewheight;
	if (!(pm->pointcontents( point, pm->ps->clientNum ) & MASK_WATER))
		return;
	pm->waterlevel = WATERLEVEL_DROWN;
}


// sets mins, maxs, and pm->ps->viewheight

static void PM_CheckDuck()
{
	VectorCopy( playerMins, pm->mins );
	VectorCopy( playerMaxs, pm->maxs );

#ifdef MISSIONPACK
	if ( pm->ps->powerups[PW_INVULNERABILITY] ) {
		if ( pm->ps->pm_flags & PMF_INVULEXPAND ) {
			// invulnerability sphere has a 42 units radius
			VectorSet( pm->mins, -42, -42, -42 );
			VectorSet( pm->maxs, 42, 42, 42 );
		}
		pm->ps->pm_flags |= PMF_DUCKED;
		pm->ps->viewheight = CROUCH_VIEWHEIGHT;
		return;
	}
	pm->ps->pm_flags &= ~PMF_INVULEXPAND;
#endif

	if (pm->ps->pm_type == PM_DEAD) {
		pm->maxs[2] = -8;
		pm->ps->viewheight = DEAD_VIEWHEIGHT;
		return;
	}

	if (pm->cmd.upmove < 0) {	// duck
		pm->ps->pm_flags |= PMF_DUCKED;
	}
	else if (pm->ps->pm_flags & PMF_DUCKED) {
		// stand up if possible
		trace_t trace;
		pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, pm->ps->origin, pm->ps->clientNum, pm->tracemask );
		if (!trace.allsolid)
			pm->ps->pm_flags &= ~PMF_DUCKED;
	}

	if (pm->ps->pm_flags & PMF_DUCKED)
	{
		pm->maxs[2] = 16;
		pm->ps->viewheight = CROUCH_VIEWHEIGHT;
	}
	else
	{
		pm->maxs[2] = playerMaxs[2];
		pm->ps->viewheight = DEFAULT_VIEWHEIGHT;
	}
}


///////////////////////////////////////////////////////////////


static void PM_Footsteps()
{
	float		bobmove;
	int			oldCycle;
	qboolean	footstep;

	// calculate speed and cycle to be used for all cyclic walking effects
	pm->xyspeed = sqrt((pm->ps->velocity[0] * pm->ps->velocity[0]) + (pm->ps->velocity[1] * pm->ps->velocity[1]));

	if ( pm->ps->groundEntityNum == ENTITYNUM_NONE ) {
#ifdef MISSIONPACK
		if ( pm->ps->powerups[PW_INVULNERABILITY] )
			PM_ContinueLegsAnim( LEGS_IDLECR );
#endif
		// airborne leaves position in cycle intact, but doesn't advance
		if (pm->waterlevel > WATERLEVEL_SHALLOW)
			PM_ContinueLegsAnim( LEGS_SWIM );
		return;
	}

	// if not trying to move
	if ( !pm->cmd.forwardmove && !pm->cmd.rightmove ) {
		if (  pm->xyspeed < 5 ) {
			pm->ps->bobCycle = 0;	// start at beginning of cycle again
			PM_ContinueLegsAnim( (pm->ps->pm_flags & PMF_DUCKED) ? LEGS_IDLECR : LEGS_IDLE );
		}
		return;
	}

	footstep = qfalse;

	if (pm->ps->pm_flags & PMF_DUCKED) {
		bobmove = 0.5f; // ducked characters bob much faster
		PM_ContinueLegsAnim( (pm->ps->pm_flags & PMF_BACKWARDS_RUN) ? LEGS_BACKCR : LEGS_WALKCR );
	}
	else if (pm->cmd.buttons & BUTTON_WALKING) {
		bobmove = 0.3f; // walking bobs slower
		PM_ContinueLegsAnim( (pm->ps->pm_flags & PMF_BACKWARDS_RUN) ? LEGS_BACKWALK : LEGS_WALK );
	} else {
		bobmove = 0.4f; // running bobs faster
		PM_ContinueLegsAnim( (pm->ps->pm_flags & PMF_BACKWARDS_RUN) ? LEGS_BACK : LEGS_RUN );
		footstep = qtrue;
	}

	oldCycle = pm->ps->bobCycle;
	pm->ps->bobCycle = (int)( oldCycle + bobmove * pml.msec ) & 255;

	// if we just crossed a cycle boundary, play an apropriate footstep event
	if ( ( ( oldCycle + 64 ) ^ ( pm->ps->bobCycle + 64 ) ) & 128 ) {
		if (pm->waterlevel == WATERLEVEL_NONE) {
			// on ground will only play sounds if running
			if ( footstep && !pm->noFootsteps ) {
				PM_AddEvent( PM_FootstepForSurface() );
			}
		} else if (pm->waterlevel == WATERLEVEL_SHALLOW) {
			// splashing
			PM_AddEvent( EV_FOOTSPLASH );
		} else if (pm->waterlevel == WATERLEVEL_DEEP) {
			// wading / swimming at surface
			PM_AddEvent( EV_SWIM );
		} else if (pm->waterlevel == WATERLEVEL_DROWN) {
			// no sound when completely underwater
		}
	}
}


// generate sound events for entering and leaving water

static void PM_WaterEvents()
{
	// if just entered a water volume, play a sound
	if (!pml.previous_waterlevel && pm->waterlevel) {
		PM_AddEvent( EV_WATER_TOUCH );
	}

	// if just completely exited a water volume, play a sound
	if (pml.previous_waterlevel && !pm->waterlevel) {
		PM_AddEvent( EV_WATER_LEAVE );
	}

	if (pm->ps->pm_type != PM_DEAD) {
		// check for head just going under water
		if ((pml.previous_waterlevel != WATERLEVEL_DROWN) && (pm->waterlevel == WATERLEVEL_DROWN)) {
			PM_AddEvent( EV_WATER_UNDER );
		}

		// check for head just coming out of water
		// CPMA  since batsuit gives air, players aren't actually holding their breath
		if (!pm->ps->powerups[PW_BATTLESUIT] && (pml.previous_waterlevel == WATERLEVEL_DROWN) && (pm->waterlevel != WATERLEVEL_DROWN)) {
			PM_AddEvent( EV_WATER_CLEAR );
		}
	}
}


static void PM_BeginWeaponChange( int weapon )
{
	if ((weapon <= WP_NONE) || (weapon >= WP_NUM_WEAPONS))
		return;

	if (!(pm->ps->stats[STAT_WEAPONS] & (1 << weapon)))
		return;

	if (pm->ps->weaponstate == WEAPON_DROPPING)
		return;

	PM_AddEvent( EV_CHANGE_WEAPON );
	pm->ps->weaponstate = WEAPON_DROPPING;
	pm->ps->weaponTime += WEAPON_DROP_TIME;
	PM_StartTorsoAnim( TORSO_DROP );
}


static void PM_FinishWeaponChange()
{
	int weapon = pm->cmd.weapon;

	if ((weapon <= WP_NONE) || (weapon >= WP_NUM_WEAPONS) || !(pm->ps->stats[STAT_WEAPONS] & (1 << weapon))) {
		pm->ps->weapon = WP_NONE;
		return;
	}

	pm->ps->weapon = weapon;
	pm->ps->weaponstate = WEAPON_RAISING;
	pm->ps->weaponTime += WEAPON_RAISE_TIME;
	PM_StartTorsoAnim( TORSO_RAISE );
}


// generates weapon events and modifes the weapon counter

static void PM_Weapon()
{
	int addTime = 0;

	// don't allow attack until all buttons are up
	if ( pm->ps->pm_flags & PMF_RESPAWNED ) {
		return;
	}

	// ignore if spectator
	if ( pm->ps->persistant[PERS_TEAM] == TEAM_SPECTATOR ) {
		return;
	}

	// check for dead player
	if ( pm->ps->stats[STAT_HEALTH] <= 0 ) {
		pm->ps->weapon = WP_NONE;
		return;
	}

	// check for item using
	if ( pm->cmd.buttons & BUTTON_USE_HOLDABLE ) {
		if ( ! ( pm->ps->pm_flags & PMF_USE_ITEM_HELD ) ) {
			if ( bg_itemlist[pm->ps->stats[STAT_HOLDABLE_ITEM]].giTag == HI_MEDKIT
				&& pm->ps->stats[STAT_HEALTH] >= (pm->ps->stats[STAT_MAX_HEALTH] + 25) ) {
				// don't use medkit if at max health
			} else {
				pm->ps->pm_flags |= PMF_USE_ITEM_HELD;
				PM_AddEvent( EV_USE_ITEM0 + bg_itemlist[pm->ps->stats[STAT_HOLDABLE_ITEM]].giTag );
				pm->ps->stats[STAT_HOLDABLE_ITEM] = 0;
			}
			return;
		}
	} else {
		pm->ps->pm_flags &= ~PMF_USE_ITEM_HELD;
	}

	// make weapon function
	if ( pm->ps->weaponTime > 0 ) {
		pm->ps->weaponTime -= pml.msec;
	}

	// check for weapon change
	// can't change if weapon is firing, but can change again if lowering or raising
	if ( pm->ps->weaponTime <= 0 || pm->ps->weaponstate != WEAPON_FIRING ) {
		if ( pm->ps->weapon != pm->cmd.weapon ) {
			PM_BeginWeaponChange( pm->cmd.weapon );
		}
	}

	if ( pm->ps->weaponTime > 0 ) {
		return;
	}

	// change weapon if time
	if ( pm->ps->weaponstate == WEAPON_DROPPING ) {
		PM_FinishWeaponChange();
		return;
	}

	if ( pm->ps->weaponstate == WEAPON_RAISING ) {
		pm->ps->weaponstate = WEAPON_READY;
		PM_StartTorsoAnim( (pm->ps->weapon == WP_GAUNTLET) ? TORSO_STAND2 : TORSO_STAND );
		return;
	}

	// check for fire
	if ( !(pm->cmd.buttons & BUTTON_ATTACK) ) {
		pm->ps->weaponTime = 0;
		pm->ps->weaponstate = WEAPON_READY;
		return;
	}

	// start the animation even if out of ammo
	if ( pm->ps->weapon == WP_GAUNTLET ) {
		// the gauntlet only "fires" when it actually hits something
		if ( !pm->gauntletHit ) {
			pm->ps->weaponTime = 0;
			pm->ps->weaponstate = WEAPON_READY;
			return;
		}
		PM_StartTorsoAnim( TORSO_ATTACK2 );
	} else {
		PM_StartTorsoAnim( TORSO_ATTACK );
	}

	pm->ps->weaponstate = WEAPON_FIRING;

	// check for out of ammo
	if ( ! pm->ps->ammo[ pm->ps->weapon ] ) {
		PM_AddEvent( EV_NOAMMO );
		pm->ps->weaponTime += WEAPON_EMPTY_TIME;
		return;
	}

	// take an ammo away if not infinite
	if ( pm->ps->ammo[ pm->ps->weapon ] != -1 ) {
		pm->ps->ammo[ pm->ps->weapon ]--;
	}

	// fire weapon
	PM_AddEvent( EV_FIRE_WEAPON );

	switch( pm->ps->weapon ) {
	default:
	case WP_GAUNTLET:
		addTime = 400;
		break;
	case WP_LIGHTNING:
		addTime = 50;
		break;
	case WP_SHOTGUN:
		addTime = 1000;
		break;
	case WP_MACHINEGUN:
		addTime = 100;
		break;
	case WP_GRENADE_LAUNCHER:
		addTime = 800;
		break;
	case WP_ROCKET_LAUNCHER:
		addTime = 800;
		break;
	case WP_PLASMAGUN:
		addTime = 100;
		break;
	case WP_RAILGUN:
		addTime = 1500;
		break;
	case WP_BFG:
		addTime = 200;
		break;
	case WP_GRAPPLING_HOOK:
		addTime = 400;
		break;
#ifdef MISSIONPACK
	case WP_NAILGUN:
		addTime = 1000;
		break;
	case WP_PROX_LAUNCHER:
		addTime = 800;
		break;
	case WP_CHAINGUN:
		addTime = 30;
		break;
#endif
	}

#ifdef MISSIONPACK
	if( bg_itemlist[pm->ps->stats[STAT_PERSISTANT_POWERUP]].giTag == PW_SCOUT ) {
		addTime /= 1.5;
	}
	else
	if( bg_itemlist[pm->ps->stats[STAT_PERSISTANT_POWERUP]].giTag == PW_AMMOREGEN ) {
		addTime /= 1.3;
	}
	else
#endif
	if ( pm->ps->powerups[PW_HASTE] ) {
		addTime *= 0.75;
	}

	assert( addTime );
	pm->ps->weaponTime += addTime;
}


static void PM_Animate()
{
	if ( pm->cmd.buttons & BUTTON_GESTURE ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_GESTURE );
			pm->ps->torsoTimer = TIMER_GESTURE;
			PM_AddEvent( EV_TAUNT );
		}
#ifdef MISSIONPACK
	} else if ( pm->cmd.buttons & BUTTON_GETFLAG ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_GETFLAG );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_GUARDBASE ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_GUARDBASE );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_PATROL ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_PATROL );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_FOLLOWME ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_FOLLOWME );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_AFFIRMATIVE ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_AFFIRMATIVE);
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_NEGATIVE ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_NEGATIVE );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
#endif
	}
}


static void PM_DropTimers()
{
	// drop misc timing counter
	if ( pm->ps->pm_time ) {
		if ( pml.msec >= pm->ps->pm_time ) {
			pm->ps->pm_flags &= ~PMF_ALL_TIMES;
			pm->ps->pm_time = 0;
		} else {
			pm->ps->pm_time -= pml.msec;
		}
	}

	// drop animation counter
	if ( pm->ps->legsTimer > 0 ) {
		pm->ps->legsTimer -= pml.msec;
		if ( pm->ps->legsTimer < 0 ) {
			pm->ps->legsTimer = 0;
		}
	}

	if ( pm->ps->torsoTimer > 0 ) {
		pm->ps->torsoTimer -= pml.msec;
		if ( pm->ps->torsoTimer < 0 ) {
			pm->ps->torsoTimer = 0;
		}
	}
}


// this can be used as another entry point when only the viewangles are being updated instead of a full move

void PM_UpdateViewAngles( playerState_t* ps, const usercmd_t* cmd )
{
	int i;

	if (ps->pm_type == PM_INTERMISSION || ps->pm_type == PM_SPINTERMISSION)
		return;		// no view changes at all

	if ((ps->pm_type != PM_SPECTATOR) && (ps->stats[STAT_HEALTH] <= 0))
		return;		// no view changes at all

	// circularly clamp the angles with deltas
	for (i = 0; i < 3; i++) {
		short temp = cmd->angles[i] + ps->delta_angles[i];
		if ( i == PITCH ) {
			// don't let the player look up or down more than about 90 degrees
			if ( temp > 16000 ) {
				ps->delta_angles[i] = 16000 - cmd->angles[i];
				temp = 16000;
			} else if ( temp < -16000 ) {
				ps->delta_angles[i] = -16000 - cmd->angles[i];
				temp = -16000;
			}
		}
		ps->viewangles[i] = SHORT2ANGLE(temp);
	}
}


static void PmoveSingle( pmove_t* pmove )
{
	pm = pmove;

	// this counter lets us debug movement problems with a journal
	// by setting a conditional breakpoint fot the previous frame
	c_pmove++;

	// clear results
	pm->numtouch = 0;
	pm->watertype = 0;
	pm->waterlevel = WATERLEVEL_NONE;

	if ( pm->ps->stats[STAT_HEALTH] <= 0 ) {
		pm->tracemask &= ~CONTENTS_BODY;	// corpses can fly through bodies
	}

	// make sure walking button is clear if they are running,
	// to avoid proxy no-footsteps cheats
	if ( abs( pm->cmd.forwardmove ) > 64 || abs( pm->cmd.rightmove ) > 64 ) {
		pm->cmd.buttons &= ~BUTTON_WALKING;
	}

	// set the talk balloon flag
	if ( pm->cmd.buttons & BUTTON_TALK ) {
		pm->ps->eFlags |= EF_TALK;
	} else {
		pm->ps->eFlags &= ~EF_TALK;
	}

	// set the firing flag for continuous beam weapons
	if ( !(pm->ps->pm_flags & PMF_RESPAWNED) && pm->ps->pm_type != PM_INTERMISSION
		&& ( pm->cmd.buttons & BUTTON_ATTACK ) && pm->ps->ammo[ pm->ps->weapon ] ) {
		pm->ps->eFlags |= EF_FIRING;
	} else {
		pm->ps->eFlags &= ~EF_FIRING;
	}

	// clear the respawned flag if attack and use are cleared
	if ( pm->ps->stats[STAT_HEALTH] > 0 && 
		!( pm->cmd.buttons & (BUTTON_ATTACK | BUTTON_USE_HOLDABLE) ) ) {
		pm->ps->pm_flags &= ~PMF_RESPAWNED;
	}

	// if talk button is down, disallow all other input
	// this is to prevent any possible intercept proxy from
	// adding fake talk balloons
	if ( pmove->cmd.buttons & BUTTON_TALK ) {
		// keep the talk button set tho for when the cmd.serverTime > 66 msec
		// and the same cmd is used multiple times in Pmove
		pmove->cmd.buttons = BUTTON_TALK;
		pmove->cmd.forwardmove = 0;
		pmove->cmd.rightmove = 0;
		pmove->cmd.upmove = 0;
	}

	// clear all pmove local vars
	memset (&pml, 0, sizeof(pml));

	// determine the time
	pml.msec = pmove->cmd.serverTime - pm->ps->commandTime;
	if ( pml.msec < 1 ) {
		pml.msec = 1;
	} else if ( pml.msec > 200 ) {
		pml.msec = 200;
	}
	pm->ps->commandTime = pmove->cmd.serverTime;

	// save old org in case we get stuck
	VectorCopy (pm->ps->origin, pml.previous_origin);

	// save old velocity for crashlanding
	VectorCopy (pm->ps->velocity, pml.previous_velocity);

	pml.frametime = pml.msec * 0.001;

	AngleVectors (pm->ps->viewangles, pml.forward, pml.right, pml.up);

	if ( pm->cmd.upmove < 10 ) {
		// not holding jump
		pm->ps->pm_flags &= ~PMF_JUMP_HELD;
	}

	// decide if backpedaling animations should be used
	if ( pm->cmd.forwardmove < 0 ) {
		pm->ps->pm_flags |= PMF_BACKWARDS_RUN;
	} else if ( pm->cmd.forwardmove > 0 || ( pm->cmd.forwardmove == 0 && pm->cmd.rightmove ) ) {
		pm->ps->pm_flags &= ~PMF_BACKWARDS_RUN;
	}

	if ( pm->ps->pm_type >= PM_DEAD ) {
		pm->cmd.forwardmove = 0;
		pm->cmd.rightmove = 0;
		pm->cmd.upmove = 0;
	}

	if ( pm->ps->pm_type == PM_SPECTATOR ) {
		PM_CheckDuck ();
		PM_FlyMove ();
		PM_DropTimers ();
		return;
	}

	if ( pm->ps->pm_type == PM_NOCLIP ) {
		PM_NoclipMove ();
		PM_DropTimers ();
		return;
	}

	if (pm->ps->pm_type == PM_FREEZE) {
		return;		// no movement at all
	}

	if ( pm->ps->pm_type == PM_INTERMISSION || pm->ps->pm_type == PM_SPINTERMISSION) {
		return;		// no movement at all
	}

	// set watertype, and waterlevel
	PM_SetWaterLevel();
	pml.previous_waterlevel = pmove->waterlevel;

	// set mins, maxs, and viewheight
	PM_CheckDuck();

	// set groundentity
	PM_GroundTrace();

	if ( pm->ps->pm_type == PM_DEAD )
		PM_DeadMove();

	PM_DropTimers();

#ifdef MISSIONPACK
	if ( pm->ps->powerups[PW_INVULNERABILITY] ) {
		PM_InvulnerabilityMove();
	} else
#endif
	if ( pm->ps->powerups[PW_FLIGHT] ) {
		// flight powerup doesn't allow jump and has different friction
		PM_FlyMove();
	} else if (pm->ps->pm_flags & PMF_GRAPPLE_PULL) {
		PM_GrappleMove();
		// We can wiggle a bit
		PM_AirMove();
	} else if (pm->ps->pm_flags & PMF_TIME_WATERJUMP) {
		PM_WaterJumpMove();
	} else if ( pm->waterlevel > WATERLEVEL_SHALLOW ) {
		// swimming
		PM_WaterMove();
	} else if ( pml.walking ) {
		// walking on ground
		PM_WalkMove();
	} else {
		// airborne
		PM_AirMove();
	}

	PM_Animate();

	// set groundentity, watertype, and waterlevel
	PM_GroundTrace();
	PM_SetWaterLevel();

	PM_Weapon();

	PM_TorsoAnimation();

	// footstep events / legs animations
	PM_Footsteps();

	// entering / leaving water splashes
	PM_WaterEvents();
}


// sole entry point for ALL movement code: can be called by either the server or the client

void Pmove( pmove_t* pmove )
{
	int finalTime = pmove->cmd.serverTime;

	if ( finalTime < pmove->ps->commandTime ) {
		assert( 0 );
		return; // should not happen
	}

	if ( finalTime > pmove->ps->commandTime + 1000 ) {
		pmove->ps->commandTime = finalTime - 1000;
	}

	pmove->ps->pmove_framecount = (pmove->ps->pmove_framecount+1) & ((1<<PS_PMOVEFRAMECOUNTBITS)-1);

	PM_UpdateViewAngles( pmove->ps, &pmove->cmd );

	// chop the move up if it is too long, to prevent framerate-dependent behavior
	while ( pmove->ps->commandTime != finalTime ) {
		int msec = finalTime - pmove->ps->commandTime;

		if ( pmove->pmove_fixed ) {
			if ( msec > pmove->pmove_msec ) {
				msec = pmove->pmove_msec;
			}
		}
		else {
			if ( msec > 66 ) {
				msec = 66;
			}
		}
		pmove->cmd.serverTime = pmove->ps->commandTime + msec;
		PmoveSingle( pmove );

		if ( pmove->ps->pm_flags & PMF_JUMP_HELD ) {
			pmove->cmd.upmove = 20;
		}
	}

	//PM_CheckStuck();
}

