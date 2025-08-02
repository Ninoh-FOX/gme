/*

How to play game music files with Music_Player (requires SDL2 and UNRAR library)

Run the program with the path to a game music file.

Left/Right Change track
Up/Down Change tempo
Button A Play file
Button B Return to file selector
Button Y Toggle track looping (infinite playback)
Button X Pause/unpause Toggle echo processing
Button L1 Enable/disable accurate emulation
Button R1 Reset tempo and turn channels back on
Select EXIT
Start Pause/unpause
GUIDE block/unblock buttons and screen
*/

// Make ISO C99 symbols available for snprintf, define must be set before any system header includes
#define _ISOC99_SOURCE 1

#include "Music_Player.h"
#include "Audio_Scope.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "SDL.h"
#include "SDL_ttf.h"

char title[512] = "GME Music Player";

// Window size and margins for text
static const int scope_width = 640;
static const int scope_height = 480;
static const int margin_top = 65;
static const int margin_bottom = 60;
static const int scope_draw_height = scope_height - margin_top - margin_bottom;

// Global objects
static Audio_Scope* scope = nullptr;
static Music_Player* player = nullptr;
static short scope_buf[scope_width * 2];

static bool paused = false;

// SDL2 and TTF
static TTF_Font* font = nullptr;
static TTF_Font* small_font = nullptr;
static TTF_Font* big_font = nullptr;
static SDL_Window* window = nullptr;
static SDL_Renderer* renderer = nullptr;

// File browser structures
struct Entry {
    std::string name;
    bool is_dir;
};
static std::vector<Entry> entries;  // current listing
static std::string current_path = "/roms/music"; // initial root directory
static int selected_index = 0;
static bool file_selected = false;
static std::string selected_file_path;
static std::string saved_path = current_path;
static int saved_index = 0;

// Playback state variables
static int track = 1;
static double tempo = 1.0;
static double stereo_depth = 0.0;
static bool accurate = false;
static bool echo_disabled = false;
static bool fading_out = true;
static int muting_mask = 0;

// Screen off state
static bool screen_off = false;

// Loop playback modes
enum LoopMode {
    LOOP_OFF = 0,
    LOOP_ONE,
    LOOP_ALL
};
static LoopMode loop_mode = LOOP_ALL;  // Default infinite loop

// Execution states
enum RunMode { MODE_SELECTION, MODE_PLAYBACK };
static RunMode run_mode = MODE_SELECTION;

// Forward declarations
static void handle_error(const char*);
static void render_text(const char* text, int x, int y, SDL_Color color);
static bool is_directory(const std::string& path);
static bool is_valid_music(const std::string& fname);
static void list_directory(const std::string& path, bool reset_selection = true);
static void draw_file_browser();
static void on_enter_pressed();
static void start_track(int trk, const char* path);

// Hardware functions to turn display on/off
void hw_display_off(void)
{
	system("wlr-randr --output DSI-1 --off");
    FILE *f;
    if ((f = fopen("/sys/class/backlight/backlight/bl_power", "w"))) {
        fprintf(f, "1\n");
        fclose(f);
    }
}

void hw_display_on(void)
{
	system("wlr-randr --output DSI-1 --on");
    FILE *f;
    if ((f = fopen("/sys/class/backlight/backlight/bl_power", "w"))) {
        fprintf(f, "0\n");
        fclose(f);
    }
}

// Render text using SDL_ttf
static void render_text(const char* text, int x, int y, SDL_Color color)
{
    SDL_Surface* surf = TTF_RenderText_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    if (!texture) return;
    SDL_Rect rect = { x, y, 0, 0 };
    SDL_QueryTexture(texture, NULL, NULL, &rect.w, &rect.h);
    SDL_RenderCopy(renderer, texture, NULL, &rect);
    SDL_DestroyTexture(texture);
}

static void render_text_small(const char* text, int x, int y, SDL_Color color)
{
    SDL_Surface* surf = TTF_RenderText_Blended(small_font, text, color);
    if (!surf) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    if (!texture) return;
    SDL_Rect rect = { x, y, 0, 0 };
    SDL_QueryTexture(texture, NULL, NULL, &rect.w, &rect.h);
    SDL_RenderCopy(renderer, texture, NULL, &rect);
    SDL_DestroyTexture(texture);
}

static void render_text_big(const char* text, int x, int y, SDL_Color color)
{
    SDL_Surface* surf = TTF_RenderText_Blended(big_font, text, color);
    if (!surf) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    if (!texture) return;
    SDL_Rect rect = { x, y, 0, 0 };
    SDL_QueryTexture(texture, NULL, NULL, &rect.w, &rect.h);
    SDL_RenderCopy(renderer, texture, NULL, &rect);
    SDL_DestroyTexture(texture);
}

// Check if path is a directory
static bool is_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFDIR) != 0;
}

// Check if filename is a valid music extension or recognized by gme
static bool is_valid_music(const std::string& fname) {
    auto pos = fname.find_last_of('.');
    if (pos != std::string::npos) {
        std::string ext = fname.substr(pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".rsn")
            return true;
    }
    return gme_identify_extension(fname.c_str());
}

// List content of directory into entries vector
// CORREGIDO: reset_selection controla si resetear selected_index
static void list_directory(const std::string& path, bool reset_selection) {
    entries.clear();
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string fname = entry->d_name;
        if (fname == ".") continue;
        std::string full_path = path + "/" + fname;
        bool dir_flag = is_directory(full_path);
        if (dir_flag || is_valid_music(fname))
            entries.push_back({fname, dir_flag});
    }
    closedir(dir);
    // Sort directories first, then files alphabetically
    std::sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });
    
    // CORREGIDO: Solo resetear si se especifica
    if (reset_selection) {
        selected_index = 0;
    }
    
    // Validar que selected_index esté en rango
    if (selected_index >= (int)entries.size()) {
        selected_index = (int)entries.size() - 1;
    }
    if (selected_index < 0) {
        selected_index = 0;
    }
}

// Draw the file browser screen
static void draw_file_browser() {
    SDL_SetRenderDrawColor(renderer, 0,0,0,255);
    SDL_RenderClear(renderer);
    SDL_Color white = {255,255,255,255};
    SDL_Color highlight = {255,255,0,255};
    SDL_Color dir_color = {0,255,255,255};

    int y = 5;
    int line_height = TTF_FontLineSkip(font);
    int max_lines = (scope_height - 40)/line_height;

    char buf[512];
    snprintf(buf, sizeof(buf), "Current path: %s", current_path.c_str());
    render_text(buf, 10, y, white);
    y += line_height + 5;

    int total_entries = (int)entries.size();

    int scroll_start = 0;
    if (total_entries > max_lines) {
        if (selected_index < max_lines/2)
            scroll_start = 0;
        else if (selected_index > total_entries - max_lines/2)
            scroll_start = total_entries - max_lines;
        else
            scroll_start = selected_index - max_lines/2;

        if (scroll_start < 0) scroll_start = 0;
    }

    int scroll_end = scroll_start + max_lines;
    if (scroll_end > total_entries) scroll_end = total_entries;

    int draw_y = y;
    for (int i = scroll_start; i < scroll_end; ++i) {
        SDL_Color color = (selected_index == i) ? highlight : (entries[i].is_dir ? dir_color : white);
        std::string textline = entries[i].is_dir ? "[DIR] " + entries[i].name : entries[i].name;
        render_text(textline.c_str(), 10, draw_y, color);
        draw_y += line_height;
    }

    SDL_RenderPresent(renderer);
}

// Handle enter key (or button A) pressed on the browser
static void on_enter_pressed() {
    if (selected_index < 0 || selected_index >= (int)entries.size()) return;

    const Entry& e = entries[selected_index];
    if (e.is_dir) {
        current_path += (current_path == "/" ? "" : "/") + e.name;
        list_directory(current_path, true); // Reset selection cuando navegas a nueva carpeta
    } else {
        saved_path = current_path;
        saved_index = selected_index;

        selected_file_path = current_path + (current_path == "/" ? "" : "/") + e.name;
        file_selected = true;
        run_mode = MODE_PLAYBACK;

        handle_error(player->load_file(selected_file_path.c_str(), false));
        track = 1;
        start_track(track, selected_file_path.c_str());
        paused = false;
    }
}

// Start playing a given track
static void start_track(int trk, const char* path)
{
    paused = false;
    handle_error(player->start_track(trk - 1));
    track = trk;
    long seconds = player->track_info().length / 1000;
    const char* game = player->track_info().game;
    if (!*game) {
        game = strrchr(path, '\\');
        if (!game)
            game = strrchr(path, '/');
        if (!game)
            game = path;
        else
            game++;
    }
    char title[512];
    snprintf(title, sizeof(title), "%s: %d/%d %s (%ld:%02ld)",
             game, track, player->track_count(),
             player->track_info().song,
             seconds / 60, seconds % 60);

    SDL_SetWindowTitle(window, title);
    player->set_stereo_depth(stereo_depth);
}

// Clear the top and bottom text areas by filling with black
static void clear_text_areas(SDL_Renderer* renderer) {
    SDL_Rect top_bar = { 0, 0, scope_width, margin_top };
    SDL_Rect bottom_bar = { 0, scope_height - margin_bottom, scope_width, margin_bottom };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &top_bar);
    SDL_RenderFillRect(renderer, &bottom_bar);
}

int main(int /*argc*/, char** /*argv*/)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
        return 1;
    if (TTF_Init() < 0)
        return 1;

    atexit(SDL_Quit);
    atexit(TTF_Quit);

    window = SDL_CreateWindow(title,
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              scope_width, scope_height, SDL_WINDOW_SHOWN);
    if (!window) handle_error("Failed to create SDL window");

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) handle_error("Failed to create SDL renderer");

    font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 24);
    if (!font) {
        font = TTF_OpenFont("DejaVuSans.ttf", 24);
        if (!font) handle_error("Failed to load TTF font");
    }
    
    big_font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 28);
    if (!big_font) {
        big_font = TTF_OpenFont("DejaVuSans.ttf", 28);
        if (!big_font) handle_error("Failed to load TTF font");
    }
    
    small_font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 22);
    if (!small_font) {
        small_font = TTF_OpenFont("DejaVuSans.ttf", 22);
        if (!small_font) handle_error("Failed to load small TTF font");
    }

    loop_mode = LOOP_ALL;
    player = new Music_Player();
    if (!player) handle_error("Out of memory Music_Player");

    handle_error(player->init());
    player->set_scope_buffer(scope_buf, scope_width * 2);

    bool running = true;
    SDL_GameController* gamepad = nullptr;
    if (SDL_NumJoysticks() > 0)
        gamepad = SDL_GameControllerOpen(0);

    // Cargar directorio inicial
    list_directory(current_path, true);

    while (running) {
        SDL_Delay(1000 / 100);

        if (run_mode == MODE_SELECTION) {
            file_selected = false;
            // CORREGIDO: NO llamar list_directory aquí para no resetear

            while (running && run_mode == MODE_SELECTION) {
                draw_file_browser();

                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (screen_off) {
                        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                            hw_display_on();
                            screen_off = false;
                            draw_file_browser();
                        }
                        continue;
                    }
                    if (e.type == SDL_QUIT) {
                        running = false;
                        break;
                    } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                        switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:
                            selected_index--;
                            if (selected_index < 0)
                                selected_index = (int)entries.size() - 1;
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                            selected_index++;
                            if (selected_index >= (int)entries.size())
                                selected_index = 0;
                            break;
                        case SDL_CONTROLLER_BUTTON_A:
                            on_enter_pressed();
                            if (file_selected) {
                                run_mode = MODE_PLAYBACK;
                            }
                            break;
                        case SDL_CONTROLLER_BUTTON_B:
                            if (run_mode == MODE_SELECTION) {
                                if (current_path != "/roms/music") {
                                    size_t pos = current_path.find_last_of('/');
                                    if (pos == std::string::npos || current_path == "/roms/music") {
                                        current_path = "/roms/music";
                                    } else {
                                        current_path = current_path.substr(0, pos);
                                        if (current_path.empty())
                                            current_path = "/roms/music";
                                    }
                                    list_directory(current_path, true); // Reset selection al subir directorio
                                }
                            }
                            break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: // L - page up
                            if (run_mode == MODE_SELECTION) {
                                selected_index -= 10;
                                if (selected_index < 0) selected_index = 0;
                            }
                            break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: // R - page down
                            if (run_mode == MODE_SELECTION) {
                                selected_index += 10;
                                if (selected_index >= (int)entries.size()) 
                                    selected_index = (int)entries.size() - 1;
                            }
                            break;
                        case SDL_CONTROLLER_BUTTON_GUIDE:
                            if (!screen_off) {
                                hw_display_off();
                                screen_off = true;
                            }
                            break;
                        case SDL_CONTROLLER_BUTTON_BACK:
                            running = false;
                            break;
                        }
                    }
                }

                if (!running) break;
            }
        }

        if (run_mode == MODE_PLAYBACK) {
            if (!scope) {
                scope = new Audio_Scope();
                if (!scope) handle_error("Out of memory Audio_Scope");
                // Initialize scope with reduced height to allow top and bottom margins for text
                std::string err_msg = scope->init(scope_width, scope_draw_height);
                if (!err_msg.empty()) handle_error(err_msg.c_str());
            }

            handle_error(player->load_file(selected_file_path.c_str(), false));
            start_track(1, selected_file_path.c_str());
            paused = false;
            track = 1;

            while (running && run_mode == MODE_PLAYBACK) {
                if (!screen_off && scope) {

                    // Clear entire screen
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                    SDL_RenderClear(renderer);

                    // Draw scope only in the center area with top and bottom margins
                    SDL_Rect scope_area = { 0, margin_top, scope_width, scope_draw_height };
                    SDL_RenderSetViewport(renderer, &scope_area);
                    scope->draw(scope_buf, scope_width, 2);
                    SDL_RenderSetViewport(renderer, NULL);

                    // Clear text areas top and bottom
                    clear_text_areas(renderer);

                    // Draw top text: title, track info and time
                    SDL_Color green = {0, 255, 0, 255};

                    char title[256];
                    snprintf(title, sizeof(title), "%s", player->track_info().game);
                    render_text_big(title, 10, 10, green);

                    char trackinfo[256];
                    long secs = player->track_info().length / 1000;
                    snprintf(trackinfo, sizeof(trackinfo), "Track %d/%d: %s (%ld:%02ld)",
                             track, player->track_count(), player->track_info().song, secs / 60, secs % 60);
                    render_text(trackinfo, 10, 36, green);

                    // Draw bottom right text: loop mode, tempo, pause status, controls info
                    SDL_Color orange = {255, 165, 0, 255};
                    
                    int info_x = 10;
                    int info_y = scope_height - margin_bottom + 5;
                    
                    int x = info_x;
                    int y = info_y;
                    int w = 0, h = 0;
                    
                    // Texto fijo "Loop: "
                    render_text("Loop:", x, y, green);
                    TTF_SizeText(font, "Loop:", &w, &h);
                    x += w;
                    
                    const char* loop_val = "";
                    switch (loop_mode) {
                        case LOOP_OFF: loop_val = "OFF"; break;
                        case LOOP_ONE: loop_val = "ONE"; break;
                        case LOOP_ALL: loop_val = "ALL"; break;
                    }
                    render_text(loop_val, x, y, orange);
                    TTF_SizeText(font, loop_val, &w, &h);
                    x += w;
                    
                    render_text(" Tempo:", x, y, green);
                    TTF_SizeText(font, " Tempo:", &w, &h);
                    x += w;
                    
                    char tempo_str[16];
                    snprintf(tempo_str, sizeof(tempo_str), "%.1f", tempo);
                    render_text(tempo_str, x, y, orange);
                    TTF_SizeText(font, tempo_str, &w, &h);
                    x += w;
                    
                    render_text(" Echo:", x, y, green);
                    TTF_SizeText(font, " Echo:", &w, &h);
                    x += w;
                    
                    const char* echo_str = echo_disabled ? "OFF" : "ON";
                    render_text(echo_str, x, y, orange);
                    TTF_SizeText(font, echo_str, &w, &h);
                    x += w;
                    
                    render_text(" Stereo:", x, y, green);
                    TTF_SizeText(font, " Stereo:", &w, &h);
                    x += w;

                    char stereo_str[16];
                    snprintf(stereo_str, sizeof(stereo_str), "%.1f", stereo_depth);
                    render_text(stereo_str, x, y, orange);
                    TTF_SizeText(font, stereo_str, &w, &h);
                    x += w;
                    
                    render_text(" ", x, y, green);
                    TTF_SizeText(font, " ", &w, &h);
                    x += w;
                    
                    if (paused) {
                        const char* paused_str = "[PAUSED]";
                        render_text(paused_str, x, y, orange);
                        TTF_SizeText(font, paused_str, &w, &h);
                        x += w;
                    }
                    
                    render_text_small("A:Str B:Back Y:Loop ST:Pause X:Echo L:Accu R:Res SE:Exit", info_x, info_y + 30, green);

                    // Present everything
                    SDL_RenderPresent(renderer);
                }

                // Playback logic and event handling ...

                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (screen_off) {
                        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                            hw_display_on();
                            screen_off = false;
                        }
                        continue;
                    }
                    if (e.type == SDL_QUIT) {
                        running = false;
                        break;
                    }
                    else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                        switch (e.cbutton.button) {
                            case SDL_CONTROLLER_BUTTON_GUIDE:
                                hw_display_off();
                                screen_off = true;
                                break;
                            case SDL_CONTROLLER_BUTTON_B:
                                if (run_mode == MODE_PLAYBACK) {
                                    player->stop();
                                    if (scope) {
                                        delete scope;
                                        scope = nullptr;
                                    }
                                    run_mode = MODE_SELECTION;
                                    paused = false;
                                    current_path = saved_path;
                                    // CORREGIDO: NO resetear selected_index aquí
                                    list_directory(current_path, false); // false = no resetear selección
                                    selected_index = saved_index;
                                    draw_file_browser();
                                }
                                break;
                            case SDL_CONTROLLER_BUTTON_A:
                                stereo_depth += 0.2;
                                if (stereo_depth > 1.0)
                                stereo_depth = 0.0;
                                player->set_stereo_depth(stereo_depth);
                                break;
                            case SDL_CONTROLLER_BUTTON_Y:
                                loop_mode = static_cast<LoopMode>((loop_mode + 1) % 3);
                                {
                                    const char* mode_str = nullptr;
                                    switch (loop_mode) {
                                        case LOOP_OFF: mode_str = "Loop OFF"; break;
                                        case LOOP_ONE: mode_str = "Loop: 1 track"; break;
                                        case LOOP_ALL: mode_str = "Infinite loop"; break;
                                    }
                                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Loop Mode", mode_str, window);
                                }
                                break;
                            case SDL_CONTROLLER_BUTTON_X:
                                echo_disabled = !echo_disabled;
                                player->set_echo_disable(echo_disabled);
                                break;
                            case SDL_CONTROLLER_BUTTON_START:
                                paused = !paused;
                                player->pause(paused);
                                break;
                            case SDL_CONTROLLER_BUTTON_BACK:
                                running = false;
                                break;
                            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                                if (!paused) {
                                    if (player->track_count() == 1) {
                                        int curr = -1;
                                        for (size_t i = 0; i < entries.size(); ++i) {
                                            std::string path = current_path + (current_path == "/" ? "" : "/") + entries[i].name;
                                            if (path == selected_file_path) {
                                                curr = (int)i;
                                                break;
                                            }
                                        }
                                        if (curr != -1) {
                                            int next = curr;
                                            do {
                                                next--;
                                                if (next < 0)
                                                    next = entries.size() - 1;    // wrap-around
                                            } while (entries[next].is_dir || !is_valid_music(entries[next].name.c_str()) || next == curr);
                                            selected_file_path = current_path + (current_path == "/" ? "" : "/") + entries[next].name;
                                            handle_error(player->load_file(selected_file_path.c_str(), false));
                                            start_track(1, selected_file_path.c_str());
                                        }
                                    } else {
                                        if (track > 1)
                                            track--;
                                        start_track(track, selected_file_path.c_str());
                                    }
                                }
                                break;

                            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                                if (!paused) {
                                    if (player->track_count() == 1) {
                                        int curr = -1;
                                        for (size_t i = 0; i < entries.size(); ++i) {
                                            std::string path = current_path + (current_path == "/" ? "" : "/") + entries[i].name;
                                            if (path == selected_file_path) {
                                                curr = (int)i;
                                                break;
                                            }
                                        }
                                        if (curr != -1) {
                                            int next = curr;
                                            do {
                                                next++;
                                                if (next >= (int)entries.size())
                                                    next = 0;    // wrap-around
                                            } while (entries[next].is_dir || !is_valid_music(entries[next].name.c_str()) || next == curr);
                                            selected_file_path = current_path + (current_path == "/" ? "" : "/") + entries[next].name;
                                            handle_error(player->load_file(selected_file_path.c_str(), false));
                                            start_track(1, selected_file_path.c_str());
                                        }
                                    } else {
                                        if (track < player->track_count())
                                            track++;
                                        start_track(track, selected_file_path.c_str());
                                    }
                                }
                                break;

                            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                                accurate = !accurate;
                                player->enable_accuracy(accurate);
                                break;
                            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                                tempo = 1.0;
                                muting_mask = 0;
                                player->set_tempo(tempo);
                                player->mute_voices(muting_mask);
                                break;
                            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                                tempo -= 0.1;
                                if (tempo < 0.1)
                                    tempo = 0.1;
                                player->set_tempo(tempo);
                                break;
                            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                                tempo += 0.1;
                                if (tempo > 2.0)
                                    tempo = 2.0;
                                player->set_tempo(tempo);
                                break;
                        }
                    }
                }
                
                const Sint16 l2 = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
                const Sint16 r2 = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                const Sint16 trigger_threshold = 16000;
                static bool l2_prev = false, r2_prev = false;
                
                if (screen_off) {
                    if (l2 > trigger_threshold && !l2_prev && !paused) {
                        if (player->track_count() == 1) {
                            int curr = -1;
                            for (size_t i = 0; i < entries.size(); ++i) {
                                std::string path = current_path + (current_path == "/" ? "" : "/") + entries[i].name;
                                if (path == selected_file_path) {
                                    curr = (int)i;
                                    break;
                                }
                            }
                            if (curr != -1) {
                                int next = curr;
                                do {
                                    next--;
                                    if (next < 0)
                                        next = entries.size() - 1;
                                } while (entries[next].is_dir || !is_valid_music(entries[next].name.c_str()) || next == curr);
                
                                selected_file_path = current_path + (current_path == "/" ? "" : "/") + entries[next].name;
                                handle_error(player->load_file(selected_file_path.c_str(), false));
                                start_track(1, selected_file_path.c_str());
                            }
                        } else {
                            if (track > 1)
                                track--;
                            start_track(track, selected_file_path.c_str());
                        }
                        l2_prev = true;
                    } else if (l2 <= trigger_threshold) {
                        l2_prev = false;
                    }
                
                    if (r2 > trigger_threshold && !r2_prev && !paused) {
                        if (player->track_count() == 1) {
                            int curr = -1;
                            for (size_t i = 0; i < entries.size(); ++i) {
                                std::string path = current_path + (current_path == "/" ? "" : "/") + entries[i].name;
                                if (path == selected_file_path) {
                                    curr = (int)i;
                                    break;
                                }
                            }
                            if (curr != -1) {
                                int next = curr;
                                do {
                                    next++;
                                    if (next >= (int)entries.size())
                                        next = 0;
                                } while (entries[next].is_dir || !is_valid_music(entries[next].name.c_str()) || next == curr);
                
                                selected_file_path = current_path + (current_path == "/" ? "" : "/") + entries[next].name;
                                handle_error(player->load_file(selected_file_path.c_str(), false));
                                start_track(1, selected_file_path.c_str());
                            }
                        } else {
                            if (track < player->track_count())
                                track++;
                            start_track(track, selected_file_path.c_str());
                        }
                        r2_prev = true;
                    } else if (r2 <= trigger_threshold) {
                        r2_prev = false;
                    }
                }

                if (!paused && player->track_ended()) {
                    if (loop_mode == LOOP_OFF) {
                        if (track >= player->track_count()) {
                            paused = true;
                            player->pause(true);
                        } else {
                            ++track;
                            start_track(track, selected_file_path.c_str());
                        }
                    }
                    else if (loop_mode == LOOP_ONE) {
                        start_track(track, selected_file_path.c_str());
                    }
                    else if (loop_mode == LOOP_ALL) {
                        if (player->track_count() == 1) {
                            int curr = -1;
                            for (size_t i = 0; i < entries.size(); ++i) {
                                std::string path = current_path + (current_path == "/" ? "" : "/") + entries[i].name;
                                if (path == selected_file_path) {
                                    curr = (int)i;
                                    break;
                                }
                            }
                            if (curr != -1) {
                                int next = curr;
                                do {
                                    next++;
                                    if (next >= (int)entries.size())
                                        next = 0; // wrap-around
                                } while (entries[next].is_dir || !is_valid_music(entries[next].name.c_str()) || next == curr);
                    
                                selected_file_path = current_path + (current_path == "/" ? "" : "/") + entries[next].name;
                                handle_error(player->load_file(selected_file_path.c_str(), false));
                                track = 1;
                                start_track(track, selected_file_path.c_str());
                            }
                        } else {
                            ++track;
                            if (track > player->track_count())
                                track = 1;
                            start_track(track, selected_file_path.c_str());
                        }
                    }
                }
            }
        }
    }

    if (gamepad) SDL_GameControllerClose(gamepad);
    if (font) TTF_CloseFont(font);
    if (small_font) TTF_CloseFont(small_font);
    if (big_font) TTF_CloseFont(big_font);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    delete player;
    if (scope) {
        delete scope;
        scope = nullptr;
    }

    return 0;
}

// Error handling helper
static void handle_error(const char* error)
{
    if (error) {
        fprintf(stderr, "Error: %s\n", error);
        if (scope)
            scope->set_caption(error);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", error, nullptr);
        exit(EXIT_FAILURE);
    }
}
