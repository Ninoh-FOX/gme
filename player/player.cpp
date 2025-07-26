/* How to play game music files with Music_Player (requires SDL2 and UNRAR library)

Run program with path to a game music file.

Left/Right  Change track
Up/Down     tempo
Button A    Play file
Button B    Back to files selector
Button Y    Toggle track looping (infinite playback)
Button X    Pause/unpauseToggle echo processing
Button L1   Enable/disable accurate emulation
Button R1   Reset tempo and turn channels back on

Select      EXIT
start       Pause/unpause */

// Make ISO C99 symbols available for snprintf, define must be set before any
// system header includes

#define _ISOC99_SOURCE 1

#include "Music_Player.h"
#include "Audio_Scope.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <cctype>

// Tamaño ventana
static const int scope_width = 640;
static const int scope_height = 480;

// Objetos globales
static Audio_Scope* scope = nullptr;
static Music_Player* player = nullptr;
static short scope_buf[scope_width * 2];
static bool paused = false;

// SDL2 y TTF
static TTF_Font* font = nullptr;
static SDL_Window* window = nullptr;
static SDL_Renderer* renderer = nullptr;

// Variables para selección de archivos y directorios
struct Entry {
    std::string name;
    bool is_dir;
};
static std::vector<Entry> entries;           // Listado actual
static std::string current_path = "/roms/music";  // Directorio raíz inicial
static int selected_index = 0;                // Índice seleccionado en lista
static bool file_selected = false;
static std::string selected_file_path;

// Variables estado reproducción
static int track = 1;
static double tempo = 1.0;
static double stereo_depth = 0.0;
static bool accurate = false;
static bool echo_disabled = false;
static bool fading_out = true;
static int muting_mask = 0;

// Variable para estado pantalla apagada
static bool screen_off = false;

// Modo loop reproducción
enum LoopMode {
    LOOP_OFF = 0,     // Sin repetición
    LOOP_ONE,         // Repetir solo pista actual
    LOOP_ALL          // Loop infinito (continuo)
};
static LoopMode loop_mode = LOOP_ALL;  // Por defecto loop continuo

// Estados de ejecución
enum RunMode { MODE_SELECTION, MODE_PLAYBACK };
static RunMode run_mode = MODE_SELECTION;

// Prototipos funciones
static void handle_error(const char*);
static void render_text(const char* text, int x, int y, SDL_Color color);
static bool is_directory(const std::string& path);
static bool is_valid_music(const std::string& fname);
static void list_directory(const std::string& path);
static void draw_file_browser();
static void on_enter_pressed();
static void start_track(int trk, const char* path);

// Funciones hardware para apagar/encender pantalla
void hw_display_off(void)
{
    FILE *f;
    if ((f = fopen("/sys/class/backlight/backlight/bl_power", "w"))) {
        fprintf(f, "1\n");
        fclose(f);
    }
}

void hw_display_on(void)
{
    FILE *f;
    if ((f = fopen("/sys/class/backlight/backlight/bl_power", "w"))) {
        fprintf(f, "0\n");
        fclose(f);
    }
}

// Implementación funciones

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

static bool is_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFDIR) != 0;
}

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

static void list_directory(const std::string& path) {
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

    std::sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });
    selected_index = 0;
}

static void draw_file_browser()
{
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color highlight = {255, 255, 0, 255};
    SDL_Color dir_color = {0, 255, 255, 255};

    int y = 5;
    int line_height = TTF_FontLineSkip(font);
    int max_lines = (scope_height - 40) / line_height;

    char buf[512];
    snprintf(buf, sizeof(buf), "Ruta actual: %s", current_path.c_str());
    render_text(buf, 10, y, white);
    y += line_height + 5;

    int total_entries = entries.size() + (current_path != "/" ? 1 : 0);

    int scroll_start = 0;
    int scroll_end = total_entries;

    if (total_entries > max_lines) {
        if (selected_index < max_lines / 2)
            scroll_start = 0;
        else if (selected_index > total_entries - max_lines / 2)
            scroll_start = total_entries - max_lines;
        else
            scroll_start = selected_index - max_lines / 2;

        if (scroll_start < 0) scroll_start = 0;
        scroll_end = scroll_start + max_lines;
        if (scroll_end > total_entries) scroll_end = total_entries;
    }

    int draw_y = y;
    for (int i = scroll_start; i < scroll_end; ++i) {
        SDL_Color color;
        std::string textline;
        if (current_path != "/" && i == 0) {
            color = (selected_index == i) ? highlight : white;
            textline = "UP DIR";
        } else {
            int real_idx = i - (current_path != "/" ? 1 : 0);
            const Entry& e = entries[real_idx];
            color = (selected_index == i) ? highlight : (e.is_dir ? dir_color : white);
            textline = e.is_dir ? "[DIR] " + e.name : e.name;
        }
        render_text(textline.c_str(), 10, draw_y, color);
        draw_y += line_height;
    }

    if (total_entries > max_lines) {
        int bar_height = std::max(10, max_lines * max_lines / total_entries);
        int bar_y = y + (selected_index * (scope_height - y - 10) / total_entries);
        SDL_Rect scrollbar = {scope_width - 8, bar_y, 6, bar_height};
        SDL_SetRenderDrawColor(renderer, 128, 128, 128, 192);
        SDL_RenderFillRect(renderer, &scrollbar);
    }

    SDL_RenderPresent(renderer);
}

static void on_enter_pressed() {
    if (current_path != "/" && selected_index == 0) {
        size_t pos = current_path.find_last_of('/');
        if (pos == std::string::npos || current_path == "/")
            current_path = "/";
        else {
            current_path = current_path.substr(0, pos);
            if (current_path.empty()) current_path = "/";
        }
        list_directory(current_path);
    } else {
        int real_idx = selected_index - (current_path != "/" ? 1 : 0);
        if (real_idx < 0 || real_idx >= (int)entries.size())
            return;
        const Entry& e = entries[real_idx];

        if (e.is_dir) {
            current_path += (current_path == "/" ? "" : "/") + e.name;
            list_directory(current_path);
        } else {
            selected_file_path = current_path + (current_path == "/" ? "" : "/") + e.name;
            file_selected = true;
        }
    }
}

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
}

int main(int /*argc*/, char** /*argv*/)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
        return 1;

    if (TTF_Init() < 0)
        return 1;

    atexit(SDL_Quit);
    atexit(TTF_Quit);

    window = SDL_CreateWindow("Game Music Player - Selección de Archivo",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        scope_width, scope_height, SDL_WINDOW_SHOWN);
    if (!window) handle_error("No se pudo crear ventana SDL");

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) handle_error("No se pudo crear renderizador SDL");

    font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 26);
    if (!font) {
        font = TTF_OpenFont("DejaVuSans.ttf", 26);
        if (!font) handle_error("No se pudo cargar fuente TTF");
    }

    loop_mode = LOOP_ALL; // modo loop continuo por defecto

    player = new Music_Player();
    if (!player) handle_error("Out of memory Music_Player");
    handle_error(player->init());
    player->set_scope_buffer(scope_buf, scope_width * 2);

    bool running = true;
    SDL_GameController* gamepad = nullptr;
    if (SDL_NumJoysticks() > 0)
        gamepad = SDL_GameControllerOpen(0);

    while (running) {
        SDL_Delay( 1000 / 100 );
        if (run_mode == MODE_SELECTION) {
            file_selected = false;
            current_path = "/roms/music";
            selected_index = 0;
            list_directory(current_path);

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
                                    selected_index = (current_path != "/" ? 1 : 0) + (int)entries.size() - 1;
                                break;
                            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                                selected_index++;
                                if (selected_index > (current_path != "/" ? 1 : 0) + (int)entries.size() - 1)
                                    selected_index = 0;
                                break;
                            case SDL_CONTROLLER_BUTTON_A:
                                on_enter_pressed();
                                if (file_selected) {
                                    run_mode = MODE_PLAYBACK;
                                }
                                break;
                            case SDL_CONTROLLER_BUTTON_B:
                                // Ya estamos en selector, solo reiniciar navegador limpio
                                selected_index = 0;
                                current_path = "/roms/music";
                                list_directory(current_path);
                                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                                SDL_RenderClear(renderer);
                                draw_file_browser();
                                break;
                            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: // L
                                if (run_mode == MODE_SELECTION) {
                                    selected_index -= 10;
                                    int max_idx = (current_path != "/" ? 1 : 0) + (int)entries.size() - 1;
                                    while (selected_index < 0)
                                    selected_index += max_idx + 1;
                                    selected_index = selected_index % (max_idx + 1);
                                }
                                break;
                            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: // R
                                if (run_mode == MODE_SELECTION) {
                                    selected_index += 10;
                                    int max_idx = (current_path != "/" ? 1 : 0) + (int)entries.size() - 1;
                                    selected_index = selected_index % (max_idx + 1);
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
            }
        }

        if (!running) break;

        if (run_mode == MODE_PLAYBACK) {
            if (!scope) {
                scope = new Audio_Scope();
                if (!scope) handle_error("Out of memory Audio_Scope");
                std::string err_msg = scope->init(scope_width, scope_height);
                if (!err_msg.empty()) handle_error(err_msg.c_str());
            }

            handle_error(player->load_file(selected_file_path.c_str(), false));
            start_track(1, selected_file_path.c_str());

            paused = false;
            track = 1;

            while (running && run_mode == MODE_PLAYBACK) {
                if (!screen_off && scope) {
                    scope->draw(scope_buf, scope_width, 2);

                    SDL_Color white = {255, 255, 255, 255};
                    char info[256];
                    long seconds = player->track_info().length / 1000;

                    const char* loop_str = "";
                    switch (loop_mode) {
                        case LOOP_OFF: loop_str = "Loop: OFF"; break;
                        case LOOP_ONE: loop_str = "Loop: 1"; break;
                        case LOOP_ALL: loop_str = "Loop: ∞"; break;
                    }

                    snprintf(info, sizeof(info),
                        "Track %d/%d: %s (%ld:%02ld) Tempo: %.1f %s %s",
                        track, player->track_count(), player->track_info().song,
                        seconds / 60, seconds % 60,
                        tempo,
                        paused ? "[PAUSA]" : "",
                        loop_str);

                    render_text(info, 10, scope_height - 30, white);
                    SDL_RenderPresent(renderer);
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
                    } else if (loop_mode == LOOP_ONE) {
                        start_track(track, selected_file_path.c_str());
                    } else if (loop_mode == LOOP_ALL) {
                        ++track;
                        if (track > player->track_count())
                            track = 1;
                        start_track(track, selected_file_path.c_str());
                    }
                }

                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (screen_off) {
                        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                            hw_display_on();
                            screen_off = false;

                            if (scope)
                                scope->draw(scope_buf, scope_width, 2);

                            SDL_Color white = {255, 255, 255, 255};
                            char info[256];
                            long seconds = player->track_info().length / 1000;

                            const char* loop_str = "";
                            switch (loop_mode) {
                                case LOOP_OFF: loop_str = "Loop: OFF"; break;
                                case LOOP_ONE: loop_str = "Loop: 1"; break;
                                case LOOP_ALL: loop_str = "Loop: ∞"; break;
                            }

                            snprintf(info, sizeof(info),
                                "Track %d/%d: %s (%ld:%02ld) Tempo: %.1f %s %s",
                                track, player->track_count(), player->track_info().song,
                                seconds / 60, seconds % 60,
                                tempo,
                                paused ? "[PAUSA]" : "",
                                loop_str);

                            render_text(info, 10, scope_height - 30, white);
                            SDL_RenderPresent(renderer);
                        }
                        continue;
                    }

                    switch (e.type) {
                    case SDL_QUIT:
                        running = false;
                        break;
                    case SDL_CONTROLLERBUTTONDOWN: {
                        switch (e.cbutton.button) {
                            case SDL_CONTROLLER_BUTTON_GUIDE:
                                hw_display_off();
                                screen_off = true;
                                break;
                            case SDL_CONTROLLER_BUTTON_B:
                                player->stop();
                                if (scope) {
                                    delete scope;
                                    scope = nullptr;
                                }
                                file_selected = false;
                                run_mode = MODE_SELECTION;
                                paused = false;
                                track = 1;
                                selected_index = 0;
                                current_path = "/roms/music";
                                list_directory(current_path);
                                loop_mode = LOOP_ALL;
                                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                                SDL_RenderClear(renderer);
                                draw_file_browser();
                                break;
                            case SDL_CONTROLLER_BUTTON_Y:
                                loop_mode = static_cast<LoopMode>((loop_mode + 1) % 3);
                                {
                                    const char* mode_str = nullptr;
                                    switch (loop_mode) {
                                        case LOOP_OFF: mode_str = "Loop OFF"; break;
                                        case LOOP_ONE: mode_str = "Loop: 1 pista"; break;
                                        case LOOP_ALL: mode_str = "Loop infinito"; break;
                                    }
                                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Modo loop", mode_str, window);
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
                                if (!paused && track > 1) track--;
                                start_track(track, selected_file_path.c_str());
                                break;
                            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                                if (track < player->track_count())
                                    start_track(++track, selected_file_path.c_str());
                                break;
                            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                                accurate = !accurate;
					            player->enable_accuracy( accurate );
                                break;
                            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                                tempo = 1.0;
                                muting_mask = 0;
					            player->set_tempo( tempo );
                                player->mute_voices( muting_mask );
                                break;
                            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
					            tempo -= 0.1;
					            if ( tempo < 0.1 )
						           tempo = 0.1;
					            player->set_tempo( tempo );
					        break;
				            case SDL_CONTROLLER_BUTTON_DPAD_UP:
					            tempo += 0.1;
					            if ( tempo > 2.0 )
						           tempo = 2.0;
					            player->set_tempo( tempo );
					       break;
                        }
                        break;
                    }
                    }
                }
            }
        }
    }

    if (gamepad) SDL_GameControllerClose(gamepad);
    if (font) TTF_CloseFont(font);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    delete player;
    if (scope) {
        delete scope;
        scope = nullptr;
    }
    return 0;
}

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
