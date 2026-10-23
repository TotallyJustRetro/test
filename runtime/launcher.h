#pragma once
#include <SDL.h>
typedef struct { int play; int quit; } LauncherResult;      /* play = game index or -1 */

void launcher_init(void);
void launcher_enter(void);                                   /* (re)opened: refresh ROM states, previews */
LauncherResult launcher_frame(float dt);                     /* draw + handle the UI for one frame */
void launcher_event(const SDL_Event *e);
int  launcher_capturing(void);                               /* waiting for a key/button for a binding */
void launcher_toast(const char *msg);
void launcher_drop(const char *path);
int  launcher_current_game(void);                            /* game of the current tab (or last used) */
int  launcher_prepare(int game, char *err, size_t n);        /* load ROM + hack into the core */
void launcher_shutdown(void);
void launcher_set_renderer(SDL_Renderer *r);
