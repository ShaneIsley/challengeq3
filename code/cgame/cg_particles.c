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

#include "cg_local.h"


#define MAX_PARTICLES 8192
static particle_t particles[MAX_PARTICLES];
static particle_t* active_particles;
static particle_t* free_particles;

static vec3_t viewF, viewR, viewU;

//static int cParticles;


void Particles_Init()
{
	int i;

	free_particles = &particles[0];
	active_particles = NULL;

	for (i = 0; i < MAX_PARTICLES; ++i)
		particles[i].next = &particles[i+1];
	particles[MAX_PARTICLES-1].next = NULL;
}


typedef union {
	byte rgba[4];
	unsigned long packed;
} modulate_t;

static void AddParticleToScene( const particle_t* p, const vec3_t pos, const float alpha )
{
	vec3_t v;
	polyVert_t verts[4];
	modulate_t modulate;

	//assert( sizeof(modulate_t) == sizeof(unsigned long) == sizeof(verts[0].modulate) );
	modulate.rgba[0] = 255 * p->color[0];
	modulate.rgba[1] = 255 * p->color[1];
	modulate.rgba[2] = 255 * p->color[2];
	modulate.rgba[3] = 255 * alpha;

	VectorMA( pos, -p->radius, viewU, v );
	VectorMA( v, -p->radius, viewR, v );

	VectorCopy( v, verts[0].xyz );
	verts[0].st[0] = 0;
	verts[0].st[1] = 0;
	*(unsigned long*)&verts[0].modulate = modulate.packed;

	VectorMA( pos, -p->radius, viewU, v );
	VectorMA( v, p->radius, viewR, v );

	VectorCopy( v, verts[1].xyz );
	verts[1].st[0] = 0;
	verts[1].st[1] = 1;
	*(unsigned long*)&verts[1].modulate = modulate.packed;

	VectorMA( pos, p->radius, viewU, v );
	VectorMA( v, p->radius, viewR, v );

	VectorCopy( v, verts[2].xyz );
	verts[2].st[0] = 1;
	verts[2].st[1] = 1;
	*(unsigned long*)&verts[2].modulate = modulate.packed;

	VectorMA( pos, p->radius, viewU, v );
	VectorMA( v, -p->radius, viewR, v );

	VectorCopy( v, verts[3].xyz );
	verts[3].st[0] = 1;
	verts[3].st[1] = 0;
	*(unsigned long*)&verts[3].modulate = modulate.packed;

	trap_R_AddPolyToScene( cgs.media.particleShader, 4, verts );
	//++cParticles;
}


void Particles_Render()
{
	particle_t *p, *next, **prev;
	float life;
	vec3_t pos;

	// just to save massive dereference counts in the non-optimising vm
	VectorCopy( cg.refdef.viewaxis[0], viewF );
	VectorCopy( cg.refdef.viewaxis[1], viewR );
	VectorCopy( cg.refdef.viewaxis[2], viewU );

	//cParticles = 0;

	prev = &active_particles;
	for (p = active_particles; p; p = next) {
		next = p->next;

		// move expired particles to the free list
		if (cg.time >= p->endtime) {
			*prev = next;
			p->next = free_particles;
			free_particles = p;
			continue;
		}

		life = (cg.time - p->time);
		VectorMA( p->pos, life * 0.001, p->vel, pos );
		AddParticleToScene( p, pos, 1.0 - (life / (p->endtime - p->time)) );

		prev = &p->next;
	}

	//CG_Printf( "%d particles\n", cParticles );
}


particle_t* Particle_Alloc()
{
	particle_t* p;

	if (free_particles) {
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
	}

	return active_particles;
}
