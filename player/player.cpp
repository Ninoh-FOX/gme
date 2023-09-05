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
Select      Reset tempo and turn channels back on
start       Toggle echo processing */

// Make ISO C99 symbols available for snprintf, define must be set before any
// system header includes
#define _ISOC99_SOURCE 1
#define MI_AO_SETVOLUME 0x4008690b
#define MI_AO_GETVOLUME 0xc008690c

int const scope_width = 512;

#include "Music_Player.h"
#include "Audio_Scope.h"
#include "cJSON.h"

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

char* load_file(char const* path) { // for read config file in miyoo mini
	char* buffer = 0;
	long length = 0;
	FILE * f = fopen(path, "rb"); //was "rb"
	
	if (f) {
		fseek(f, 0, SEEK_END);
		length = ftell(f);
		fseek(f, 0, SEEK_SET);
		buffer = (char*) malloc((length+1)*sizeof(char));
		if (buffer) {
			fread(buffer, sizeof(char), length, f);
		}
		fclose(f);
	}
	buffer[length] = '\0';
	
	return buffer;
}

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
	
	// Read config file in miyoo mini
	const char *settings_file = getenv("SETTINGS_FILE");
	if (settings_file == NULL) {
		FILE* pipe = popen("dmesg | fgrep '[FSP] Flash is detected (0x1100, 0x68, 0x40, 0x18) ver1.1'", "r");
		if (!pipe) {
			settings_file = "/appconfigs/system.json";
		} else {
			char buffer[128];
			int flash_detected = 0;
			
			while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
				if (strstr(buffer, "[FSP] Flash is detected (0x1100, 0x68, 0x40, 0x18) ver1.1") != NULL) {
					flash_detected = 1;
					break;
				}
			}
			
			pclose(pipe);
			
			if (flash_detected) {
				settings_file = "/mnt/SDCARD/system.json";
			} else {
				settings_file = "/appconfigs/system.json";
			}
		}
	}
	
	// Get brightness and volume in MIYOO MINI
	cJSON* request_json = NULL;
	cJSON* itemBrightness;
	cJSON* itemVol;
	
	char *request_body = load_file(settings_file);
	request_json = cJSON_Parse(request_body);
	itemBrightness = cJSON_GetObjectItem(request_json, "brightness");
	itemVol = cJSON_GetObjectItem(request_json, "vol");
	int brightness = cJSON_GetNumberValue(itemBrightness);
	int vol = cJSON_GetNumberValue(itemVol);
	
	cJSON_Delete(request_json);
	free(request_body);
	
	// set brightness
	int fb = open("/sys/class/pwm/pwmchip0/pwm0/duty_cycle", O_WRONLY);
	if (fb >= 0) {
		dprintf(fb, "%d", brightness * 10);
		close(fb);
	}

	// set volume
	int fv = open("/dev/mi_ao", O_RDWR);
	if (fv >= 0) {
		int buf2[] = {0, 0};
		uint64_t buf1[] = {sizeof(buf2), (uintptr_t)buf2};
						
		// Get the current volume
		ioctl(fv, MI_AO_GETVOLUME, buf1);
		int recent_volume = buf2[1];
		
		buf2[1] = (vol * 3) - 60;
						
		if (buf2[1] != recent_volume) 
			ioctl(fv, MI_AO_SETVOLUME, buf1);
	}
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
		strcpy(scope->title, title);
	}
}

int main( int argc, char** argv )
{
	init();
	
	bool by_mem = false;
	const char* path = "test.nsf";

	for ( int i = 1; i < argc; ++i )
	{
		if ( SDL_strcmp( "-m", argv[i] ) == 0 )
			by_mem = true;
		else
			path = argv[i];
	}

	// Load file
	handle_error( player->load_file( path, by_mem ) );
	start_track( 1, path );
	
	// Main loop
	int track = 1;
	double tempo = 1.0;
	bool running = true;
	double stereo_depth = 0.0;
	bool accurate = false;
	bool echo_disabled = false;
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
				case SDLK_RSUPER: // vol button MMP
					if (fd >= 0) {
						int buf2[] = {0, 0};
						uint64_t buf1[] = {sizeof(buf2), (uintptr_t)buf2};
						
						// Get the current volume
						ioctl(fd, MI_AO_GETVOLUME, buf1);
						int recent_volume = buf2[1];
						
						// Decrease volume by 3
						buf2[1] += 3;
						
						// Clamp the volume to be within [-60, 9]
						if (buf2[1] > 9) 
							buf2[1] = 9;
						else if (buf2[1] < -60) 
							buf2[1] = -60;
						
						// Only set the volume if it's different from the recent one
						
						if (buf2[1] != recent_volume) 
							ioctl(fd, MI_AO_SETVOLUME, buf1);
					}
					break;
				
				case SDLK_DOWN: // down volumen
				case SDLK_LSUPER: // vol button MMP
					if (fd >= 0) {
						int buf2[] = {0, 0};
						uint64_t buf1[] = {sizeof(buf2), (uintptr_t)buf2};
						
						// Get the current volume
						ioctl(fd, MI_AO_GETVOLUME, buf1);
						int recent_volume = buf2[1];
						
						// Upgrade volume by 3
						buf2[1] -= 3;
						
						// Clamp the volume to be within [-60, 9]
						if (buf2[1] > 9) 
							buf2[1] = 9;
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
						
				case SDLK_RETURN: // toggle echo on/off
					echo_disabled = !echo_disabled;
					player->set_echo_disable(echo_disabled);
					if(echo_disabled)
          {
            printf( "SPC echo is disabled");
            strcpy(scope->info, "SPC echo is disabled");
          }
          else
          {
            printf( "SPC echo is enabled");
            strcpy(scope->info, "SPC echo is enabled");
          }
					//printf( "%s\n", echo_disabled ? "SPC echo is disabled" : "SPC echo is enabled" );
					fflush( stdout );
					break;
				
				case SDLK_LSHIFT: // toggle loop
					player->set_fadeout( fading_out = !fading_out );
					if(fading_out)
          {
            printf( "Will stop at track end" );
            strcpy(scope->info, "Will stop at track end");
          }
          else
          {
            printf( "Playing forever" );
            strcpy(scope->info, "Playing forever");
          }
					//printf( "%s\n", fading_out ? "Will stop at track end" : "Playing forever" );
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
    strcpy(scope->title, str);


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
