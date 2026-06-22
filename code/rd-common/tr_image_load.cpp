/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include "../server/exe_headers.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../qcommon/load_timing.h"
#include "../qcommon/qfiles.h"
#include "../qcommon/sstring.h"
#include "../rd-common/tr_common.h"

const int MAX_IMAGE_LOADERS = 10;
struct ImageLoaderMap
{
	const char *extension;
	ImageLoaderFn loader;
} imageLoaders[MAX_IMAGE_LOADERS];
int numImageLoaders;

static std::mutex s_imageLoadMutex;

struct WarmImageEntry
{
	byte *pic;
	int width;
	int height;
};

typedef std::map<sstring_t, WarmImageEntry> WarmImageMap_t;
static WarmImageMap_t s_warmImages;
static std::vector<sstring_t> s_warmLoadedSamples;
static std::vector<sstring_t> s_warmMissSamples;
static std::mutex s_warmImagesMutex;
static std::thread s_warmThread;
static std::atomic<bool> s_warmStop(false);
static std::atomic<bool> s_warmWorkerDone(true);
static std::atomic<int> s_warmStartMs(0);
static std::atomic<int> s_warmEndMs(0);
static std::atomic<int> s_warmDurationMs(0);
static std::atomic<int> s_warmQueued(0);
static std::atomic<int> s_warmLoaded(0);
static std::atomic<int> s_warmFailed(0);
static std::atomic<int> s_warmHits(0);
static std::atomic<int> s_warmMisses(0);
static std::atomic<int> s_warmPriorityQueued(0);
static std::atomic<int> s_warmPriorityLoaded(0);
static std::atomic<int> s_warmPriorityFailed(0);
static std::atomic<bool> s_warmPriorityDone(false);
static std::atomic<int> s_warmStartupCandidates(0);
static std::atomic<int> s_warmBspCandidates(0);
static std::atomic<int> s_warmStageCandidates(0);
static std::atomic<int> s_warmDirectoryCandidates(0);

static void R_NormalizeImageLookupName( const char *source, char *dest, size_t destSize )
{
	size_t i = 0;

	if ( !destSize )
	{
		return;
	}

	while ( source && source[i] != '\0' && i < destSize - 1 )
	{
		char letter = (char)tolower( (unsigned char)source[i] );
		if ( letter == '\\' )
		{
			letter = '/';
		}
		dest[i++] = letter;
	}
	dest[i] = '\0';
}

static sstring_t R_ImageWarmCacheKey( const char *shortname )
{
	char extensionlessName[MAX_QPATH];
	char normalizedName[MAX_QPATH];
	COM_StripExtension( shortname, extensionlessName, sizeof( extensionlessName ) );
	R_NormalizeImageLookupName( extensionlessName, normalizedName, sizeof( normalizedName ) );
	return sstring_t( normalizedName );
}

static void R_ImageWarm_ClearCache( void )
{
	std::lock_guard<std::mutex> lock( s_warmImagesMutex );
	for ( WarmImageMap_t::iterator it = s_warmImages.begin(); it != s_warmImages.end(); ++it )
	{
		if ( it->second.pic )
		{
			R_Free( it->second.pic );
		}
	}
	s_warmImages.clear();
}

static void R_ImageWarm_Publish( const char *shortname, byte *pic, int width, int height )
{
	if ( !pic )
	{
		return;
	}

	const sstring_t key = R_ImageWarmCacheKey( shortname );
	std::lock_guard<std::mutex> lock( s_warmImagesMutex );
	WarmImageMap_t::iterator it = s_warmImages.find( key );
	if ( it != s_warmImages.end() && it->second.pic )
	{
		R_Free( pic );
		return;
	}

	WarmImageEntry entry;
	entry.pic = pic;
	entry.width = width;
	entry.height = height;
	s_warmImages[key] = entry;
	if ( s_warmLoadedSamples.size() < 24 )
	{
		s_warmLoadedSamples.push_back( key );
	}
}

qboolean R_ImageWarm_TakeImage( const char *shortname, byte **pic, int *width, int *height )
{
	if ( !shortname || !pic || !width || !height )
	{
		return qfalse;
	}

	const sstring_t key = R_ImageWarmCacheKey( shortname );
	std::lock_guard<std::mutex> lock( s_warmImagesMutex );
	WarmImageMap_t::iterator it = s_warmImages.find( key );
	if ( it == s_warmImages.end() || !it->second.pic )
	{
		s_warmMisses++;
		if ( s_warmMissSamples.size() < 24 )
		{
			s_warmMissSamples.push_back( key );
		}
		return qfalse;
	}

	*pic = it->second.pic;
	*width = it->second.width;
	*height = it->second.height;
	it->second.pic = NULL;
	s_warmImages.erase( it );
	s_warmHits++;
	return qtrue;
}

static void R_ImageWarm_AddBspShaders( const char *mapName, std::set<sstring_t> &images )
{
	void *buffer = NULL;
	const long len = ri.FS_ReadFile( mapName, &buffer );
	if ( len <= 0 || !buffer )
	{
		return;
	}

	const byte *bytes = (const byte *)buffer;
	if ( len < (long)sizeof( dheader_t ) )
	{
		ri.FS_FreeFile( buffer );
		return;
	}

	const dheader_t *header = (const dheader_t *)buffer;
	const int version = LittleLong( header->version );
	if ( version != BSP_VERSION )
	{
		ri.FS_FreeFile( buffer );
		return;
	}

	lump_t shaderLump = header->lumps[LUMP_SHADERS];
	shaderLump.fileofs = LittleLong( shaderLump.fileofs );
	shaderLump.filelen = LittleLong( shaderLump.filelen );
	if ( shaderLump.fileofs < 0 || shaderLump.filelen < 0 ||
		 shaderLump.fileofs + shaderLump.filelen > len ||
		 shaderLump.filelen % (int)sizeof( dshader_t ) != 0 )
	{
		ri.FS_FreeFile( buffer );
		return;
	}

	const int count = shaderLump.filelen / sizeof( dshader_t );
	const dshader_t *shaders = (const dshader_t *)( bytes + shaderLump.fileofs );
	for ( int i = 0; i < count; i++ )
	{
		if ( s_warmStop.load() )
		{
			break;
		}

		char name[MAX_QPATH];
		Q_strncpyz( name, shaders[i].shader, sizeof( name ) );
		if ( name[0] )
		{
			images.insert( sstring_t( name ) );
		}
	}

	ri.FS_FreeFile( buffer );
}

static qboolean R_ImageWarm_DirectoryLooksRelevant( const char *directory )
{
	if ( !directory )
	{
		return qfalse;
	}

	if ( !Q_stricmpn( directory, "kejim", 5 ) ||
		 !Q_stricmpn( directory, "imp", 3 ) ||
		 !Q_stricmpn( directory, "common", 6 ) )
	{
		return qtrue;
	}

	return qfalse;
}

static void R_ImageWarm_SkipWhitespaceAndComments( const char **text )
{
	const char *p = *text;
	for ( ;; )
	{
		while ( *p && isspace( (unsigned char)*p ) )
		{
			p++;
		}

		if ( p[0] == '/' && p[1] == '/' )
		{
			p += 2;
			while ( *p && *p != '\n' )
			{
				p++;
			}
			continue;
		}

		if ( p[0] == '/' && p[1] == '*' )
		{
			p += 2;
			while ( *p && !( p[0] == '*' && p[1] == '/' ) )
			{
				p++;
			}
			if ( *p )
			{
				p += 2;
			}
			continue;
		}

		break;
	}
	*text = p;
}

static qboolean R_ImageWarm_ParseToken( const char **text, char *token, size_t tokenSize )
{
	R_ImageWarm_SkipWhitespaceAndComments( text );

	const char *p = *text;
	if ( !*p )
	{
		token[0] = '\0';
		return qfalse;
	}

	if ( *p == '{' || *p == '}' )
	{
		token[0] = *p++;
		token[1] = '\0';
		*text = p;
		return qtrue;
	}

	size_t len = 0;
	while ( *p && !isspace( (unsigned char)*p ) && *p != '{' && *p != '}' )
	{
		if ( len + 1 < tokenSize )
		{
			token[len++] = *p;
		}
		p++;
	}
	token[len] = '\0';
	*text = p;
	return qtrue;
}

static void R_ImageWarm_AddShaderBlockStageImages( const char **text, std::set<sstring_t> &images )
{
	char token[MAX_QPATH];
	int depth = 1;

	while ( depth > 0 && R_ImageWarm_ParseToken( text, token, sizeof( token ) ) )
	{
		if ( s_warmStop.load() )
		{
			return;
		}

		if ( !strcmp( token, "{" ) )
		{
			depth++;
			continue;
		}

		if ( !strcmp( token, "}" ) )
		{
			depth--;
			continue;
		}

		if ( !Q_stricmp( token, "map" ) || !Q_stricmp( token, "clampmap" ) )
		{
			if ( R_ImageWarm_ParseToken( text, token, sizeof( token ) ) )
			{
				if ( token[0] && token[0] != '$' && token[0] != '*' )
				{
					images.insert( sstring_t( token ) );
				}
			}
		}
	}
}

static void R_ImageWarm_AddShaderStageImagesFromText( const char *text, const std::set<sstring_t> &shaderNames, std::set<sstring_t> &images )
{
	const char *p = text;
	char token[MAX_QPATH];
	char nextToken[MAX_QPATH];

	while ( R_ImageWarm_ParseToken( &p, token, sizeof( token ) ) )
	{
		if ( s_warmStop.load() )
		{
			return;
		}

		if ( token[0] == '{' || token[0] == '}' )
		{
			continue;
		}

		const char *afterName = p;
		if ( !R_ImageWarm_ParseToken( &afterName, nextToken, sizeof( nextToken ) ) )
		{
			return;
		}

		if ( strcmp( nextToken, "{" ) )
		{
			p = afterName;
			continue;
		}

		const sstring_t shaderKey = R_ImageWarmCacheKey( token );
		p = afterName;
		if ( shaderNames.find( shaderKey ) != shaderNames.end() )
		{
			R_ImageWarm_AddShaderBlockStageImages( &p, images );
		}
		else
		{
			int depth = 1;
			while ( depth > 0 && R_ImageWarm_ParseToken( &p, nextToken, sizeof( nextToken ) ) )
			{
				if ( !strcmp( nextToken, "{" ) )
				{
					depth++;
				}
				else if ( !strcmp( nextToken, "}" ) )
				{
					depth--;
				}
			}
		}
	}
}

static void R_ImageWarm_AddShaderStageImages( const std::set<sstring_t> &shaderNames, std::set<sstring_t> &images )
{
	int numShaderFiles = 0;
	char **shaderFiles = ri.FS_ListFiles( "shaders", ".shader", &numShaderFiles );
	if ( !shaderFiles )
	{
		return;
	}

	for ( int i = 0; i < numShaderFiles; i++ )
	{
		if ( s_warmStop.load() )
		{
			break;
		}

		char filename[MAX_QPATH];
		char *buffer = NULL;
		Com_sprintf( filename, sizeof( filename ), "shaders/%s", shaderFiles[i] );
		ri.FS_ReadFile( filename, (void **)&buffer );
		if ( buffer )
		{
			R_ImageWarm_AddShaderStageImagesFromText( buffer, shaderNames, images );
			ri.FS_FreeFile( buffer );
		}
	}

	ri.FS_FreeFileList( shaderFiles );
}

static void R_ImageWarm_AddDirectoryImagesForExtension( const char *directory, const char *extension, std::set<sstring_t> &images )
{
	int numFiles = 0;
	char **files = ri.FS_ListFiles( directory, extension, &numFiles );
	if ( !files )
	{
		return;
	}

	for ( int i = 0; i < numFiles; i++ )
	{
		if ( s_warmStop.load() )
		{
			break;
		}

		char path[MAX_QPATH];
		Com_sprintf( path, sizeof( path ), "%s/%s", directory, files[i] );
		images.insert( sstring_t( path ) );
	}

	ri.FS_FreeFileList( files );
}

static void R_ImageWarm_AddDirectoryImages( const char *directory, std::set<sstring_t> &images )
{
	R_ImageWarm_AddDirectoryImagesForExtension( directory, ".jpg", images );
	R_ImageWarm_AddDirectoryImagesForExtension( directory, ".png", images );
	R_ImageWarm_AddDirectoryImagesForExtension( directory, ".tga", images );
}

static void R_ImageWarm_AddLikelyStartupImages( std::set<sstring_t> &images )
{
	static const char *const directories[] =
	{
		"levelshots",
		"gfx/colors",
		"gfx/effects",
		"models/weapons2/blaster_r",
		"models/players/stormtrooper",
		"models/players/imperial",
		"models/players/gonk",
		"models/players/probe",
		"models/players/jan",
		"models/map_objects/imp_mine",
		"textures/common",
	};

	for ( size_t i = 0; i < ARRAY_LEN( directories ); i++ )
	{
		if ( s_warmStop.load() )
		{
			break;
		}
		R_ImageWarm_AddDirectoryImages( directories[i], images );
	}
}

static void R_ImageWarm_AddPriorityKejimImages( std::vector<sstring_t> &images )
{
	static const char *const priorityImages[] =
	{
		"levelshots/yavin_temple",
		"levelshots/kejim_post",
		"models/weapons2/blaster_r/blaster_w",
		"models/players/stormtrooper/torso_legs",
		"models/players/stormtrooper/helmet",
		"models/players/stormtrooper/shoulder",
		"models/players/stormtrooper/armor",
		"textures/common/caps",
		"textures/common/caps_glow",
		"models/players/imperial/boots_hips",
		"models/players/imperial/officer_legs",
		"models/players/imperial/officer_torso",
		"models/players/imperial/basic_hand",
		"models/players/imperial/key",
		"models/players/imperial/head",
		"models/players/imperial/face",
		"models/players/imperial/mouth_eyes",
		"models/players/gonk/gonk",
		"models/players/probe/probe_droid",
		"models/map_objects/imp_mine/turret_cannon_base",
		"models/map_objects/imp_mine/turret_glow",
		"gfx/colors/black",
		"models/map_objects/imp_mine/turret_cannon",
		"models/players/jan/legs",
		"models/players/jan/accesories",
		"models/players/jan/vest",
	};

	for ( size_t i = 0; i < ARRAY_LEN( priorityImages ); i++ )
	{
		images.push_back( sstring_t( priorityImages[i] ) );
	}
}

static qboolean R_ImageWarm_LoadCandidate( const char *name )
{
	byte *pic = NULL;
	int width = 0;
	int height = 0;
	R_LoadImage( name, &pic, &width, &height );
	if ( pic )
	{
		R_ImageWarm_Publish( name, pic, width, height );
		s_warmLoaded++;
		return qtrue;
	}

	s_warmFailed++;
	return qfalse;
}

static void R_ImageWarm_MarkWorkerDone( int startTime )
{
	const int endTime = ri.Milliseconds();
	s_warmEndMs = endTime;
	s_warmDurationMs = endTime - startTime;
	s_warmWorkerDone = true;
}

static void R_ImageWarm_AddLikelyKejimTextureDirs( std::set<sstring_t> &images )
{
	int numDirs = 0;
	char **dirs = ri.FS_ListFiles( "textures", "/", &numDirs );
	if ( !dirs )
	{
		return;
	}

	for ( int i = 0; i < numDirs; i++ )
	{
		if ( s_warmStop.load() )
		{
			break;
		}
		if ( !R_ImageWarm_DirectoryLooksRelevant( dirs[i] ) )
		{
			continue;
		}

		char directory[MAX_QPATH];
		Com_sprintf( directory, sizeof( directory ), "textures/%s", dirs[i] );
		R_ImageWarm_AddDirectoryImages( directory, images );
	}

	ri.FS_FreeFileList( dirs );
}

static void R_ImageWarm_Worker( void )
{
	const int startTime = ri.Milliseconds();
	std::set<sstring_t> images;
	std::set<sstring_t> bspShaders;
	std::vector<sstring_t> priorityImages;
	size_t before = 0;

	s_warmStartMs = startTime;
	R_ImageWarm_AddPriorityKejimImages( priorityImages );
	s_warmQueued = (int)priorityImages.size();
	s_warmPriorityQueued = (int)priorityImages.size();
	for ( std::vector<sstring_t>::const_iterator it = priorityImages.begin(); it != priorityImages.end(); ++it )
	{
		if ( s_warmStop.load() )
		{
			R_ImageWarm_MarkWorkerDone( startTime );
			return;
		}

		if ( R_ImageWarm_LoadCandidate( it->c_str() ) )
		{
			s_warmPriorityLoaded++;
		}
		else
		{
			s_warmPriorityFailed++;
		}
	}
	s_warmPriorityDone = true;

	before = images.size();
	R_ImageWarm_AddLikelyStartupImages( images );
	s_warmStartupCandidates = (int)( images.size() - before );

	before = images.size();
	R_ImageWarm_AddBspShaders( "maps/kejim_post.bsp", images );
	s_warmBspCandidates = (int)( images.size() - before );
	for ( std::set<sstring_t>::const_iterator it = images.begin(); it != images.end(); ++it )
	{
		bspShaders.insert( R_ImageWarmCacheKey( it->c_str() ) );
	}

	before = images.size();
	R_ImageWarm_AddShaderStageImages( bspShaders, images );
	s_warmStageCandidates = (int)( images.size() - before );

	before = images.size();
	R_ImageWarm_AddLikelyKejimTextureDirs( images );
	s_warmDirectoryCandidates = (int)( images.size() - before );

	for ( std::vector<sstring_t>::const_iterator it = priorityImages.begin(); it != priorityImages.end(); ++it )
	{
		images.erase( R_ImageWarmCacheKey( it->c_str() ) );
		images.erase( *it );
	}

	s_warmQueued = (int)( images.size() + priorityImages.size() );
	for ( std::set<sstring_t>::const_iterator it = images.begin(); it != images.end(); ++it )
	{
		if ( s_warmStop.load() )
		{
			break;
		}

		R_ImageWarm_LoadCandidate( it->c_str() );
	}
	R_ImageWarm_MarkWorkerDone( startTime );
}

void R_ImageWarm_StartKejimPost( void )
{
	if ( s_warmThread.joinable() )
	{
		if ( !s_warmWorkerDone.load() )
		{
			return;
		}
		s_warmThread.join();
	}

	s_warmStop = false;
	s_warmWorkerDone = false;
	s_warmStartMs = 0;
	s_warmEndMs = 0;
	s_warmDurationMs = 0;
	s_warmQueued = 0;
	s_warmLoaded = 0;
	s_warmFailed = 0;
	s_warmHits = 0;
	s_warmMisses = 0;
	s_warmPriorityQueued = 0;
	s_warmPriorityLoaded = 0;
	s_warmPriorityFailed = 0;
	s_warmPriorityDone = false;
	s_warmStartupCandidates = 0;
	s_warmBspCandidates = 0;
	s_warmStageCandidates = 0;
	s_warmDirectoryCandidates = 0;
	R_ImageWarm_ClearCache();
	{
		std::lock_guard<std::mutex> lock( s_warmImagesMutex );
		s_warmLoadedSamples.clear();
		s_warmMissSamples.clear();
	}
	s_warmThread = std::thread( R_ImageWarm_Worker );
}

void R_ImageWarm_Shutdown( void )
{
	s_warmStop = true;
	if ( s_warmThread.joinable() )
	{
		s_warmThread.join();
	}
	s_warmWorkerDone = true;
	R_ImageWarm_ClearCache();
}

void R_ImageWarm_LogStats( void )
{
#if LOAD_LOGGING
	LoadLog_Append( "    WarmImageCache queued/loaded/failed/hits/misses: %d/%d/%d/%d/%d\n",
		s_warmQueued.load(), s_warmLoaded.load(), s_warmFailed.load(), s_warmHits.load(), s_warmMisses.load() );
	LoadLog_Append( "    WarmImagePriority queued/loaded/failed/done: %d/%d/%d/%d\n",
		s_warmPriorityQueued.load(), s_warmPriorityLoaded.load(), s_warmPriorityFailed.load(), s_warmPriorityDone.load() ? 1 : 0 );
	LoadLog_Append( "    WarmImageWorker done/ms: %d/%d\n",
		s_warmWorkerDone.load() ? 1 : 0, s_warmDurationMs.load() );
	LoadLog_Append( "    WarmImageSources startup/bsp/stages/dirs: %d/%d/%d/%d\n",
		s_warmStartupCandidates.load(), s_warmBspCandidates.load(), s_warmStageCandidates.load(), s_warmDirectoryCandidates.load() );
	{
		std::lock_guard<std::mutex> lock( s_warmImagesMutex );
		LoadLog_Append( "    WarmImageCache retained: %d\n", (int)s_warmImages.size() );
		LoadLog_Append( "    WarmImageLoadedSamples:" );
		for ( size_t i = 0; i < s_warmLoadedSamples.size(); i++ )
		{
			LoadLog_Append( " %s", s_warmLoadedSamples[i].c_str() );
		}
		LoadLog_Append( "\n" );

		LoadLog_Append( "    WarmImageMissSamples:" );
		for ( size_t i = 0; i < s_warmMissSamples.size(); i++ )
		{
			LoadLog_Append( " %s", s_warmMissSamples[i].c_str() );
		}
		LoadLog_Append( "\n" );
	}
#endif
}

void R_ImageWarm_PrintStatus( void )
{
	std::lock_guard<std::mutex> lock( s_warmImagesMutex );
	ri.Printf( PRINT_ALL,
		"kejim_post image warmer: queued=%d loaded=%d failed=%d hits=%d misses=%d retained=%d priority=%d/%d failed=%d done=%s workerDone=%s warmMs=%d\n",
		s_warmQueued.load(), s_warmLoaded.load(), s_warmFailed.load(), s_warmHits.load(), s_warmMisses.load(),
		(int)s_warmImages.size(), s_warmPriorityLoaded.load(), s_warmPriorityQueued.load(),
		s_warmPriorityFailed.load(), s_warmPriorityDone.load() ? "yes" : "no", s_warmWorkerDone.load() ? "yes" : "no",
		s_warmDurationMs.load() );
}

/*
=================
Finds the image loader associated with the given extension.
=================
*/
const ImageLoaderMap *FindImageLoader ( const char *extension )
{
	for ( int i = 0; i < numImageLoaders; i++ )
	{
		if ( Q_stricmp (extension, imageLoaders[i].extension) == 0 )
		{
			return &imageLoaders[i];
		}
	}

	return NULL;
}

/*
=================
Adds a new image loader to load the specified image file extension.
The 'extension' string should not begin with a period (full stop).
=================
*/
qboolean R_ImageLoader_Add ( const char *extension, ImageLoaderFn imageLoader )
{
	if ( numImageLoaders >= MAX_IMAGE_LOADERS )
	{
		ri.Printf (PRINT_DEVELOPER, "R_AddImageLoader: Cannot add any more image loaders (maximum %d).\n", MAX_IMAGE_LOADERS);
		return qfalse;
	}

	if ( FindImageLoader (extension) != NULL )
	{
		ri.Printf (PRINT_DEVELOPER, "R_AddImageLoader: Image loader already exists for extension \"%s\".\n", extension);
		return qfalse;
	}

	ImageLoaderMap *newImageLoader = &imageLoaders[numImageLoaders];
	newImageLoader->extension = extension;
	newImageLoader->loader = imageLoader;

	numImageLoaders++;

	return qtrue;
}

/*
=================
Initializes the image loader, and adds the built-in
image loaders
=================
*/
void R_ImageLoader_Init()
{
	std::lock_guard<std::mutex> lock( s_imageLoadMutex );
	Com_Memset (imageLoaders, 0, sizeof (imageLoaders));
	numImageLoaders = 0;

	R_ImageLoader_Add ("jpg", LoadJPG);
	R_ImageLoader_Add ("png", LoadPNG);
	R_ImageLoader_Add ("tga", LoadTGA);
}

/*
=================
Loads any of the supported image types into a cannonical
32 bit format.
=================
*/
void R_LoadImage( const char *shortname, byte **pic, int *width, int *height ) {
	std::lock_guard<std::mutex> lock( s_imageLoadMutex );

	*pic = NULL;
	*width = 0;
	*height = 0;

	// Try loading the image with the original extension (if possible).
	const char *extension = COM_GetExtension (shortname);
	const ImageLoaderMap *imageLoader = FindImageLoader (extension);
	if ( imageLoader != NULL )
	{
		imageLoader->loader (shortname, pic, width, height);
		if ( *pic )
		{
			return;
		}
	}

	// Loop through all the image loaders trying to load this image.
	char extensionlessName[MAX_QPATH];
	COM_StripExtension(shortname, extensionlessName, sizeof( extensionlessName ));
	for ( int i = 0; i < numImageLoaders; i++ )
	{
		const ImageLoaderMap *tryLoader = &imageLoaders[i];
		if ( tryLoader == imageLoader )
		{
			// Already tried this one.
			continue;
		}

		const char *name = va ("%s.%s", extensionlessName, tryLoader->extension);
		tryLoader->loader (name, pic, width, height);
		if ( *pic )
		{
			return;
		}
	}
}
