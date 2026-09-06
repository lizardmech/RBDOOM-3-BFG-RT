#ifndef __VERTEXCACHEHANDLE_H__
#define __VERTEXCACHEHANDLE_H__

#if 1

	// RB: quadruppled static memory limits for custom content

	const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
	const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
	const int VERTCACHE_JOINT_MEMORY_PER_FRAME = 256 * 1024;

	// there are a lot more static indexes than vertexes, because interactions are just new
	// index lists that reference existing vertexes
	const int STATIC_INDEX_MEMORY = 4 * 31 * 1024 * 1024;
	const int STATIC_VERTEX_MEMORY = 4 * 31 * 1024 * 1024;	// make sure it fits in VERTCACHE_OFFSET_MASK!

	// vertCacheHandle_t packs size, offset, and frame number into 64 bits
	typedef unsigned long long vertCacheHandle_t;
	const int VERTCACHE_STATIC = 1;						// in the static set, not the per-frame set

	const int VERTCACHE_SIZE_SHIFT = 1;
	const int VERTCACHE_SIZE_MASK = 0x7fffff;			// 23 bits = 8 megs

	const int VERTCACHE_OFFSET_SHIFT = 24;
	const int VERTCACHE_OFFSET_MASK = 0x7ffffff;		// 27 bits = 128 megs

	const int VERTCACHE_FRAME_SHIFT = 51;
	const int VERTCACHE_FRAME_MASK = 0x1fff;			// 13 bits = 8191 frames to wrap around


#else

	// RB: original values which are still good for low spec hardware and performance

	const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
	const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
	const int VERTCACHE_JOINT_MEMORY_PER_FRAME = 256 * 1024;

	// there are a lot more static indexes than vertexes, because interactions are just new
	// index lists that reference existing vertexes
	const int STATIC_INDEX_MEMORY = 31 * 1024 * 1024;
	const int STATIC_VERTEX_MEMORY = 31 * 1024 * 1024;	// make sure it fits in VERTCACHE_OFFSET_MASK!

	// vertCacheHandle_t packs size, offset, and frame number into 64 bits
	typedef unsigned long long vertCacheHandle_t;
	const int VERTCACHE_STATIC = 1;					// in the static set, not the per-frame set
	const int VERTCACHE_SIZE_SHIFT = 1;
	const int VERTCACHE_SIZE_MASK = 0x7fffff;		// 8 megs
	const int VERTCACHE_OFFSET_SHIFT = 24;
	const int VERTCACHE_OFFSET_MASK = 0x1ffffff;	// 32 megs
	const int VERTCACHE_FRAME_SHIFT = 49;
	const int VERTCACHE_FRAME_MASK = 0x7fff;		// 15 bits = 32k frames to wrap around, python hex( ( 1 << 15 ) - 1 )

#endif

inline bool VertCacheHandleIsCurrent( const vertCacheHandle_t handle, const int currentFrame )
{
	const int isStatic = handle & VERTCACHE_STATIC;
	if( isStatic )
	{
		return true;
	}
	const unsigned long long frameNum = ( int )( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
	if( frameNum != ( currentFrame & VERTCACHE_FRAME_MASK ) )
	{
		return false;
	}
	return true;
}

#endif // __VERTEXCACHEHANDLE_H__
