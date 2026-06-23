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
#include <cstdlib>
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

struct WarmImageJob
{
	sstring_t cacheName;
	sstring_t fileName;
	std::vector<byte> bytes;
};

typedef std::map<sstring_t, WarmImageEntry> WarmImageMap_t;
static WarmImageMap_t s_warmImages;
static std::vector<sstring_t> s_warmLoadedSamples;
static std::vector<sstring_t> s_warmMissSamples;
static std::mutex s_warmImagesMutex;
static std::vector<WarmImageJob> s_warmJobs;
static std::vector<std::thread> s_warmWorkers;
static std::atomic<bool> s_warmStop(false);
static std::atomic<bool> s_warmWorkerDone(true);
static std::atomic<int> s_warmWorkersRemaining(0);
static std::atomic<int> s_warmStartMs(0);
static std::atomic<int> s_warmEndMs(0);
static std::atomic<int> s_warmDurationMs(0);
static std::atomic<int> s_warmQueued(0);
static std::atomic<int> s_warmLoaded(0);
static std::atomic<int> s_warmFailed(0);
static std::atomic<int> s_warmHits(0);
static std::atomic<int> s_warmMisses(0);
static std::atomic<int> s_warmBspCandidates(0);
static std::atomic<int> s_warmStageCandidates(0);
static std::atomic<int> s_warmDecodeThreads(0);

static void *R_ImageWarm_PrivateAlloc( size_t size )
{
	return std::malloc( size );
}

static void R_ImageWarm_PrivateFree( void *ptr )
{
	std::free( ptr );
}

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
			R_ImageWarm_PrivateFree( it->second.pic );
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
		R_ImageWarm_PrivateFree( pic );
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

static qboolean R_ImageWarm_ReadFilePrivate( const char *name, std::vector<byte> &bytes )
{
	void *buffer = NULL;
	long len = 0;

	std::lock_guard<std::mutex> lock( s_imageLoadMutex );
	len = ri.FS_ReadFile( name, &buffer );
	if ( len <= 0 || !buffer )
	{
		return qfalse;
	}

	bytes.resize( len );
	memcpy( &bytes[0], buffer, len );
	ri.FS_FreeFile( buffer );
	return qtrue;
}

static void R_ImageWarm_ListFilesPrivate( const char *directory, const char *extension, std::vector<sstring_t> &files )
{
	int numFiles = 0;
	std::lock_guard<std::mutex> lock( s_imageLoadMutex );
	char **fileList = ri.FS_ListFiles( directory, extension, &numFiles );
	if ( !fileList )
	{
		return;
	}

	for ( int i = 0; i < numFiles; i++ )
	{
		files.push_back( sstring_t( fileList[i] ) );
	}

	ri.FS_FreeFileList( fileList );
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

	if ( it->second.width <= 0 || it->second.height <= 0 )
	{
		R_ImageWarm_PrivateFree( it->second.pic );
		s_warmImages.erase( it );
		s_warmMisses++;
		return qfalse;
	}

	const size_t imageSize = (size_t)it->second.width * (size_t)it->second.height * 4;
	if ( imageSize > 0x7fffffff )
	{
		R_ImageWarm_PrivateFree( it->second.pic );
		s_warmImages.erase( it );
		s_warmMisses++;
		return qfalse;
	}

	byte *handoffPic = (byte *)R_Malloc( (int)imageSize, TAG_TEMP_WORKSPACE, qfalse );
	if ( !handoffPic )
	{
		R_ImageWarm_PrivateFree( it->second.pic );
		s_warmImages.erase( it );
		s_warmMisses++;
		return qfalse;
	}

	memcpy( handoffPic, it->second.pic, imageSize );
	R_ImageWarm_PrivateFree( it->second.pic );
	*pic = handoffPic;
	*width = it->second.width;
	*height = it->second.height;
	s_warmImages.erase( it );
	s_warmHits++;
	return qtrue;
}

static void R_ImageWarm_AddBspShaders( const char *mapName, std::set<sstring_t> &images )
{
	std::vector<byte> bsp;
	if ( !R_ImageWarm_ReadFilePrivate( mapName, bsp ) )
	{
		return;
	}

	const long len = (long)bsp.size();
	const byte *bytes = &bsp[0];
	if ( len < (long)sizeof( dheader_t ) )
	{
		return;
	}

	const dheader_t *header = (const dheader_t *)bytes;
	const int version = LittleLong( header->version );
	if ( version != BSP_VERSION )
	{
		return;
	}

	lump_t shaderLump = header->lumps[LUMP_SHADERS];
	shaderLump.fileofs = LittleLong( shaderLump.fileofs );
	shaderLump.filelen = LittleLong( shaderLump.filelen );
	if ( shaderLump.fileofs < 0 || shaderLump.filelen < 0 ||
		 shaderLump.fileofs + shaderLump.filelen > len ||
		 shaderLump.filelen % (int)sizeof( dshader_t ) != 0 )
	{
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
	std::vector<sstring_t> shaderFiles;
	R_ImageWarm_ListFilesPrivate( "shaders", ".shader", shaderFiles );
	if ( shaderFiles.empty() )
	{
		return;
	}

	for ( std::vector<sstring_t>::const_iterator it = shaderFiles.begin(); it != shaderFiles.end(); ++it )
	{
		if ( s_warmStop.load() )
		{
			break;
		}

		char filename[MAX_QPATH];
		std::vector<byte> shaderText;
		Com_sprintf( filename, sizeof( filename ), "shaders/%s", it->c_str() );
		if ( R_ImageWarm_ReadFilePrivate( filename, shaderText ) )
		{
			shaderText.push_back( '\0' );
			R_ImageWarm_AddShaderStageImagesFromText( (const char *)&shaderText[0], shaderNames, images );
		}
	}
}

static qboolean R_ImageWarm_DecodePrivateBuffer( const char *name, std::vector<byte> &bytes, byte **pic, int *width, int *height )
{
	const char *extension = COM_GetExtension( name );
	if ( !Q_stricmp( extension, "jpg" ) || !Q_stricmp( extension, "jpeg" ) )
	{
		LoadJPGFromBufferWithAllocator( &bytes[0], bytes.size(), pic, width, height, R_ImageWarm_PrivateAlloc );
	}
	else if ( !Q_stricmp( extension, "png" ) )
	{
		LoadPNGFromBufferWithAllocator( &bytes[0], bytes.size(), pic, width, height, R_ImageWarm_PrivateAlloc, R_ImageWarm_PrivateFree );
	}
	else if ( !Q_stricmp( extension, "tga" ) )
	{
		LoadTGAFromBufferWithAllocator( name, &bytes[0], bytes.size(), pic, width, height, R_ImageWarm_PrivateAlloc );
	}

	return *pic ? qtrue : qfalse;
}

static qboolean R_ImageWarm_PrepareImageJob( const char *shortname, WarmImageJob &job )
{
	char extensionlessName[MAX_QPATH];
	const char *extension = COM_GetExtension( shortname );
	const char *tryExtensions[] = { "jpg", "png", "tga" };

	if ( extension && extension[0] )
	{
		std::vector<byte> bytes;
		if ( R_ImageWarm_ReadFilePrivate( shortname, bytes ) )
		{
			job.cacheName = sstring_t( shortname );
			job.fileName = sstring_t( shortname );
			job.bytes.swap( bytes );
			return qtrue;
		}
	}

	COM_StripExtension( shortname, extensionlessName, sizeof( extensionlessName ) );
	for ( size_t i = 0; i < ARRAY_LEN( tryExtensions ); i++ )
	{
		if ( extension && extension[0] && !Q_stricmp( extension, tryExtensions[i] ) )
		{
			continue;
		}

		char name[MAX_QPATH];
		std::vector<byte> bytes;
		Com_sprintf( name, sizeof( name ), "%s.%s", extensionlessName, tryExtensions[i] );
		if ( R_ImageWarm_ReadFilePrivate( name, bytes ) )
		{
			job.cacheName = sstring_t( shortname );
			job.fileName = sstring_t( name );
			job.bytes.swap( bytes );
			return qtrue;
		}
	}

	return qfalse;
}

static qboolean R_ImageWarm_LoadPreparedCandidate( WarmImageJob &job )
{
	byte *pic = NULL;
	int width = 0;
	int height = 0;
	R_ImageWarm_DecodePrivateBuffer( job.fileName.c_str(), job.bytes, &pic, &width, &height );
	if ( pic )
	{
		if ( s_warmStop.load() )
		{
			R_ImageWarm_PrivateFree( pic );
			return qfalse;
		}
		R_ImageWarm_Publish( job.cacheName.c_str(), pic, width, height );
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

static int R_ImageWarm_GetDecodeThreadCount( size_t jobCount )
{
	if ( jobCount == 0 )
	{
		return 0;
	}

	unsigned int hardwareThreads = std::thread::hardware_concurrency();
	int decodeThreads = 1;
	if ( hardwareThreads > 1 )
	{
		decodeThreads = (int)hardwareThreads - 1;
	}

	if ( decodeThreads < 1 )
	{
		decodeThreads = 1;
	}
	if ( decodeThreads > (int)jobCount )
	{
		decodeThreads = (int)jobCount;
	}

	return decodeThreads;
}

static void R_ImageWarm_DecodeWorker( std::atomic<size_t> *nextJob )
{
	for ( ;; )
	{
		if ( s_warmStop.load() )
		{
			break;
		}

		const size_t jobIndex = nextJob->fetch_add( 1 );
		if ( jobIndex >= s_warmJobs.size() )
		{
			break;
		}

		R_ImageWarm_LoadPreparedCandidate( s_warmJobs[jobIndex] );
	}

	if ( s_warmWorkersRemaining.fetch_sub( 1 ) == 1 )
	{
		R_ImageWarm_MarkWorkerDone( s_warmStartMs.load() );
	}
}

static void R_ImageWarm_StopWorkers( void )
{
	s_warmStop = true;
	for ( std::vector<std::thread>::iterator it = s_warmWorkers.begin(); it != s_warmWorkers.end(); ++it )
	{
		if ( it->joinable() )
		{
			it->join();
		}
	}
	s_warmWorkers.clear();
	s_warmJobs.clear();
	s_warmWorkersRemaining = 0;
}

static void R_ImageWarm_StartDecodeJobs( const std::set<sstring_t> &images )
{
	s_warmJobs.clear();
	s_warmJobs.reserve( images.size() );
	for ( std::set<sstring_t>::const_iterator it = images.begin(); it != images.end(); ++it )
	{
		WarmImageJob job;
		if ( R_ImageWarm_PrepareImageJob( it->c_str(), job ) )
		{
			s_warmJobs.push_back( job );
		}
		else
		{
			s_warmFailed++;
		}
	}

	const int decodeThreads = R_ImageWarm_GetDecodeThreadCount( s_warmJobs.size() );
	s_warmDecodeThreads = decodeThreads;
	if ( s_warmJobs.empty() )
	{
		R_ImageWarm_MarkWorkerDone( s_warmStartMs.load() );
		return;
	}

	static std::atomic<size_t> nextJob;
	nextJob = 0;
	s_warmWorkersRemaining = decodeThreads;
	s_warmWorkers.reserve( decodeThreads );
	for ( int i = 0; i < decodeThreads; i++ )
	{
		s_warmWorkers.push_back( std::thread( R_ImageWarm_DecodeWorker, &nextJob ) );
	}
}

void R_ImageWarm_StartMap( const char *mapName )
{
	if ( !mapName || !mapName[0] )
	{
		return;
	}

	R_ImageWarm_StopWorkers();

	const int startTime = ri.Milliseconds();
	std::set<sstring_t> images;
	std::set<sstring_t> bspShaders;
	size_t before = 0;

	s_warmStop = false;
	s_warmWorkerDone = false;
	s_warmStartMs = startTime;
	s_warmEndMs = 0;
	s_warmDurationMs = 0;
	s_warmQueued = 0;
	s_warmLoaded = 0;
	s_warmFailed = 0;
	s_warmHits = 0;
	s_warmMisses = 0;
	s_warmBspCandidates = 0;
	s_warmStageCandidates = 0;
	s_warmDecodeThreads = 0;
	R_ImageWarm_ClearCache();
	{
		std::lock_guard<std::mutex> lock( s_warmImagesMutex );
		s_warmLoadedSamples.clear();
		s_warmMissSamples.clear();
	}

	before = images.size();
	R_ImageWarm_AddBspShaders( mapName, images );
	s_warmBspCandidates = (int)( images.size() - before );
	for ( std::set<sstring_t>::const_iterator it = images.begin(); it != images.end(); ++it )
	{
		bspShaders.insert( R_ImageWarmCacheKey( it->c_str() ) );
	}

	before = images.size();
	R_ImageWarm_AddShaderStageImages( bspShaders, images );
	s_warmStageCandidates = (int)( images.size() - before );

	s_warmQueued = (int)images.size();
	R_ImageWarm_StartDecodeJobs( images );
}

void R_ImageWarm_Shutdown( void )
{
	R_ImageWarm_StopWorkers();
	s_warmWorkerDone = true;
	R_ImageWarm_ClearCache();
}

void R_ImageWarm_LogStats( void )
{
#if LOAD_LOGGING
	LoadLog_Append( "    WarmImageCache queued/loaded/failed/hits/misses: %d/%d/%d/%d/%d\n",
		s_warmQueued.load(), s_warmLoaded.load(), s_warmFailed.load(), s_warmHits.load(), s_warmMisses.load() );
	LoadLog_Append( "    WarmImageWorker done/ms: %d/%d\n",
		s_warmWorkerDone.load() ? 1 : 0, s_warmDurationMs.load() );
	LoadLog_Append( "    WarmImageDecodeThreads: %d\n",
		s_warmDecodeThreads.load() );
	LoadLog_Append( "    WarmImageSources bsp/stages: %d/%d\n",
		s_warmBspCandidates.load(), s_warmStageCandidates.load() );
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
		"map image warmer: queued=%d loaded=%d failed=%d hits=%d misses=%d retained=%d bsp=%d stages=%d decodeThreads=%d workerDone=%s warmMs=%d\n",
		s_warmQueued.load(), s_warmLoaded.load(), s_warmFailed.load(), s_warmHits.load(), s_warmMisses.load(),
		(int)s_warmImages.size(), s_warmBspCandidates.load(), s_warmStageCandidates.load(),
		s_warmDecodeThreads.load(), s_warmWorkerDone.load() ? "yes" : "no", s_warmDurationMs.load() );
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
