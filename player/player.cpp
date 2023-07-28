/* How to play game music files with Music_Player (requires SDL library)

Run program with path to a game music file.

Left/Right  Change track
Up/Down     Volumen
Button A    Pause/unpause
Button B    Normal/slight stereo echo/more stereo echo
Button Y    Enable/disable accurate emulation
Button X    Toggle track looping (infinite playback)
R1/L1       Adjust tempo
1-9         Toggle channel on/off
Select      Reset tempo and turn channels back on */

// Make ISO C99 symbols available for snprintf, define must be set before any
// system header includes
#define _ISOC99_SOURCE 1
#define MI_AO_SETVOLUME 0x4008690b
#define MI_AO_GETVOLUME 0xc008690c

int const scope_width = 512;

#include "Music_Player.h"
#include "Audio_Scope.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <linux/input.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "SDL.h"

void handle_error( const char* );

static bool paused;
static Audio_Scope* scope;
static Music_Player* player;
static short scope_buf [scope_width * 2];

static void init()
{
	// Start SDL
	if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_AUDIO ) < 0 )
		exit( EXIT_FAILURE );
	atexit( SDL_Quit );
	SDL_EnableKeyRepeat( 500, 80 );
	
	// Init scope
	scope = new Audio_Scope;
	if ( !scope )
		handle_error( "Out of memory" );
	if ( scope->init( scope_width, 256 ) )
		handle_error( "Couldn't initialize scope" );
	memset( scope_buf, 0, sizeof scope_buf );
	
	// Create player
	player = new Music_Player;
	if ( !player )
		handle_error( "Out of memory" );
	handle_error( player->init() );
	player->set_scope_buffer( scope_buf, scope_width * 2 );
}

static void start_track( int track, const char* path )
{
	paused = false;
	handle_error( player->start_track( track - 1 ) );
	
	// update window title with track info
	
	long seconds = player->track_info().length / 1000;
	const char* game = player->track_info().game;
	if ( !*game )
	{
		// extract filename
		game = strrchr( path, '\\' ); // DOS
		if ( !game )
			game = strrchr( path, '/' ); // UNIX
		if ( !game )
			game = path;
		else
			game++; // skip path separator
	}
	
	char title [512];
	if ( 0 < snprintf( title, sizeof title, "%s: %d/%d %s (%ld:%02ld)",
			game, track, player->track_count(), player->track_info().song,
			seconds / 60, seconds % 60 ) )
	{
		SDL_WM_SetCaption( title, title );
	}
}

int main( int argc, char** argv )
{
	init();
	
	// Load file
	const char* path = (argc > 1 ? argv [argc - 1] : "test.nsf");
	handle_error( player->load_file( path ) );
	start_track( 1, path );
	
	// Main loop
	int track = 1;
	double tempo = 1.0;
	bool running = true;
	double stereo_depth = 0.0;
	bool accurate = false;
	bool fading_out = true;
	int muting_mask = 0;
	int fd = open("/dev/mi_ao", O_RDWR);
	
	while ( running )
	{
		SDL_Delay( 1000 / 100 );
		
		// Update scope
		scope->draw( scope_buf, scope_width, 2 );
		
		// Automatically go to next track when current one ends
		if ( player->track_ended() )
		{
			if ( track < player->track_count() )
				start_track( ++track, path );
			else
				player->pause( paused = true );
		}
		
		// Handle keyboard input
		SDL_Event e;
		while ( SDL_PollEvent( &e ) )
		{
			switch ( e.type )
			{
			case SDL_QUIT:
				running = false;
				break;
			
			case SDL_KEYDOWN:
				int key = e.key.keysym.sym;
				switch ( key )
				{
				case SDLK_ESCAPE: // quit
					running = false;
					break;
				
				case SDLK_LEFT: // prev track
					if ( !paused && !--track )
						track = 1;
					start_track( track, path );
					break;
				
				case SDLK_RIGHT: // next track
					if ( track < player->track_count() )
						start_track( ++track, path );
					break;
								
				case SDLK_UP: // up volumen
					if (fd >= 0) {
						int buf2[] = {0, 0};
						uint64_t buf1[] = {sizeof(buf2), (uintptr_t)buf2};
						
						// Get the current volume
						ioctl(fd, MI_AO_GETVOLUME, buf1);
						int recent_volume = buf2[1];
						
						// Decrease volume by 3
						buf2[1] += 3;
						
						// Clamp the volume to be within [-60, 0]
						if (buf2[1] > 0) 
							buf2[1] = 0;
						else if (buf2[1] < -60) 
							buf2[1] = -60;
						
						// Only set the volume if it's different from the recent one
						
						if (buf2[1] != recent_volume) 
							ioctl(fd, MI_AO_SETVOLUME, buf1);
					}
					break;
				
				case SDLK_DOWN: // down volumen
					if (fd >= 0) {
						int buf2[] = {0, 0};
						uint64_t buf1[] = {sizeof(buf2), (uintptr_t)buf2};
						
						// Get the current volume
						ioctl(fd, MI_AO_GETVOLUME, buf1);
						int recent_volume = buf2[1];
						
						// Decrease volume by 3
						buf2[1] -= 3;
						
						// Clamp the volume to be within [-60, 0]
						if (buf2[1] > 0) 
							buf2[1] = 0;
						else if (buf2[1] < -60) 
							buf2[1] = -60;
						
						// Only set the volume if it's different from the recent one
						
						if (buf2[1] != recent_volume) 
							ioctl(fd, MI_AO_SETVOLUME, buf1);
					}
					break;
				
				case SDLK_e: // reduce tempo
					tempo -= 0.1;
					if ( tempo < 0.1 )
						tempo = 0.1;
					player->set_tempo( tempo );
					break;
				
				case SDLK_t: // increase tempo
					tempo += 0.1;
					if ( tempo > 2.0 )
						tempo = 2.0;
					player->set_tempo( tempo );
					break;
				
				case SDLK_SPACE: // toggle pause
					paused = !paused;
					player->pause( paused );
					break;
				
				case SDLK_LALT: // toggle accurate emulation
					accurate = !accurate;
					player->enable_accuracy( accurate );
					break;
				
				case SDLK_LCTRL: // toggle echo
					stereo_depth += 0.2;
					if ( stereo_depth > 0.5 )
						stereo_depth = 0;
					player->set_stereo_depth( stereo_depth );
					break;
				
				case SDLK_LSHIFT: // toggle loop
					player->set_fadeout( fading_out = !fading_out );
					printf( "%s\n", fading_out ? "Will stop at track end" : "Playing forever" );
					break;
				
				case SDLK_RCTRL: // reset tempo and muting
					tempo = 1.0;
					muting_mask = 0;
					player->set_tempo( tempo );
					player->mute_voices( muting_mask );
					break;
				
				default:
					if ( SDLK_1 <= key && key <= SDLK_9 ) // toggle muting
					{
						muting_mask ^= 1 << (key - SDLK_1);
						player->mute_voices( muting_mask );
					}
				}
			}
		}
	}
	
	// Cleanup
	delete player;
	delete scope;
	
	return 0;
}

void handle_error( const char* error )
{
	if ( error )
	{
		// put error in window title
		char str [256];
		sprintf( str, "Error: %s", error );
		fprintf( stderr, "%s\n", str );
		SDL_WM_SetCaption( str, str );
		
		// wait for keyboard or mouse activity
		SDL_Event e;
		do
		{
			while ( !SDL_PollEvent( &e ) ) { }
		}
		while ( e.type != SDL_QUIT && e.type != SDL_KEYDOWN && e.type != SDL_MOUSEBUTTONDOWN );

		exit( EXIT_FAILURE );
	}
}
