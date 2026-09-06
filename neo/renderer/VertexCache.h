/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2016-2017 Dustin Land
Copyright (C) 2022 Stephen Pridham
Copyright (C) 2024 Robert Beckebans

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#ifndef __VERTEXCACHE_H__
#define __VERTEXCACHE_H__

#include <type_traits>

#include "VertexCacheHandle.h"

const int VERTEX_CACHE_ALIGN		= 32;
const int INDEX_CACHE_ALIGN			= 16;
const int JOINT_CACHE_ALIGN			= 16;

enum cacheType_t
{
	CACHE_VERTEX,
	CACHE_INDEX,
	CACHE_JOINT
};

struct geoBufferSet_t
{
	idIndexBuffer			indexBuffer;
	idVertexBuffer			vertexBuffer;
	idUniformBuffer			jointBuffer;
	byte* 					mappedVertexBase;
	byte* 					mappedIndexBase;
	byte* 					mappedJointBase;
	idSysInterlockedInteger	indexMemUsed;
	idSysInterlockedInteger	vertexMemUsed;
	idSysInterlockedInteger	jointMemUsed;
	int						allocations;	// number of index and vertex allocations combined
};

class idVertexCache
{
public:
	void			Init( int uniformBufferOffsetAlignment, nvrhi::ICommandList* commandList );
	void			Shutdown();
	void			PurgeAll( nvrhi::ICommandList* commandList );

	// call on loading a new map
	void			FreeStaticData();

	// this data is only valid for one frame of rendering
	vertCacheHandle_t	AllocVertex( const void* data, int num, size_t size = sizeof( idDrawVert ), nvrhi::ICommandList* commandList = nullptr );
	vertCacheHandle_t	AllocIndex( const void* data, int num, size_t size = sizeof( triIndex_t ), nvrhi::ICommandList* commandList = nullptr );
	vertCacheHandle_t	AllocJoint( const void* data, int num, size_t size = sizeof( idJointMat ), nvrhi::ICommandList* commandList = nullptr );

	// this data is valid until the next map load
	vertCacheHandle_t	AllocStaticVertex( const void* data, int bytes, nvrhi::ICommandList* commandList );
	vertCacheHandle_t	AllocStaticIndex( const void* data, int bytes, nvrhi::ICommandList* commandList );

	byte* 			MappedVertexBuffer( vertCacheHandle_t handle );
	byte* 			MappedIndexBuffer( vertCacheHandle_t handle );

	// Returns false if it's been purged
	// This can only be called by the front end, the back end should only be looking at
	// vertCacheHandle_t that are already validated.
	bool			CacheIsCurrent( const vertCacheHandle_t handle )
	{
		return VertCacheHandleIsCurrent( handle, currentFrame );
	}
	static bool		CacheIsStatic( const vertCacheHandle_t handle )
	{
		return ( handle & VERTCACHE_STATIC ) != 0;
	}

	// vb/ib is a temporary reference -- don't store it
	bool			GetVertexBuffer( vertCacheHandle_t handle, idVertexBuffer* vb );
	bool			GetIndexBuffer( vertCacheHandle_t handle, idIndexBuffer* ib );
	bool			GetJointBuffer( vertCacheHandle_t handle, idUniformBuffer* jb );

	void			BeginBackEnd();

public:
	int				currentFrame;	// for determining the active buffers
	int				listNum;		// currentFrame % NUM_FRAME_DATA
	int				drawListNum;	// (currentFrame-1) % NUM_FRAME_DATA

	geoBufferSet_t	staticData;
	geoBufferSet_t	frameData[ NUM_FRAME_DATA ];

	int				uniformBufferOffsetAlignment;

	// High water marks for the per-frame buffers
	int				mostUsedVertex;
	int				mostUsedIndex;
	int				mostUsedJoint;

	// Try to make room for <bytes> bytes
	vertCacheHandle_t	ActuallyAlloc( geoBufferSet_t& vcs, const void* data, int bytes, cacheType_t type, nvrhi::ICommandList* commandList );
};

// The harness cannot see this class, so the forwarding guarantee is asserted
// here, adjacent to the forwarder it guards.
static_assert(std::is_same<decltype(&idVertexCache::CacheIsCurrent),
		bool (idVertexCache::*)(vertCacheHandle_t)>::value,
	"idVertexCache::CacheIsCurrent must remain a bool(vertCacheHandle_t) forwarder");

// platform specific code to memcpy into vertex buffers efficiently
// 16 byte alignment is guaranteed
void CopyBuffer( byte* dst, const byte* src, int numBytes );

extern	idVertexCache	vertexCache;

#endif // __VERTEXCACHE_H__
